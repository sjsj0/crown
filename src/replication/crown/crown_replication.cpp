#include "crown_replication.h"
#include <iostream>
#include "chain.pb.h"

using namespace std;

chain::WriteResponse CROWNReplication::handle_write(const chain::WriteRequest& req, Node& node) {
    cout << "[CROWN] handle_write called\n";
    return chain::WriteResponse();
}

chain::ReadResponse CROWNReplication::handle_read(const chain::ReadRequest& req, Node& node) {
    cout << "[CROWN] handle_read called\n";
    return chain::ReadResponse();
}

void CROWNReplication::handle_propagate(const chain::PropagateRequest& req, Node& node) {
    cout << "[CROWN] handle_propagate called\n";
}

void CROWNReplication::handle_ack(const chain::AckRequest& req, Node& node) {
    cout << "[CROWN] handle_ack called\n";
}
