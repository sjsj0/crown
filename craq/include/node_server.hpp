#pragma once

#include <mutex>
#include <string>

#include "store.hpp"

namespace craq {

class CraqNodeServer {
public:
    CraqNodeServer(std::string host, int port);
    bool start(std::string& error);
    void stop();

private:
    struct RuntimeConfig {
        std::string mode{"CRAQ"};
        std::string node_id;
        bool configured{false};
        bool is_head{false};
        bool is_tail{false};
        std::string next_host;
        int next_port{0};
        std::string prev_host;
        int prev_port{0};
        std::string tail_host;
        int tail_port{0};
    };

    std::string on_request_line(const std::string& line);

    std::string handle_config(const std::string& line);
    std::string handle_client_write(const std::string& line);
    std::string handle_repl_write(const std::string& line);
    std::string handle_ack(const std::string& line);
    std::string handle_client_read(const std::string& line);
    std::string handle_tail_read(const std::string& line);
    std::string handle_dump(const std::string& line);

    bool forward_to_next(const std::string& line, std::string& error);
    bool forward_ack_upstream(const std::string& key, int version, std::string& error);
    bool send_client_ack(const std::string& key, int version, const std::string& request_id,
                         const std::string& client_host, int client_port, std::string& error);

    std::string host_;
    int port_;
    CraqStore store_;

    std::mutex cfg_mu_;
    RuntimeConfig cfg_;

    class TcpServerHolder;
    TcpServerHolder* server_holder_;
};

} // namespace craq
