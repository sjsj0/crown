#include <csignal>
#include <iostream>
#include <string>
#include <thread>

#include "node_server.hpp"

namespace {

volatile std::sig_atomic_t g_stop = 0;

void signal_handler(int) {
    g_stop = 1;
}

} // namespace

int main(int argc, char** argv) {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    std::string host = "0.0.0.0";
    int port = 5001;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--host" && i + 1 < argc) {
            host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            port = std::stoi(argv[++i]);
        } else {
            std::cerr << "Unknown arg: " << arg << "\n";
            return 1;
        }
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    craq::CraqNodeServer server(host, port);
    std::string error;

    std::cout << "Starting CRAQ node on " << host << ":" << port << "\n";
    if (!server.start(error)) {
        std::cerr << "Server exited with error: " << error << "\n";
        return 1;
    }

    return 0;
}
