#include "crown_replication.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

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

void log_key_ownership(const Node& node, const string& key, const string& action) {
    const bool is_head = node.is_head_for(key);
    const bool is_tail = node.is_tail_for(key);
    cout << "[CROWN] Node " << crown_node_label(node)
         << " action=" << action
         << " key='" << key << "'"
         << " is_head_for=" << (is_head ? "true" : "false")
         << " is_tail_for=" << (is_tail ? "true" : "false")
         << " predecessor="
         << (node.predecessor().has_value() ? node.predecessor()->to_string() : "<none>")
         << " successor="
         << (node.successor().has_value() ? node.successor()->to_string() : "<none>")
         << "\n";
}

void forward_propagate_clockwise_async(std::shared_ptr<chain::ChainNode::Stub> successor,
                                       chain::PropagateRequest req,
                                       const std::string& from_node) {
    std::thread([successor = std::move(successor),
                 req = std::move(req),
                 from_node]() mutable {
        if (!successor) {
            cerr << "[CROWN] Async PROPAGATE skipped from " << from_node
                 << ": no successor stub\n";
            return;
        }

        google::protobuf::Empty ignored;
        grpc::ClientContext ctx;
        grpc::Status status = successor->Propagate(&ctx, req, &ignored);
        if (!status.ok()) {
            cerr << "[CROWN] Async PROPAGATE failed from " << from_node
                 << " key='" << req.key() << "' version=" << req.version()
                 << ": " << status.error_message() << "\n";
        }
    }).detach();
}

void notify_client_ack_async(chain::AckRequest req,
                             const std::string& from_node) {
    std::thread([req = std::move(req), from_node]() mutable {
        if (req.client_addr().empty()) {
            cerr << "[CROWN] Client ACK skipped from " << from_node
                 << ": empty client_addr\n";
            return;
        }

        auto channel = grpc::CreateChannel(req.client_addr(), grpc::InsecureChannelCredentials());
        auto client_stub = chain::ChainNode::NewStub(channel);

        google::protobuf::Empty ignored;
        grpc::ClientContext ctx;
        grpc::Status status = client_stub->Ack(&ctx, req, &ignored);
        if (!status.ok()) {
            cerr << "[CROWN] Client ACK failed from " << from_node
                 << " request_id=" << req.request_id()
                 << " client_addr='" << req.client_addr()
                 << "': " << status.error_message() << "\n";
        }
    }).detach();
}

} // namespace

chain::WriteResponse CROWNReplication::handle_write(const chain::WriteRequest& req, Node& node) {
    log_key_ownership(node, req.key(), "handle_write");

    if (!node.is_head_for(req.key())) {
        throw runtime_error("CROWN write rejected: node is not key head");
    }

    const uint64_t version = support_.assign_next_version(req.key());
    support_.record_local_write(req.key(), req.value(), version);

    cout << "[CROWN] Head accepted write key='" << req.key()
        << "' assigned version=" << version
        << " (response means accepted by head; commit happens later at key-tail)\n";

    chain::WriteResponse resp;
    resp.set_success(true);
    resp.set_version(version);

    if (node.is_tail_for(req.key())) {
       support_.mark_version_clean(req.key(), version);

        cout << "[CROWN] Single-node ownership committed key='" << req.key()
             << "' version=" << version << "\n";

        chain::AckRequest ack;
        ack.set_key(req.key());
        ack.set_version(version);
        ack.set_client_addr(req.client_addr());
        ack.set_request_id(req.request_id());
        notify_client_ack_async(std::move(ack), crown_node_label(node));

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
    forward_propagate_clockwise_async(succ, std::move(fwd), crown_node_label(node));

    cout << "[CROWN] Head scheduled async PROPAGATE clockwise key='" << req.key()
        << "' version=" << version << "\n";
    return resp;
}

chain::ReadResponse CROWNReplication::handle_read(const chain::ReadRequest& req, Node& node) {
    log_key_ownership(node, req.key(), "handle_read");

    if (!node.is_tail_for(req.key())) {
        throw runtime_error("CROWN read rejected: node is not key tail");
    }

    const auto clean = support_.read_clean(req.key());
    if (!clean.found) {
        throw runtime_error("CROWN read failed: key not found or not committed");
    }

    cout << "[CROWN] Tail serving read key='" << req.key()
         << "' CLEAN version=" << clean.version << "\n";

    chain::ReadResponse resp;
    resp.set_key(req.key());
    resp.set_value(clean.value);
    resp.set_version(clean.version);
    return resp;
}

void CROWNReplication::handle_propagate(const chain::PropagateRequest& req, Node& node) {
    log_key_ownership(node, req.key(), "handle_propagate");

    if (req.version() == 0) {
        throw runtime_error("CROWN propagate rejected: version must be non-zero");
    }

    support_.record_local_write(req.key(), req.value(), req.version());
    cout << "[CROWN] Node " << crown_node_label(node)
         << " recorded DIRTY key='" << req.key() << "' version=" << req.version() << "\n";

    if (node.is_tail_for(req.key())) {
        support_.mark_version_clean(req.key(), req.version());

        cout << "[CROWN] Key-tail committed key='" << req.key()
             << " version=" << req.version() << " and notifying client directly\n";

        chain::AckRequest ack;
        ack.set_key(req.key());
        ack.set_version(req.version());
        ack.set_client_addr(req.client_addr());
        ack.set_request_id(req.request_id());
        notify_client_ack_async(std::move(ack), crown_node_label(node));
        return;
    }

    auto succ = support_.successor_stub();
    chain::PropagateRequest fwd = req;
    forward_propagate_clockwise_async(succ, std::move(fwd), crown_node_label(node));

    cout << "[CROWN] Node " << crown_node_label(node)
         << " forwarded DIRTY PROPAGATE clockwise key='" << req.key()
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
