#include "crown_replication.h"

#include <iostream>
#include <stdexcept>
#include <string>

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

} // namespace

chain::WriteResponse CROWNReplication::handle_write(const chain::WriteRequest& req, Node& node) {
    log_key_ownership(node, req.key(), "handle_write");

    if (!node.is_head_for(req.key())) {
        throw runtime_error("CROWN write rejected: node is not key head");
    }

    const uint64_t version = support_.assign_next_version(req.key());
    cout << "[CROWN] Head accepted write key='" << req.key()
         << "' assigned version=" << version << "\n";

    support_.apply_replica_write(req.key(), req.value(), version);

    if (node.is_tail_for(req.key())) {
        support_.mark_write_committed(req.key(), version);
        support_.notify_commit(req.key(), version);

        cout << "[CROWN] Single-node ownership committed key='" << req.key()
             << "' version=" << version << "\n";

        chain::WriteResponse resp;
        resp.set_success(true);
        resp.set_version(version);
        return resp;
    }

    auto succ = support_.successor_stub();
    if (!succ) {
        throw runtime_error("CROWN write failed: no successor stub");
    }

    chain::PropagateRequest fwd;
    fwd.set_key(req.key());
    fwd.set_value(req.value());
    fwd.set_version(version);
    fwd.set_origin_id(node.id());

    google::protobuf::Empty ignored;
    grpc::ClientContext ctx;

    cout << "[CROWN] Head forwarding PROPAGATE clockwise key='" << req.key()
         << "' version=" << version << "\n";
    grpc::Status status = succ->Propagate(&ctx, fwd, &ignored);
    if (!status.ok()) {
        throw runtime_error("CROWN propagate to successor failed: " + status.error_message());
    }

    cout << "[CROWN] Head waiting for ACK key='" << req.key()
         << "' version=" << version << "\n";
    support_.wait_for_commit(req.key(), version);
    cout << "[CROWN] Head observed commit key='" << req.key()
         << "' version=" << version << "\n";

    chain::WriteResponse resp;
    resp.set_success(true);
    resp.set_version(version);
    return resp;
}

chain::ReadResponse CROWNReplication::handle_read(const chain::ReadRequest& req, Node& node) {
    log_key_ownership(node, req.key(), "handle_read");

    if (!node.is_tail_for(req.key())) {
        throw runtime_error("CROWN read rejected: node is not key tail");
    }

    const auto latest = support_.read_latest(req.key());
    if (!latest.found) {
        throw runtime_error("CROWN read failed: key not found or not committed");
    }

    cout << "[CROWN] Tail serving read key='" << req.key()
         << "' version=" << latest.version << "\n";

    chain::ReadResponse resp;
    resp.set_key(req.key());
    resp.set_value(latest.value);
    resp.set_version(latest.version);
    return resp;
}

void CROWNReplication::handle_propagate(const chain::PropagateRequest& req, Node& node) {
    log_key_ownership(node, req.key(), "handle_propagate");

    if (req.version() == 0) {
        throw runtime_error("CROWN propagate rejected: version must be non-zero");
    }

    support_.apply_replica_write(req.key(), req.value(), req.version());

    if (node.is_tail_for(req.key())) {
        support_.mark_write_committed(req.key(), req.version());
        support_.notify_commit(req.key(), req.version());

        cout << "[CROWN] Key-tail committed key='" << req.key()
             << "' version=" << req.version() << " and starting ACK counter-clockwise\n";

        if (!node.is_head_for(req.key())) {
            auto pred = support_.predecessor_stub();
            if (!pred) {
                throw runtime_error("CROWN tail ACK failed: no predecessor stub");
            }

            chain::AckRequest ack;
            ack.set_key(req.key());
            ack.set_version(req.version());

            google::protobuf::Empty ignored;
            grpc::ClientContext ctx;

            grpc::Status status = pred->Ack(&ctx, ack, &ignored);
            if (!status.ok()) {
                throw runtime_error("CROWN tail ACK to predecessor failed: " + status.error_message());
            }
        }
        return;
    }

    auto succ = support_.successor_stub();
    if (!succ) {
        throw runtime_error("CROWN propagate failed: no successor stub");
    }

    google::protobuf::Empty ignored;
    grpc::ClientContext ctx;

    cout << "[CROWN] Forwarding PROPAGATE clockwise key='" << req.key()
         << "' version=" << req.version() << "\n";
    grpc::Status status = succ->Propagate(&ctx, req, &ignored);
    if (!status.ok()) {
        throw runtime_error("CROWN propagate forward failed: " + status.error_message());
    }
}

void CROWNReplication::handle_ack(const chain::AckRequest& req, Node& node) {
    log_key_ownership(node, req.key(), "handle_ack");

    support_.mark_write_committed(req.key(), req.version());
    support_.notify_commit(req.key(), req.version());

    if (node.is_head_for(req.key())) {
        cout << "[CROWN] Key-head finalized commit key='" << req.key()
             << "' version=" << req.version() << "\n";
        return;
    }

    auto pred = support_.predecessor_stub();
    if (!pred) {
        throw runtime_error("CROWN ack forward failed: no predecessor stub");
    }

    google::protobuf::Empty ignored;
    grpc::ClientContext ctx;

    cout << "[CROWN] Forwarding ACK counter-clockwise key='" << req.key()
         << "' version=" << req.version() << "\n";
    grpc::Status status = pred->Ack(&ctx, req, &ignored);
    if (!status.ok()) {
        throw runtime_error("CROWN ack forward to predecessor failed: " + status.error_message());
    }
}

void CROWNReplication::on_config_change(Node& node) {
    support_.on_config_change(node);
}
