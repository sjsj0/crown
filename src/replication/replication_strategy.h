#pragma once

#include <string>

// Forward declarations of protobuf types — avoids pulling in heavy headers
// in every translation unit that only needs the interface.
namespace chain {
    class WriteRequest;
    class WriteResponse;
    class ReadRequest;
    class ReadResponse;
    class PropagateRequest;
    class AckRequest;
    class VersionQueryRequest;
    class VersionQueryResponse;
}

class Node;

using namespace std;

// ============================================================
// ReplicationStrategy — interface the server delegates into
// ============================================================
//
// Each concrete strategy (Chain, CRAQ, CROWN) implements this interface
// and owns its own key/value store, since storage semantics differ:
//
//   Chain  — one clean value per key, writes committed immediately at tail.
//   CRAQ   — one clean value + a map<version, value> of pending dirty writes.
//   CROWN  — same as chain per segment, but each node may be head/tail for
//             different key ranges simultaneously.
//
// The server only calls handle_write, handle_read, handle_propagate, and
// handle_ack. handle_back_propagate and handle_version_query are CRAQ-only;
// the base provides a default that throws NOT_IMPLEMENTED so chain and crown
// strategies don't have to override them.

class ReplicationStrategy {
public:
    virtual ~ReplicationStrategy() = default;

    // ----------------------------------------------------------
    // Client-facing operations (called by the server RPC handlers)
    // ----------------------------------------------------------

    // Handle a write initiated by a client.
    // Head nodes start propagation; non-head nodes should reject or forward.
    virtual chain::WriteResponse handle_write(const chain::WriteRequest& req,
                                              Node& node) = 0;

    // Handle a read from a client.
    // Tail nodes (chain) serve directly. CRAQ non-tail nodes may need a
    // version query. CROWN nodes check key-range ownership first.
    virtual chain::ReadResponse handle_read(const chain::ReadRequest& req,
                                            Node& node) = 0;

    // ----------------------------------------------------------
    // Peer-facing operations (called by neighbour nodes via RPC)
    // ----------------------------------------------------------

    // A predecessor has forwarded a write down the chain.
    // The strategy applies it to its store and propagates further if needed.
    virtual void handle_propagate(const chain::PropagateRequest& req,
                                  Node& node) = 0;

    // An acknowledgment is travelling back up the chain toward the head.
    // Chain and crown: mark the version committed and forward the ack.
    // CRAQ: same, but also unblocks pending dirty reads for this version.
    virtual void handle_ack(const chain::AckRequest& req,
                             Node& node) = 0;

    // ----------------------------------------------------------
    // CRAQ-only: back-propagation from tail to non-tail on reads
    // ----------------------------------------------------------
    // A non-tail node has a dirty read and queries the tail for the latest
    // committed version. Default throws so chain/crown don't need to implement it.
    virtual chain::VersionQueryResponse
    handle_version_query(const chain::VersionQueryRequest& req, Node& node);

    // ----------------------------------------------------------
    // Lifecycle
    // ----------------------------------------------------------

    // Called after the server applies a new NodeConfig to the Node.
    // Strategies can use this to re-establish stubs to new neighbours.
    virtual void on_config_change(Node& node) {}
};