#pragma once

#include <string>
#include <vector>

namespace craq {

struct NodeInfo {
    std::string id;
    std::string host;
    int port{0};
};

struct ClusterConfig {
    std::string mode;
    std::vector<NodeInfo> nodes;
};

bool parse_cluster_config(const std::string& path, ClusterConfig& out, std::string& error);

} // namespace craq
