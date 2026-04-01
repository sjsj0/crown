#include "craq_replication.h"
#include <iostream>
#include "chain.pb.h"

using namespace std;

chain::WriteResponse CRAQReplication::handle_write(const chain::WriteRequest& req, Node& node) {
    cout << "[CRAQ] handle_write called\n";
    return chain::WriteResponse();
}

chain::ReadResponse CRAQReplication::handle_read(const chain::ReadRequest& req, Node& node) {
    cout << "[CRAQ] handle_read called\n";
    return chain::ReadResponse();
}

void CRAQReplication::handle_propagate(const chain::PropagateRequest& req, Node& node) {
    cout << "[CRAQ] handle_propagate called\n";
}

void CRAQReplication::handle_ack(const chain::AckRequest& req, Node& node) {
    cout << "[CRAQ] handle_ack called\n";
}

chain::VersionQueryResponse CRAQReplication::handle_version_query(const chain::VersionQueryRequest& req, Node& node) {
    cout << "[CRAQ] handle_version_query called\n";
    return chain::VersionQueryResponse();
}
