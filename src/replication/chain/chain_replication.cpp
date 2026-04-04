#include "chain_replication.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

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

void forward_propagate_async(std::shared_ptr<chain::ChainNode::Stub> successor,
                             chain::PropagateRequest req,
                             const std::string& from_node) {
    std::thread([successor = std::move(successor),
                 req = std::move(req),
                 from_node]() mutable {
        if (!successor) {
            cerr << "[Chain] Async PROPAGATE skipped from " << from_node
                 << ": no successor stub\n";
            return;
        }

        google::protobuf::Empty ignored;
        grpc::ClientContext ctx;
        grpc::Status status = successor->Propagate(&ctx, req, &ignored);
        if (!status.ok()) {
            cerr << "[Chain] Async PROPAGATE failed from " << from_node
                 << " key='" << req.key() << "' version=" << req.version()
                 << ": " << status.error_message() << "\n";
        }
    }).detach();
}

void forward_ack_async(std::shared_ptr<chain::ChainNode::Stub> predecessor,
                       chain::AckRequest req,
                       const std::string& from_node) {
    std::thread([predecessor = std::move(predecessor),
                 req = std::move(req),
                 from_node]() mutable {
        if (!predecessor) {
            cerr << "[Chain] Async ACK skipped from " << from_node
                 << ": no predecessor stub\n";
            return;
        }

        google::protobuf::Empty ignored;
        grpc::ClientContext ctx;
        grpc::Status status = predecessor->Ack(&ctx, req, &ignored);
        if (!status.ok()) {
            cerr << "[Chain] Async ACK failed from " << from_node
                 << " key='" << req.key() << "' version=" << req.version()
                 << ": " << status.error_message() << "\n";
        }
    }).detach();
}

void notify_client_ack_async(chain::AckRequest req,
                             const std::string& from_node) {
    std::thread([req = std::move(req), from_node]() mutable {
        if (req.client_addr().empty()) {
            cerr << "[Chain] Client ACK skipped from " << from_node
                 << ": empty client_addr\n";
            return;
        }

        auto channel = grpc::CreateChannel(req.client_addr(), grpc::InsecureChannelCredentials());
        auto client_stub = chain::ChainNode::NewStub(channel);

        google::protobuf::Empty ignored;
        grpc::ClientContext ctx;
        grpc::Status status = client_stub->Ack(&ctx, req, &ignored);
        if (!status.ok()) {
            cerr << "[Chain] Client ACK failed from " << from_node
                 << " request_id=" << req.request_id()
                 << " client_addr='" << req.client_addr()
                 << "': " << status.error_message() << "\n";
        }
    }).detach();
}

} // namespace

chain::WriteResponse ChainReplication::handle_write(const chain::WriteRequest& req, Node& node) {
    if (!node.is_head()) {
        throw runtime_error("CHAIN write rejected: node is not head");
    }

    const uint64_t version = support_.assign_next_version(req.key());
    support_.record_local_write(req.key(), req.value(), version);

    cout << "[Chain] Head " << chain_node_label(node)
        << " accepted write key='" << req.key() << "' version=" << version
        << " (response means accepted by head; commit happens later when ACK comes back)\n";

    chain::WriteResponse resp;
    resp.set_success(true);
    resp.set_version(version);

    if (node.is_tail()) {
       support_.mark_version_clean(req.key(), version);
       cout << "[Chain] Single-node chain committed immediately key='" << req.key()
           << "' version=" << version << "\n";

        chain::AckRequest ack;
        ack.set_key(req.key());
        ack.set_version(version);
        ack.set_client_addr(req.client_addr());
        ack.set_request_id(req.request_id());
        notify_client_ack_async(std::move(ack), chain_node_label(node));

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

    const auto clean = support_.read_clean(req.key());
    if (!clean.found) {
        throw runtime_error("CHAIN read failed: key not found or not committed");
    }

    cout << "[Chain] Tail " << chain_node_label(node)
         << " serving CLEAN read key='" << req.key() << "' version=" << clean.version << "\n";

    chain::ReadResponse resp;
    resp.set_key(req.key());
    resp.set_value(clean.value);
    resp.set_version(clean.version);
    return resp;
}

void ChainReplication::handle_propagate(const chain::PropagateRequest& req, Node& node) {
    cout << "[Chain] Node " << chain_node_label(node)
         << " received PROPAGATE key='" << req.key() << "' version=" << req.version() << "\n";
    support_.record_local_write(req.key(), req.value(), req.version());

    if (node.is_tail()) {
        support_.mark_version_clean(req.key(), req.version());

        cout << "[Chain] Tail committed key='" << req.key() << "' version=" << req.version()
             << " and will send ACK asynchronously\n";

        if (!node.is_head()) {
            auto pred = support_.predecessor_stub();

            chain::AckRequest ack;
            ack.set_key(req.key());
            ack.set_version(req.version());
            ack.set_client_addr(req.client_addr());
            ack.set_request_id(req.request_id());

            forward_ack_async(pred, std::move(ack), chain_node_label(node));
        }
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
    cout << "[Chain] Node " << chain_node_label(node)
         << " received ACK key='" << req.key() << "' version=" << req.version() << "\n";

    support_.mark_version_clean(req.key(), req.version());

    if (node.is_head()) {
        cout << "[Chain] Head finalized commit request_id=" << req.request_id()
             << " key='" << req.key() << "' version=" << req.version()
             << " client_addr='" << req.client_addr() << "'\n";

        chain::AckRequest client_ack = req;
        notify_client_ack_async(std::move(client_ack), chain_node_label(node));

        return;
    }

    auto pred = support_.predecessor_stub();
    chain::AckRequest fwd = req;
    forward_ack_async(pred, std::move(fwd), chain_node_label(node));

    cout << "[Chain] Node " << chain_node_label(node)
         << " forwarded ACK asynchronously key='" << req.key()
         << "' version=" << req.version() << "\n";
}

void ChainReplication::on_config_change(Node& node) {
    support_.on_config_change(node);
}
