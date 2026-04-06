#include "crown_replication.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <limits>
#include <condition_variable>
#include <queue>
#include <vector>

#include "chain.pb.h"
#include "node/node.h"

#include <google/protobuf/empty.pb.h>

using namespace std;

namespace {

string crown_node_label(const Node& node) {
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

PropagateDispatcher& crown_propagate_dispatcher() {
    static PropagateDispatcher dispatcher("[CROWN]");
    return dispatcher;
}

int parse_origin_index_or_throw(const std::string& origin_id, int ring_size) {
    if (origin_id.empty()) {
        throw runtime_error("CROWN propagate rejected: missing origin_id");
    }

    size_t consumed = 0;
    long long parsed = 0;
    try {
        parsed = stoll(origin_id, &consumed, 10);
    } catch (const exception&) {
        throw runtime_error("CROWN propagate rejected: origin_id must be numeric");
    }

    if (consumed != origin_id.size()) {
        throw runtime_error("CROWN propagate rejected: origin_id must be numeric");
    }
    if (parsed < 0 || parsed > numeric_limits<int>::max()) {
        throw runtime_error("CROWN propagate rejected: origin_id out of range");
    }

    const int origin_index = static_cast<int>(parsed);
    if (origin_index >= ring_size) {
        throw runtime_error("CROWN propagate rejected: origin_id outside ring size");
    }
    return origin_index;
}

bool is_request_tail_node(const Node& node, const chain::PropagateRequest& req) {
    const int ring_size = node.config().crown_node_count;
    if (ring_size <= 1) {
        return true;
    }

    const int origin_index = parse_origin_index_or_throw(req.origin_id(), ring_size);
    const int tail_index = (origin_index + ring_size - 1) % ring_size;
    return node.node_index() == tail_index;
}

void forward_propagate_clockwise_async(std::shared_ptr<chain::ChainNode::Stub> successor,
                                       chain::PropagateRequest req,
                                       const std::string& from_node) {
    crown_propagate_dispatcher().enqueue(std::move(successor), std::move(req), from_node);
}

} // namespace

chain::WriteResponse CROWNReplication::handle_write(const chain::WriteRequest& req, Node& node) {
    cout << "[CROWN] Node " << crown_node_label(node)
         << " handling write key='" << req.key() << "'"
         << " (client is responsible for routing to correct head)\n";

    const uint64_t version = support_.assign_next_version(req.key());
    support_.record_local_write_if_newer(req.key(), req.value(), version);

    cout << "[CROWN] Head accepted write key='" << req.key()
        << "' assigned version=" << version
        << " (response means accepted by head; commit happens later at key-tail)\n";

    chain::WriteResponse resp;
    resp.set_success(true);
    resp.set_version(version);

    const int ring_size = node.config().crown_node_count;
    if (ring_size <= 1) {
        support_.mark_version_committed_if_newer(req.key(), req.value(), version);

        cout << "[CROWN] Single-node ownership committed key='" << req.key()
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
    fwd.set_origin_id(to_string(node.node_index()));
    fwd.set_client_addr(req.client_addr());
    fwd.set_request_id(req.request_id());

    auto succ = support_.successor_stub();
    forward_propagate_clockwise_async(succ, std::move(fwd), crown_node_label(node));

    cout << "[CROWN] Head scheduled async PROPAGATE clockwise key='" << req.key()
        << "' version=" << version << "\n";
    return resp;
}

chain::ReadResponse CROWNReplication::handle_read(const chain::ReadRequest& req, Node& node) {
    cout << "[CROWN] Node " << crown_node_label(node)
         << " handling read key='" << req.key() << "'"
         << " (client is responsible for routing to correct tail)\n";

    const auto committed = support_.read_committed(req.key());
    if (!committed.found) {
        throw runtime_error("CROWN read failed: key not found or not committed");
    }

    cout << "[CROWN] Tail serving read key='" << req.key()
            << "' committed version=" << committed.version << "\n";

    chain::ReadResponse resp;
    resp.set_key(req.key());
        resp.set_value(committed.value);
        resp.set_version(committed.version);
    return resp;
}

void CROWNReplication::handle_propagate(const chain::PropagateRequest& req, Node& node) {
    cout << "[CROWN] Node " << crown_node_label(node)
         << " handling propagate key='" << req.key() << "'"
         << " version=" << req.version()
         << " origin_id=" << req.origin_id() << "\n";

    if (req.version() == 0) {
        throw runtime_error("CROWN propagate rejected: version must be non-zero");
    }

    support_.record_local_write_if_newer(req.key(), req.value(), req.version());
    cout << "[CROWN] Node " << crown_node_label(node)
         << " recorded propagated write key='" << req.key() << "' version=" << req.version() << "\n";

    if (is_request_tail_node(node, req)) {
        support_.mark_version_committed_if_newer(req.key(), req.value(), req.version());

        cout << "[CROWN] Key-tail committed key='" << req.key()
             << " version=" << req.version() << " and notifying client directly\n";

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
    forward_propagate_clockwise_async(succ, std::move(fwd), crown_node_label(node));

    cout << "[CROWN] Node " << crown_node_label(node)
            << " forwarded PROPAGATE clockwise key='" << req.key()
         << "' version=" << req.version() << "\n";
}

void CROWNReplication::handle_ack(const chain::AckRequest& req, Node& node) {
    throw runtime_error(
        "CROWN Ack RPC is disabled: only CRAQ uses inter-node ACK propagation"
    );
}

void CROWNReplication::on_config_change(Node& node) {
    support_.on_config_change(node);
}
