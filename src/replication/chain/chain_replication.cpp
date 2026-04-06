#include "chain_replication.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <condition_variable>
#include <queue>
#include <vector>

#include "chain.pb.h"
#include "node/node.h"

#include <google/protobuf/empty.pb.h>

using namespace std;

namespace {

string chain_node_label(const Node& node) {
    if (!node.id().empty()) {
        return node.id();
    }
    return node.self_addr().to_string();
}

struct PropagateTask {
    std::shared_ptr<chain::ChainNode::Stub> successor;
    chain::PropagateRequest req;
    std::string from_node;
};

class PropagateDispatcher {
public:
    explicit PropagateDispatcher(const std::string& tag)
        : tag_(tag) {
        size_t workers = std::thread::hardware_concurrency();
        if (workers == 0) workers = 4;
        workers = std::min<size_t>(workers, 8);

        workers_.reserve(workers);
        for (size_t i = 0; i < workers; ++i) {
            workers_.emplace_back([this]() { worker_loop(); });
        }
    }

    ~PropagateDispatcher() {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& worker : workers_) {
            if (worker.joinable()) worker.join();
        }
    }

    void enqueue(std::shared_ptr<chain::ChainNode::Stub> successor,
                 chain::PropagateRequest req,
                 std::string from_node) {
        if (!successor) {
            cerr << tag_ << " Async PROPAGATE skipped from " << from_node
                 << ": no successor stub\n";
            return;
        }

        {
            std::lock_guard<std::mutex> lk(mtx_);
            queue_.push(PropagateTask{std::move(successor), std::move(req), std::move(from_node)});
        }
        cv_.notify_one();
    }

private:
    void worker_loop() {
        while (true) {
            PropagateTask task;
            {
                std::unique_lock<std::mutex> lk(mtx_);
                cv_.wait(lk, [this]() { return stop_ || !queue_.empty(); });
                if (stop_ && queue_.empty()) return;
                task = std::move(queue_.front());
                queue_.pop();
            }

            google::protobuf::Empty ignored;
            grpc::ClientContext ctx;
            grpc::Status status = task.successor->Propagate(&ctx, task.req, &ignored);
            if (!status.ok()) {
                cerr << tag_ << " Async PROPAGATE failed from " << task.from_node
                     << " key='" << task.req.key() << "' version=" << task.req.version()
                     << ": " << status.error_message() << "\n";
            }
        }
    }

    std::string tag_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::queue<PropagateTask> queue_;
    std::vector<std::thread> workers_;
    bool stop_ = false;
};

PropagateDispatcher& chain_propagate_dispatcher() {
    static PropagateDispatcher dispatcher("[Chain]");
    return dispatcher;
}

void forward_propagate_async(std::shared_ptr<chain::ChainNode::Stub> successor,
                             chain::PropagateRequest req,
                             const std::string& from_node) {
    chain_propagate_dispatcher().enqueue(std::move(successor), std::move(req), from_node);
}

} // namespace

chain::WriteResponse ChainReplication::handle_write(const chain::WriteRequest& req, Node& node) {
    if (!node.is_head()) {
        throw runtime_error("CHAIN write rejected: node is not head");
    }

    const uint64_t version = support_.assign_next_version(req.key());
    support_.record_local_write_if_newer(req.key(), req.value(), version);

    cout << "[Chain] Head " << chain_node_label(node)
        << " accepted write key='" << req.key() << "' version=" << version
        << " (response means accepted by head; commit happens later at tail)\n";

    chain::WriteResponse resp;
    resp.set_success(true);
    resp.set_version(version);

    if (node.is_tail()) {
        support_.mark_version_committed_if_newer(req.key(), req.value(), version);
        cout << "[Chain] Single-node chain committed immediately key='" << req.key()
             << "' version=" << version << "\n";

        chain::AckRequest ack;
        ack.set_key(req.key());
        ack.set_version(version);
        ack.set_client_addr(req.client_addr());
        ack.set_request_id(req.request_id());
        support_.send_client_ack(ack);

        return resp;
    }

    chain::PropagateRequest fwd;
    fwd.set_key(req.key());
    fwd.set_value(req.value());
    fwd.set_version(version);
    fwd.set_origin_id(node.id());
    fwd.set_client_addr(req.client_addr());
    fwd.set_request_id(req.request_id());

    auto succ = support_.successor_stub();
    forward_propagate_async(succ, std::move(fwd), chain_node_label(node));

    cout << "[Chain] Head scheduled async PROPAGATE key='" << req.key() << "' version=" << version
        << "\n";
    return resp;
}

chain::ReadResponse ChainReplication::handle_read(const chain::ReadRequest& req, Node& node) {
    if (!node.is_tail()) {
        throw runtime_error("CHAIN read rejected: node is not tail");
    }

        const auto committed = support_.read_committed(req.key());
        if (!committed.found) {
        throw runtime_error("CHAIN read failed: key not found or not committed");
    }

    cout << "[Chain] Tail " << chain_node_label(node)
            << " serving committed read key='" << req.key() << "' version=" << committed.version << "\n";

    chain::ReadResponse resp;
    resp.set_key(req.key());
        resp.set_value(committed.value);
        resp.set_version(committed.version);
    return resp;
}

void ChainReplication::handle_propagate(const chain::PropagateRequest& req, Node& node) {
    cout << "[Chain] Node " << chain_node_label(node)
         << " received PROPAGATE key='" << req.key() << "' version=" << req.version() << "\n";
    support_.record_local_write_if_newer(req.key(), req.value(), req.version());

    if (node.is_tail()) {
        support_.mark_version_committed_if_newer(req.key(), req.value(), req.version());

        cout << "[Chain] Tail committed key='" << req.key() << "' version=" << req.version()
             << " and will notify client asynchronously\n";

        chain::AckRequest ack;
        ack.set_key(req.key());
        ack.set_version(req.version());
        ack.set_client_addr(req.client_addr());
        ack.set_request_id(req.request_id());
        support_.send_client_ack(ack);
        return;
    }

    auto succ = support_.successor_stub();
    chain::PropagateRequest fwd = req;
    forward_propagate_async(succ, std::move(fwd), chain_node_label(node));

    cout << "[Chain] Node " << chain_node_label(node)
         << " forwarded PROPAGATE asynchronously key='" << req.key()
         << "' version=" << req.version() << "\n";
}

void ChainReplication::handle_ack(const chain::AckRequest& req, Node& node) {
    throw runtime_error(
        "CHAIN Ack RPC is disabled: only CRAQ uses inter-node ACK propagation"
    );
}

void ChainReplication::on_config_change(Node& node) {
    support_.on_config_change(node);
}
