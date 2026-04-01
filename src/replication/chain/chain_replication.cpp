#include "chain_replication.h"
#include <iostream>
#include "chain.pb.h"

using namespace std;

chain::WriteResponse ChainReplication::handle_write(const chain::WriteRequest& req, Node& node) {
    cout << "[Chain] handle_write called\n";
    return chain::WriteResponse();
}

chain::ReadResponse ChainReplication::handle_read(const chain::ReadRequest& req, Node& node) {
    cout << "[Chain] handle_read called\n";
    return chain::ReadResponse();
}

void ChainReplication::handle_propagate(const chain::PropagateRequest& req, Node& node) {
    cout << "[Chain] handle_propagate called\n";
}

void ChainReplication::handle_ack(const chain::AckRequest& req, Node& node) {
    cout << "[Chain] handle_ack called\n";
}
