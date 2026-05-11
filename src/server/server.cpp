// server.cpp — entry point for a chain-replication node.
//
// The server does NOT read topology config from files or CLI args.
// CLI args only control bind host/port, optional server logging, and
// optionally --join <metadata_addr> to add this node to a running cluster.
// All topology config is pushed via the Configure RPC by the metadata server.

#include <iostream>
#include <memory>
#include <string>
#include <stdexcept>
#include <cctype>
#include <atomic>
#include <thread>
#include <chrono>

#include <grpcpp/grpcpp.h>
#include "chain.grpc.pb.h"

#include "node/node.h"
#include "replication/replication_strategy.h"
#include "replication/chain/chain_replication.h"
#include "replication/craq/craq_replication.h"
#include "replication/crown/crown_replication.h"
#include "replication/common/chain_style_replication_support.h"

using namespace std;

namespace {

bool parse_bool_flag(const string& raw, bool* out) {
    string normalized;
    normalized.reserve(raw.size());
    for (unsigned char ch : raw)
        normalized.push_back(static_cast<char>(std::tolower(ch)));

    if (normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "y" || normalized == "on") {
        *out = true;
        return true;
    }
    if (normalized == "0" || normalized == "false" || normalized == "no" || normalized == "n" || normalized == "off") {
        *out = false;
        return true;
    }
    return false;
}

void print_usage(const char* program_name) {
    cerr << "Usage: " << program_name
         << " [--host <host>] [--port <port>] [--server-log <true|false>]"
         << " [--join <metadata_host:port>]\n";
}

} // namespace

// ============================================================
// gRPC service implementation
// ============================================================

class ChainNodeServiceImpl final : public chain::ChainNode::Service {
public:
    ChainNodeServiceImpl()
        : node_(NodeConfig{}) {}

    // ----------------------------------------------------------
    // Config RPC — called by metadata_server (initial + reconfig)
    // ----------------------------------------------------------

    grpc::Status Configure(grpc::ServerContext*     /*ctx*/,
                           const chain::NodeConfig* req,
                           google::protobuf::Empty* /*resp*/) override {
        NodeConfig cfg = proto_to_config(*req);

        if (!strategy_ || cfg.mode != node_.mode()) {
            strategy_ = make_strategy(cfg.mode);
            cout << "[Server] Strategy set to " << mode_name(cfg.mode) << "\n";
        }

        node_.update_config(std::move(cfg));
        strategy_->on_config_change(node_);

        // A Configure arriving during freeze means reconfig is complete —
        // resume accepting client writes.
        if (frozen_.exchange(false)) {
            cout << "[Server] Unfrozen by Configure (reconfig_id="
                 << active_reconfig_id_.load() << ")\n";
            active_reconfig_id_.store(0);
        }
        return grpc::Status::OK;
    }

    // ----------------------------------------------------------
    // Client-facing RPCs
    // ----------------------------------------------------------

    grpc::Status Write(grpc::ServerContext*       /*ctx*/,
                       const chain::WriteRequest* req,
                       chain::WriteResponse*      resp) override {
        if (frozen_.load())
            return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                                "node frozen for reconfig");
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

    // ----------------------------------------------------------
    // Liveness probe
    // ----------------------------------------------------------

    grpc::Status Ping(grpc::ServerContext*       /*ctx*/,
                      const chain::PingRequest*  req,
                      chain::PingResponse*       resp) override {
        resp->set_node_id(strategy_ ? node_.node_index() : -1);
        resp->set_seq(req->seq());
        resp->set_configured(strategy_ != nullptr);
        return grpc::Status::OK;
    }

    // ----------------------------------------------------------
    // Reconfigure protocol RPCs
    // ----------------------------------------------------------

    grpc::Status Freeze(grpc::ServerContext*         /*ctx*/,
                        const chain::FreezeRequest*  req,
                        google::protobuf::Empty*     /*resp*/) override {
        if (!strategy_)
            return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                                "Node not configured yet");

        const uint64_t reconfig_id = req->reconfig_id();
        const string metadata_addr = req->metadata_addr().host() + ":"
                                   + std::to_string(req->metadata_addr().port());

        frozen_.store(true);
        active_reconfig_id_.store(reconfig_id);
        metadata_addr_.store(new string(metadata_addr));  // leak-on-overwrite is fine for rare reconfig

        cout << "[Server] Frozen for reconfig_id=" << reconfig_id
             << " (metadata=" << metadata_addr << ")\n";

        // Initiate inflight check based on mode + role:
        //   CROWN: every node sends its own token
        //   CHAIN/CRAQ: only head initiates
        const ReplicationMode mode = node_.mode();
        const bool should_initiate = (mode == ReplicationMode::CROWN) || node_.is_head();
        if (should_initiate) {
            const int32_t my_id = node_.node_index();
            strategy_->support()->send_inflight_check(reconfig_id, my_id);
            cout << "[Server] Initiated InflightCheck origin=" << my_id << "\n";
        }
        return grpc::Status::OK;
    }

    grpc::Status InflightCheck(grpc::ServerContext*                /*ctx*/,
                               const chain::InflightCheckRequest*  req,
                               google::protobuf::Empty*            /*resp*/) override {
        if (!strategy_)
            return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                                "Node not configured yet");

        const uint64_t reconfig_id = req->reconfig_id();
        const int32_t  origin      = req->origin_node_id();
        const int32_t  my_id       = node_.node_index();
        const ReplicationMode mode = node_.mode();

        // Determine if this node is the terminal for the token:
        //   CROWN: terminal when origin == self (token has circled the ring)
        //   CHAIN/CRAQ: terminal when this node is tail
        const bool terminal = (mode == ReplicationMode::CROWN)
                                ? (origin == my_id)
                                : node_.is_tail();

        if (terminal) {
            // ACK the metadata server
            cout << "[Server] InflightCheck terminal at node " << my_id
                 << " reconfig_id=" << reconfig_id << " (origin=" << origin << ")\n";
            send_inflight_ack_to_metadata(reconfig_id, my_id);
        } else {
            // Forward to successor with retry
            cout << "[Server] InflightCheck forwarding from node " << my_id
                 << " reconfig_id=" << reconfig_id << " (origin=" << origin << ")\n";
            strategy_->support()->send_inflight_check(reconfig_id, origin);
        }
        return grpc::Status::OK;
    }

    grpc::Status FetchData(grpc::ServerContext*            /*ctx*/,
                           const chain::FetchDataRequest*  /*req*/,
                           chain::DataDump*                resp) override {
        if (!strategy_)
            return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                                "Node not configured yet");
        *resp = strategy_->support()->dump_committed_state();
        cout << "[Server] FetchData served " << resp->entries_size() << " entries\n";
        return grpc::Status::OK;
    }

    grpc::Status BootstrapFromSource(grpc::ServerContext*             /*ctx*/,
                                     const chain::BootstrapRequest*   req,
                                     google::protobuf::Empty*         /*resp*/) override {
        // New node: fetch data from source, populate local state, ACK metadata.
        const string source_addr = req->source_addr().host() + ":"
                                 + std::to_string(req->source_addr().port());
        const uint64_t reconfig_id = req->reconfig_id();
        const int32_t  new_node_id = req->new_node_id();

        cout << "[Server] BootstrapFromSource: fetching from " << source_addr
             << " (reconfig_id=" << reconfig_id << ")\n";

        // Run the fetch in a detached thread so we don't block the RPC
        std::thread([this, source_addr, reconfig_id, new_node_id]() {
            auto channel = grpc::CreateChannel(source_addr, grpc::InsecureChannelCredentials());
            auto stub = chain::ChainNode::NewStub(channel);

            chain::FetchDataRequest fetch_req;
            fetch_req.set_reconfig_id(reconfig_id);
            chain::DataDump dump;
            grpc::ClientContext fctx;
            grpc::Status status = stub->FetchData(&fctx, fetch_req, &dump);
            if (!status.ok()) {
                cerr << "[Server] BootstrapFromSource: FetchData failed: "
                     << status.error_message() << "\n";
                return;
            }
            cout << "[Server] BootstrapFromSource: received " << dump.entries_size()
                 << " entries, loading...\n";

            if (strategy_) {
                strategy_->support()->load_from_dump(dump);
            }

            // Tell metadata we're ready
            const string* meta_addr_ptr = metadata_addr_.load();
            if (!meta_addr_ptr || meta_addr_ptr->empty()) {
                cerr << "[Server] BootstrapFromSource: no metadata_addr to ACK\n";
                return;
            }
            auto meta_channel = grpc::CreateChannel(*meta_addr_ptr, grpc::InsecureChannelCredentials());
            auto meta_stub = chain::MetadataStore::NewStub(meta_channel);
            chain::DataReadyRequest dr;
            dr.set_reconfig_id(reconfig_id);
            dr.set_new_node_id(new_node_id);
            google::protobuf::Empty ignored;
            grpc::ClientContext dctx;
            grpc::Status drs = meta_stub->DataReady(&dctx, dr, &ignored);
            if (!drs.ok()) {
                cerr << "[Server] DataReady RPC failed: " << drs.error_message() << "\n";
            } else {
                cout << "[Server] DataReady sent for reconfig_id=" << reconfig_id << "\n";
            }
        }).detach();

        return grpc::Status::OK;
    }

    // ----------------------------------------------------------
    // Configuration entry from CLI --join flow
    // ----------------------------------------------------------

    // Called from main() when --join was specified. Stashes the metadata
    // address so BootstrapFromSource can locate the metadata server.
    void set_metadata_addr(const string& addr) {
        metadata_addr_.store(new string(addr));
    }

private:
    void send_inflight_ack_to_metadata(uint64_t reconfig_id, int32_t my_id) {
        const string* meta_addr_ptr = metadata_addr_.load();
        if (!meta_addr_ptr || meta_addr_ptr->empty()) {
            cerr << "[Server] InflightAck skipped: no metadata_addr\n";
            return;
        }
        // Run async so we don't block the RPC handler
        const string addr = *meta_addr_ptr;
        std::thread([addr, reconfig_id, my_id]() {
            static constexpr int kMaxAttempts = 4;
            static constexpr int kBackoffs[] = {2, 5, 10};

            auto channel = grpc::CreateChannel(addr, grpc::InsecureChannelCredentials());
            auto stub = chain::MetadataStore::NewStub(channel);
            chain::InflightAckRequest req;
            req.set_reconfig_id(reconfig_id);
            req.set_node_id(my_id);

            for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
                google::protobuf::Empty ignored;
                grpc::ClientContext ctx;
                grpc::Status status = stub->InflightAck(&ctx, req, &ignored);
                if (status.ok()) {
                    cout << "[Server] InflightAck sent for reconfig_id=" << reconfig_id
                         << " node_id=" << my_id << "\n";
                    return;
                }
                cerr << "[Server] InflightAck attempt " << (attempt + 1)
                     << " failed: " << status.error_message() << "\n";
                if (attempt < kMaxAttempts - 1) {
                    std::this_thread::sleep_for(std::chrono::seconds(kBackoffs[attempt]));
                }
            }
        }).detach();
    }

    Node                            node_;
    unique_ptr<ReplicationStrategy> strategy_;
    std::atomic<bool>               frozen_{false};
    std::atomic<uint64_t>           active_reconfig_id_{0};
    // metadata address is set on first Freeze or via --join; raw atomic ptr
    // (intentional small-leak on overwrite — only changes during reconfig)
    std::atomic<string*>            metadata_addr_{nullptr};

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

    static NodeAddress addr_from_proto(const chain::NodeAddress& p) {
        return { p.host(), p.port() };
    }

    static NodeConfig proto_to_config(const chain::NodeConfig& p) {
        NodeConfig cfg;
        cfg.node_id   = std::to_string(p.node_id());
        cfg.node_index = p.node_id();
        cfg.self_addr = addr_from_proto(p.self_addr());
        cfg.is_head   = p.is_head();
        cfg.is_tail   = p.is_tail();
        cfg.crown_node_count = p.head_ranges_size();

        if (p.has_predecessor()) cfg.predecessor = addr_from_proto(p.predecessor());
        if (p.has_successor())   cfg.successor   = addr_from_proto(p.successor());
        if (p.has_tail())        cfg.tail        = addr_from_proto(p.tail());

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
// main — bind, optionally join, then wait
// ============================================================

namespace {

void send_join_to_metadata(const string& metadata_addr,
                           const string& self_host,
                           int           self_port) {
    auto channel = grpc::CreateChannel(metadata_addr, grpc::InsecureChannelCredentials());
    auto stub = chain::MetadataStore::NewStub(channel);
    chain::JoinRequest req;
    req.mutable_addr()->set_host(self_host);
    req.mutable_addr()->set_port(self_port);

    chain::JoinResponse resp;
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
    grpc::Status status = stub->Join(&ctx, req, &resp);

    if (!status.ok()) {
        cerr << "[Server] Join RPC failed: " << status.error_message() << "\n";
        return;
    }
    if (!resp.accepted()) {
        cerr << "[Server] Join rejected by metadata: " << resp.error() << "\n";
        return;
    }
    cout << "[Server] Joined cluster, assigned node_id=" << resp.assigned_node_id() << "\n";
}

} // namespace

int main(int argc, char** argv) {
    string host = "0.0.0.0";
    string port = "50051";
    bool server_log_enabled = false;
    string join_metadata_addr;

    for (int i = 1; i < argc; ++i) {
        const string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        }

        if (arg == "--host" || arg == "--port" || arg == "--server-log" || arg == "--join") {
            if (i + 1 >= argc) {
                cerr << "[Server] Missing value for " << arg << "\n";
                print_usage(argv[0]);
                return 1;
            }

            const string value = argv[++i];
            if (arg == "--host") {
                host = value;
            } else if (arg == "--port") {
                port = value;
            } else if (arg == "--join") {
                join_metadata_addr = value;
            } else {
                if (!parse_bool_flag(value, &server_log_enabled)) {
                    cerr << "[Server] Invalid value for --server-log: " << value << "\n";
                    print_usage(argv[0]);
                    return 1;
                }
            }
            continue;
        }

        cerr << "[Server] Unknown argument: " << arg << "\n";
        print_usage(argv[0]);
        return 1;
    }

    const string addr = host + ":" + port;
    ChainNodeServiceImpl service;

    grpc::ServerBuilder builder;
    builder.AddListeningPort(addr, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    unique_ptr<grpc::Server> server = builder.BuildAndStart();
    if (!server) {
        cerr << "[Server] Failed to bind on " << addr << "\n";
        return 1;
    }

    if (server_log_enabled)
        cout << "[Server] Listening on " << addr << " - waiting for client config.\n";
    else {
        cout.setstate(std::ios_base::failbit);
        cerr.setstate(std::ios_base::failbit);
    }

    // If --join was specified, contact metadata server to be added.
    if (!join_metadata_addr.empty()) {
        service.set_metadata_addr(join_metadata_addr);
        // Run in background so the gRPC server keeps serving
        std::thread([&service, join_metadata_addr, host, port]() {
            // Tiny delay so our server is fully listening
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            int port_num = 0;
            try { port_num = std::stoi(port); } catch (...) { port_num = 0; }
            send_join_to_metadata(join_metadata_addr, host, port_num);
        }).detach();
    }

    server->Wait();
    return 0;
}
