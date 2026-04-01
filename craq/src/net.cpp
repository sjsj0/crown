#include "net.hpp"

#include <cerrno>
#include <cstring>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include <chrono>
#include <mutex>
#include <thread>

namespace craq {

namespace {

#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
#endif

#ifdef _WIN32
bool ensure_winsock(std::string& error) {
    static bool initialized = false;
    static std::mutex mu;
    std::lock_guard<std::mutex> lock(mu);
    if (initialized) {
        return true;
    }
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        error = "WSAStartup failed";
        return false;
    }
    initialized = true;
    return true;
}
#endif

void close_socket(SocketHandle fd) {
#ifdef _WIN32
    closesocket(fd);
#else
    ::close(fd);
#endif
}

bool send_all(SocketHandle fd, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        int n = ::send(fd, data.data() + static_cast<int>(sent), static_cast<int>(data.size() - sent), 0);
        if (n <= 0) {
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool recv_line(SocketHandle fd, std::string& out) {
    out.clear();
    char ch = '\0';
    while (true) {
        int n = ::recv(fd, &ch, 1, 0);
        if (n <= 0) {
            return false;
        }
        if (ch == '\n') {
            return true;
        }
        out.push_back(ch);
    }
}

SocketHandle open_client_socket(const std::string& host, int port, int timeout_ms, std::string& error) {
#ifdef _WIN32
    if (!ensure_winsock(error)) {
        return kInvalidSocket;
    }
#endif

    struct addrinfo hints {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* result = nullptr;
    const int g = ::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &result);
    if (g != 0) {
        error = std::string("getaddrinfo failed: ") + gai_strerror(g);
        return kInvalidSocket;
    }

    SocketHandle fd = kInvalidSocket;
    for (auto* rp = result; rp != nullptr; rp = rp->ai_next) {
        fd = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd == kInvalidSocket) {
            continue;
        }

#ifdef _WIN32
        DWORD tv = static_cast<DWORD>(timeout_ms);
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
        timeval tv {};
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

        if (::connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            break;
        }

        close_socket(fd);
        fd = kInvalidSocket;
    }

    ::freeaddrinfo(result);

    if (fd == kInvalidSocket) {
#ifdef _WIN32
        error = "connect failed to " + host + ":" + std::to_string(port) + " err=" + std::to_string(WSAGetLastError());
#else
        error = "connect failed to " + host + ":" + std::to_string(port) + " err=" + std::strerror(errno);
#endif
        return kInvalidSocket;
    }

    return fd;
}

} // namespace

TcpServer::TcpServer(std::string bind_host, int bind_port, RequestHandler handler)
    : bind_host_(std::move(bind_host)),
      bind_port_(bind_port),
      handler_(std::move(handler)),
    listen_fd_(static_cast<std::intptr_t>(kInvalidSocket)),
      running_(false) {}

TcpServer::~TcpServer() {
    stop();
}

bool TcpServer::start(std::string& error) {
#ifdef _WIN32
    if (!ensure_winsock(error)) {
        return false;
    }
#endif

    if (running_.load()) {
        return true;
    }

    SocketHandle listen_socket = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_socket == kInvalidSocket) {
        error = std::string("socket failed: ") + std::strerror(errno);
        return false;
    }
    listen_fd_ = static_cast<std::intptr_t>(listen_socket);

    int yes = 1;
#ifdef _WIN32
    ::setsockopt(static_cast<SocketHandle>(listen_fd_), SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
#else
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#endif

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(bind_port_));

    if (bind_host_ == "0.0.0.0" || bind_host_ == "*") {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else if (::inet_pton(AF_INET, bind_host_.c_str(), &addr.sin_addr) != 1) {
        error = "invalid bind host: " + bind_host_;
        close_socket(static_cast<SocketHandle>(listen_fd_));
        listen_fd_ = static_cast<std::intptr_t>(kInvalidSocket);
        return false;
    }

    if (::bind(static_cast<SocketHandle>(listen_fd_), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        error = std::string("bind failed: ") + std::strerror(errno);
        close_socket(static_cast<SocketHandle>(listen_fd_));
        listen_fd_ = static_cast<std::intptr_t>(kInvalidSocket);
        return false;
    }

    if (::listen(static_cast<SocketHandle>(listen_fd_), 128) != 0) {
        error = std::string("listen failed: ") + std::strerror(errno);
        close_socket(static_cast<SocketHandle>(listen_fd_));
        listen_fd_ = static_cast<std::intptr_t>(kInvalidSocket);
        return false;
    }

    running_.store(true);

    while (running_.load()) {
        sockaddr_in peer {};
        socklen_t peer_len = sizeof(peer);
        const SocketHandle client_fd = ::accept(static_cast<SocketHandle>(listen_fd_), reinterpret_cast<sockaddr*>(&peer), &peer_len);
    #ifdef _WIN32
        if (client_fd == INVALID_SOCKET) {
    #else
        if (client_fd < 0) {
    #endif
            if (!running_.load()) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        std::thread([this, client_fd]() {
            std::string req;
            std::string resp = "ERR|message=failed to read request";
            if (recv_line(client_fd, req)) {
                resp = handler_(req);
            }
            resp.push_back('\n');
            send_all(client_fd, resp);
            close_socket(client_fd);
        }).detach();
    }

    return true;
}

void TcpServer::stop() {
    running_.store(false);
    if (static_cast<SocketHandle>(listen_fd_) != kInvalidSocket) {
        SocketHandle s = static_cast<SocketHandle>(listen_fd_);
#ifdef _WIN32
        ::shutdown(s, SD_BOTH);
#else
        ::shutdown(s, SHUT_RDWR);
#endif
        close_socket(s);
        listen_fd_ = static_cast<std::intptr_t>(kInvalidSocket);
    }
}

bool send_request(const std::string& host, int port, const std::string& request_line,
                  int timeout_ms, std::string& response_line, std::string& error) {
    SocketHandle fd = open_client_socket(host, port, timeout_ms, error);
    if (fd == kInvalidSocket) {
        return false;
    }

    std::string payload = request_line;
    payload.push_back('\n');
    if (!send_all(fd, payload)) {
        error = "send failed";
        close_socket(fd);
        return false;
    }

    if (!recv_line(fd, response_line)) {
        error = "recv failed";
        close_socket(fd);
        return false;
    }

    close_socket(fd);
    return true;
}

bool split_host_port(const std::string& text, std::string& host, int& port) {
    const size_t pos = text.rfind(':');
    if (pos == std::string::npos || pos == 0 || pos + 1 >= text.size()) {
        return false;
    }

    host = text.substr(0, pos);
    try {
        port = std::stoi(text.substr(pos + 1));
    } catch (...) {
        return false;
    }

    return port > 0 && port < 65536;
}

} // namespace craq
