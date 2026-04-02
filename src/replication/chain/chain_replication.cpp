#include "chain_replication.h"

#include <iostream>
#include <stdexcept>
#include <string>

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

} // namespace

chain::WriteResponse ChainReplication::handle_write(const chain::WriteRequest& req, Node& node) {
    if (!node.is_head()) {
        throw runtime_error("CHAIN write rejected: node is not head");
    }

    const uint64_t version = support_.assign_next_version(req.key());
    cout << "[Chain] Head " << chain_node_label(node)
         << " accepted client write key='" << req.key() << "' version=" << version << "\n";

    support_.apply_replica_write(req.key(), req.value(), version);

    if (node.is_tail()) {
        support_.mark_write_committed(req.key(), version);
        support_.notify_commit(req.key(), version);

        cout << "[Chain] Single-node chain committed key='" << req.key()
             << "' version=" << version << "\n";

        chain::WriteResponse resp;
        resp.set_success(true);
        resp.set_version(version);
        return resp;
    }

    auto succ = support_.successor_stub();
    if (!succ) {
        throw runtime_error("CHAIN write failed: head has no successor stub");
    }

    chain::PropagateRequest fwd;
    fwd.set_key(req.key());
    fwd.set_value(req.value());
    fwd.set_version(version);
    fwd.set_origin_id(node.id());

    google::protobuf::Empty ignored;
    grpc::ClientContext ctx;

    cout << "[Chain] Forwarding PROPAGATE key='" << req.key() << "' version=" << version
         << " to successor\n";
    grpc::Status status = succ->Propagate(&ctx, fwd, &ignored);
    if (!status.ok()) {
        throw runtime_error("CHAIN propagate to successor failed: " + status.error_message());
    }

    cout << "[Chain] Head waiting for ACK key='" << req.key() << "' version=" << version << "\n";
    support_.wait_for_commit(req.key(), version);
    cout << "[Chain] Head observed commit key='" << req.key() << "' version=" << version << "\n";

    chain::WriteResponse resp;
    resp.set_success(true);
    resp.set_version(version);
    return resp;
}

chain::ReadResponse ChainReplication::handle_read(const chain::ReadRequest& req, Node& node) {
    if (!node.is_tail()) {
        throw runtime_error("CHAIN read rejected: node is not tail");
    }

    const auto latest = support_.read_latest(req.key());
    if (!latest.found) {
        throw runtime_error("CHAIN read failed: key not found or not committed");
    }

    cout << "[Chain] Tail " << chain_node_label(node)
         << " serving read key='" << req.key() << "' version=" << latest.version << "\n";

    chain::ReadResponse resp;
    resp.set_key(req.key());
    resp.set_value(latest.value);
    resp.set_version(latest.version);
    return resp;
}

void ChainReplication::handle_propagate(const chain::PropagateRequest& req, Node& node) {
    cout << "[Chain] Node " << chain_node_label(node)
         << " received PROPAGATE key='" << req.key() << "' version=" << req.version() << "\n";
    support_.apply_replica_write(req.key(), req.value(), req.version());

    if (node.is_tail()) {
        support_.mark_write_committed(req.key(), req.version());
        support_.notify_commit(req.key(), req.version());

        cout << "[Chain] Tail committed key='" << req.key() << "' version=" << req.version() << "\n";

        if (!node.is_head()) {
            auto pred = support_.predecessor_stub();
            if (!pred) {
                throw runtime_error("CHAIN tail ACK failed: no predecessor stub");
            }

            chain::AckRequest ack;
            ack.set_key(req.key());
            ack.set_version(req.version());

            google::protobuf::Empty ignored;
            grpc::ClientContext ctx;

            cout << "[Chain] Tail sending ACK key='" << req.key()
                 << "' version=" << req.version() << " to predecessor\n";
            grpc::Status status = pred->Ack(&ctx, ack, &ignored);
            if (!status.ok()) {
                throw runtime_error("CHAIN tail ACK to predecessor failed: " + status.error_message());
            }
        }
        return;
    }

    auto succ = support_.successor_stub();
    if (!succ) {
        throw runtime_error("CHAIN propagate failed: node has no successor stub");
    }

    google::protobuf::Empty ignored;
    grpc::ClientContext ctx;

    cout << "[Chain] Forwarding PROPAGATE key='" << req.key() << "' version=" << req.version()
         << " to successor\n";
    grpc::Status status = succ->Propagate(&ctx, req, &ignored);
    if (!status.ok()) {
        throw runtime_error("CHAIN propagate forward failed: " + status.error_message());
    }
}

void ChainReplication::handle_ack(const chain::AckRequest& req, Node& node) {
    cout << "[Chain] Node " << chain_node_label(node)
         << " received ACK key='" << req.key() << "' version=" << req.version() << "\n";

    support_.mark_write_committed(req.key(), req.version());
    support_.notify_commit(req.key(), req.version());

    if (node.is_head()) {
        cout << "[Chain] Head finalized commit key='" << req.key()
             << "' version=" << req.version() << "\n";
        return;
    }

    auto pred = support_.predecessor_stub();
    if (!pred) {
        throw runtime_error("CHAIN ack forward failed: node has no predecessor stub");
    }

    google::protobuf::Empty ignored;
    grpc::ClientContext ctx;

    cout << "[Chain] Forwarding ACK key='" << req.key() << "' version=" << req.version()
         << " to predecessor\n";
    grpc::Status status = pred->Ack(&ctx, req, &ignored);
    if (!status.ok()) {
        throw runtime_error("CHAIN ack forward to predecessor failed: " + status.error_message());
    }
}

void ChainReplication::on_config_change(Node& node) {
    support_.on_config_change(node);
}
