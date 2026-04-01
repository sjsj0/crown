#include "config.hpp"

#include <cctype>
#include <fstream>
#include <sstream>

namespace craq {

namespace {

std::string trim(const std::string& in) {
    size_t start = 0;
    while (start < in.size() && std::isspace(static_cast<unsigned char>(in[start]))) {
        ++start;
    }
    size_t end = in.size();
    while (end > start && std::isspace(static_cast<unsigned char>(in[end - 1]))) {
        --end;
    }
    return in.substr(start, end - start);
}

} // namespace

bool parse_cluster_config(const std::string& path, ClusterConfig& out, std::string& error) {
    std::ifstream in(path);
    if (!in) {
        error = "cannot open config file: " + path;
        return false;
    }

    out = ClusterConfig{};
    std::string line;
    int line_no = 0;
    while (std::getline(in, line)) {
        ++line_no;
        line = trim(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }

        std::istringstream iss(line);
        std::string token;
        iss >> token;

        if (token == "mode" || token == "MODE") {
            if (!(iss >> out.mode)) {
                error = "invalid mode line at " + std::to_string(line_no);
                return false;
            }
            continue;
        }

        if (token == "node" || token == "NODE") {
            NodeInfo n;
            if (!(iss >> n.id >> n.host >> n.port)) {
                error = "invalid node line at " + std::to_string(line_no);
                return false;
            }
            out.nodes.push_back(n);
            continue;
        }

        error = "unknown token at line " + std::to_string(line_no) + ": " + token;
        return false;
    }

    if (out.mode.empty()) {
        error = "mode not set";
        return false;
    }
    if (out.nodes.empty()) {
        error = "no nodes found";
        return false;
    }

    return true;
}

} // namespace craq
