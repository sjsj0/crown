#pragma once
#include "../replication_strategy.h"
#include "../common/chain_style_replication_support.h"

class CRAQReplication : public ReplicationStrategy {
public:
    chain::WriteResponse handle_write(const chain::WriteRequest& req, Node& node) override;

    chain::ReadResponse handle_read(const chain::ReadRequest& req, Node& node) override;

    void handle_propagate(const chain::PropagateRequest& req, Node& node) override;

    void handle_ack(const chain::AckRequest& req, Node& node) override;

    chain::VersionQueryResponse handle_version_query(const chain::VersionQueryRequest& req, Node& node) override;

    void on_config_change(Node& node) override;

private:
    ChainStyleReplicationSupport support_;
};
