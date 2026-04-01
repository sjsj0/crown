#pragma once
#include "../replication_strategy.h"

class CROWNReplication : public ReplicationStrategy {
public:
    chain::WriteResponse handle_write(const chain::WriteRequest& req, Node& node) override;

    chain::ReadResponse handle_read(const chain::ReadRequest& req, Node& node) override;

    void handle_propagate(const chain::PropagateRequest& req, Node& node) override;

    void handle_ack(const chain::AckRequest& req, Node& node) override;
};