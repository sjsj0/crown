// server.cpp — entry point for a chain-replication node.
//
// The server does NOT read any config from files or CLI args (beyond --port).
// All topology config is pushed to it by client.cpp via the Configure RPC.
// The server just starts, registers the gRPC service, and waits.

#include <iostream>
#include <memory>
#include <string>
#include <stdexcept>

#include <grpcpp/grpcpp.h>
#include "chain.grpc.pb.h"

#include "node/node.cpp"
#include "replication/replication_strategy.h"
#include "replication/chain/chain_replication.h"
#include "replication/craq/craq_replication.h"
#include "replication/crown/crown_replication.h"

using namespace std;

// ============================================================
// gRPC service implementation
// ============================================================

class ChainNodeServiceImpl final : public chain::ChainNode::Service {
public:
    // Node starts with an empty/default config. The client pushes the real
    // config via Configure before issuing any reads or writes.
    ChainNodeServiceImpl()
        : node_(NodeConfig{}) {}

    // ----------------------------------------------------------
    // Config RPC — called by client.cpp on startup
    // ----------------------------------------------------------

    grpc::Status Configure(grpc::ServerContext*     /*ctx*/,
                           const chain::NodeConfig* req,
                           google::protobuf::Empty* /*resp*/) override {
        NodeConfig cfg = proto_to_config(*req);

        // Swap strategy if the mode changed (or on first configure).
        if (!strategy_ || cfg.mode != node_.mode()) {
            strategy_ = make_strategy(cfg.mode);
            cout << "[Server] Strategy set to " << mode_name(cfg.mode) << "\n";
        }

        node_.update_config(std::move(cfg));
        strategy_->on_config_change(node_);
        return grpc::Status::OK;
    }

    // ----------------------------------------------------------
    // Client-facing RPCs
    // ----------------------------------------------------------

    grpc::Status Write(grpc::ServerContext*       /*ctx*/,
                       const chain::WriteRequest* req,
                       chain::WriteResponse*      resp) override {
        if (!strategy_)
            return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                                "Node not configured yet");
        try {
            *resp = strategy_->handle_write(*req, node_);
        } catch (const exception& ex) {
            return grpc::Status(grpc::StatusCode::INTERNAL, ex.what());
        }
        return grpc::Status::OK;
    }

    grpc::Status Read(grpc::ServerContext*      /*ctx*/,
                      const chain::ReadRequest* req,
                      chain::ReadResponse*      resp) override {
        if (!strategy_)
            return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                                "Node not configured yet");
        try {
            *resp = strategy_->handle_read(*req, node_);
        } catch (const exception& ex) {
            return grpc::Status(grpc::StatusCode::INTERNAL, ex.what());
        }
        return grpc::Status::OK;
    }

    // ----------------------------------------------------------
    // Peer-facing RPCs (called by neighbour nodes)
    // ----------------------------------------------------------

    grpc::Status Propagate(grpc::ServerContext*           /*ctx*/,
                           const chain::PropagateRequest* req,
                           google::protobuf::Empty*       /*resp*/) override {
        if (!strategy_)
            return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                                "Node not configured yet");
        try {
            strategy_->handle_propagate(*req, node_);
        } catch (const exception& ex) {
            return grpc::Status(grpc::StatusCode::INTERNAL, ex.what());
        }
        return grpc::Status::OK;
    }

    grpc::Status Ack(grpc::ServerContext*     /*ctx*/,
                     const chain::AckRequest* req,
                     google::protobuf::Empty* /*resp*/) override {
        if (!strategy_)
            return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                                "Node not configured yet");
        try {
            strategy_->handle_ack(*req, node_);
        } catch (const exception& ex) {
            return grpc::Status(grpc::StatusCode::INTERNAL, ex.what());
        }
        return grpc::Status::OK;
    }

    // CRAQ-only — non-tail node queries the tail for latest committed version.
    // Chain and CROWN strategies will throw, which maps to UNIMPLEMENTED.
    grpc::Status VersionQuery(grpc::ServerContext*              /*ctx*/,
                              const chain::VersionQueryRequest* req,
                              chain::VersionQueryResponse*      resp) override {
        if (!strategy_)
            return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                                "Node not configured yet");
        try {
            *resp = strategy_->handle_version_query(*req, node_);
        } catch (const exception& ex) {
            return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, ex.what());
        }
        return grpc::Status::OK;
    }

private:
    Node                            node_;
    unique_ptr<ReplicationStrategy> strategy_;   // null until Configure is called

    // ----------------------------------------------------------
    // Strategy factory
    // ----------------------------------------------------------
    static unique_ptr<ReplicationStrategy> make_strategy(ReplicationMode mode) {
        switch (mode) {
            case ReplicationMode::CHAIN: return make_unique<ChainReplication>();
            case ReplicationMode::CRAQ:  return make_unique<CRAQReplication>();
            case ReplicationMode::CROWN: return make_unique<CROWNReplication>();
        }
        throw invalid_argument("Unknown ReplicationMode");
    }

    static string mode_name(ReplicationMode mode) {
        switch (mode) {
            case ReplicationMode::CHAIN: return "CHAIN";
            case ReplicationMode::CRAQ:  return "CRAQ";
            case ReplicationMode::CROWN:  return "CROWN";
        }
        return "UNKNOWN";
    }

    // ----------------------------------------------------------
    // Proto <-> domain-type helpers
    // ----------------------------------------------------------

    static NodeAddress addr_from_proto(const chain::NodeAddress& p) {
        return { p.host(), p.port() };
    }

    static NodeConfig proto_to_config(const chain::NodeConfig& p) {
        NodeConfig cfg;
        cfg.node_id   = p.node_id();
        cfg.self_addr = addr_from_proto(p.self_addr());
        cfg.is_head   = p.is_head();
        cfg.is_tail   = p.is_tail();

        if (p.has_predecessor()) cfg.predecessor = addr_from_proto(p.predecessor());
        if (p.has_successor())   cfg.successor   = addr_from_proto(p.successor());

        for (const auto& kr : p.head_ranges())
            cfg.head_ranges.push_back({ kr.start_key(), kr.end_key() });
        for (const auto& kr : p.tail_ranges())
            cfg.tail_ranges.push_back({ kr.start_key(), kr.end_key() });

        switch (p.mode()) {
            case chain::ReplicationMode::CHAIN: cfg.mode = ReplicationMode::CHAIN; break;
            case chain::ReplicationMode::CRAQ:  cfg.mode = ReplicationMode::CRAQ;  break;
            case chain::ReplicationMode::CROWN:  cfg.mode = ReplicationMode::CROWN;  break;
            default: throw invalid_argument("Unknown ReplicationMode in proto");
        }
        return cfg;
    }
};

// ============================================================
// main — just bind and wait
// ============================================================

int main(int argc, char** argv) {
    // The only thing the server needs at launch is which port to listen on.
    // Everything else comes from the client via Configure RPC.
    string port = "50051";
    if (argc == 3 && string(argv[1]) == "--port")
        port = argv[2];

    string addr = "0.0.0.0:" + port;
    ChainNodeServiceImpl service;

    grpc::ServerBuilder builder;
    builder.AddListeningPort(addr, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    unique_ptr<grpc::Server> server = builder.BuildAndStart();
    if (!server) {
        cerr << "[Server] Failed to bind on " << addr << "\n";
        return 1;
    }

    cout << "[Server] Listening on " << addr << " — waiting for client config.\n";
    server->Wait();
    return 0;
}
