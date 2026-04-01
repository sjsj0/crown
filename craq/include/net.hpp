#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace craq {

using RequestHandler = std::function<std::string(const std::string&)>;

class TcpServer {
public:
    TcpServer(std::string bind_host, int bind_port, RequestHandler handler);
    ~TcpServer();

    bool start(std::string& error);
    void stop();

private:
    std::string bind_host_;
    int bind_port_;
    RequestHandler handler_;
    std::intptr_t listen_fd_;
    std::atomic<bool> running_;
};

bool send_request(const std::string& host, int port, const std::string& request_line,
                  int timeout_ms, std::string& response_line, std::string& error);

bool split_host_port(const std::string& text, std::string& host, int& port);

} // namespace craq
