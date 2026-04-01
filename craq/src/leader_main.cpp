#include <chrono>
#include <ctime>
#include <future>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "config.hpp"
#include "net.hpp"
#include "protocol.hpp"

namespace {

using craq::ClusterConfig;
using craq::Message;
using craq::NodeInfo;

bool parse_message_or_print(const std::string& line, Message& out) {
    std::string err;
    if (!craq::parse_message(line, out, err)) {
        std::cerr << "failed to parse response: " << err << "\n";
        return false;
    }
    return true;
}

std::string endpoint(const std::string& host, int port) {
    return host + ":" + std::to_string(port);
}

std::string now_ts() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t tt = std::chrono::system_clock::to_time_t(now);

    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &tt);
#else
    localtime_r(&tt, &tmv);
#endif

    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch())
                        .count() % 1000;

    std::ostringstream oss;
    oss << std::put_time(&tmv, "%Y-%m-%d %H:%M:%S") << "." << std::setw(3)
        << std::setfill('0') << ms;
    return oss.str();
}

void print_step(const std::string& msg) {
    std::cout << "[" << now_ts() << "] [leader] " << msg << "\n\n";
}

void print_error_step(const std::string& msg) {
    std::cerr << "[" << now_ts() << "] [leader] " << msg << "\n\n";
}

bool send_and_expect_ok(const std::string& host, int port, const Message& request,
                        int timeout_ms, std::string& response_text) {
    print_step("Sending " + request.type + " | route=leader -> " + endpoint(host, port));
    std::string err;
    if (!craq::send_request(host, port, craq::serialize_message(request), timeout_ms, response_text, err)) {
        print_error_step("Request failed | route=leader -> " + endpoint(host, port) + " | error=" + err);
        return false;
    }

    Message resp;
    if (!parse_message_or_print(response_text, resp)) {
        return false;
    }
    if (resp.type != "OK") {
        print_error_step("Request rejected by " + endpoint(host, port) + " | reason=" +
                         craq::get_field_or(resp, "message", "unknown"));
        return false;
    }
    print_step("Received OK | route=" + endpoint(host, port) + " -> leader");
    return true;
}

bool configure_cluster(const std::string& config_path) {
    ClusterConfig cfg;
    std::string err;
    if (!craq::parse_cluster_config(config_path, cfg, err)) {
        print_error_step("Config parse failed: " + err);
        return false;
    }

    if (cfg.mode != "CRAQ") {
        print_error_step("This binary only configures CRAQ mode. Found mode=" + cfg.mode);
        return false;
    }

    const NodeInfo& tail = cfg.nodes.back();

    print_step("Starting cluster CONFIG push"
               " | nodes=" + std::to_string(cfg.nodes.size()) +
               " | head=" + endpoint(cfg.nodes.front().host, cfg.nodes.front().port) +
               " | tail=" + endpoint(tail.host, tail.port));

    for (size_t i = 0; i < cfg.nodes.size(); ++i) {
        const NodeInfo& node = cfg.nodes[i];
        Message m;
        m.type = "CONFIG";
        m.fields["mode"] = "CRAQ";
        m.fields["node_id"] = node.id;
        m.fields["is_head"] = (i == 0) ? "1" : "0";
        m.fields["is_tail"] = (i == cfg.nodes.size() - 1) ? "1" : "0";
        m.fields["tail_host"] = tail.host;
        m.fields["tail_port"] = std::to_string(tail.port);

        if (i + 1 < cfg.nodes.size()) {
            m.fields["next_host"] = cfg.nodes[i + 1].host;
            m.fields["next_port"] = std::to_string(cfg.nodes[i + 1].port);
        } else {
            m.fields["next_host"] = "";
            m.fields["next_port"] = "0";
        }

        if (i > 0) {
            m.fields["prev_host"] = cfg.nodes[i - 1].host;
            m.fields["prev_port"] = std::to_string(cfg.nodes[i - 1].port);
        } else {
            m.fields["prev_host"] = "";
            m.fields["prev_port"] = "0";
        }

        std::string response;
        if (!send_and_expect_ok(node.host, node.port, m, 3000, response)) {
            return false;
        }
        print_step("Configured node"
                   " | node_id=" + node.id +
                   " | endpoint=" + endpoint(node.host, node.port) +
                   " | is_head=" + m.fields["is_head"] +
                   " | is_tail=" + m.fields["is_tail"] +
                   " | prev=" + endpoint(m.fields["prev_host"], std::stoi(m.fields["prev_port"])) +
                   " | next=" + endpoint(m.fields["next_host"], std::stoi(m.fields["next_port"])));
    }

    print_step("CRAQ cluster configured successfully");
    return true;
}

bool wait_for_tail_ack(const std::string& bind_host, int ack_port, const std::string& request_id,
                       int timeout_ms, Message& ack_out) {
    std::promise<Message> prom;
    auto fut = prom.get_future();

    std::atomic<bool> delivered{false};
    craq::TcpServer ack_server(bind_host, ack_port, [&prom, &delivered](const std::string& line) {
        Message m;
        std::string err;
        if (!craq::parse_message(line, m, err)) {
            Message bad;
            bad.type = "ERR";
            bad.fields["message"] = "invalid ACK payload";
            return craq::serialize_message(bad);
        }
        if (m.type == "CLIENT_ACK" && !delivered.exchange(true)) {
            prom.set_value(m);
        }
        Message ok;
        ok.type = "OK";
        ok.fields["message"] = "ack received";
        return craq::serialize_message(ok);
    });

    std::string err;
    std::thread server_thread([&]() {
        ack_server.start(err);
    });

    print_step("ACK listener started"
               " | request_id=" + request_id +
               " | listen_on=" + endpoint(bind_host, ack_port));

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const auto status = fut.wait_for(std::chrono::milliseconds(timeout_ms));
    ack_server.stop();
    server_thread.join();

    if (status != std::future_status::ready) {
        print_error_step("Timeout waiting for tail ACK | request_id=" + request_id);
        return false;
    }

    ack_out = fut.get();
    print_step("Tail ACK received"
               " | request_id=" + request_id +
               " | key=" + craq::get_field_or(ack_out, "key", "") +
               " | version=" + craq::get_field_or(ack_out, "version", ""));
    return true;
}

bool write_key(const std::string& head, const std::string& key, const std::string& value,
               const std::string& leader_host, int ack_port, int timeout_ms) {
    std::string head_host;
    int head_port = 0;
    if (!craq::split_host_port(head, head_host, head_port)) {
        print_error_step("invalid --head host:port");
        return false;
    }

    const std::string request_id = std::to_string(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());

    Message req;
    req.type = "CLIENT_WRITE";
    req.fields["key"] = key;
    req.fields["value"] = value;
    req.fields["client_host"] = leader_host;
    req.fields["client_port"] = std::to_string(ack_port);
    req.fields["request_id"] = request_id;

    Message tail_ack;
    std::thread waiter([&]() {
        wait_for_tail_ack(leader_host, ack_port, request_id, timeout_ms, tail_ack);
    });

    print_step("Dispatching CLIENT_WRITE"
               " | request_id=" + request_id +
               " | key=" + key +
               " | route=leader -> " + endpoint(head_host, head_port) +
               " | ack_callback=" + endpoint(leader_host, ack_port));

    std::string response;
    bool ok = send_and_expect_ok(head_host, head_port, req, timeout_ms, response);
    if (!ok) {
        waiter.join();
        return false;
    }

    waiter.join();

    if (tail_ack.type != "CLIENT_ACK") {
        print_error_step("Tail ACK was not received");
        return false;
    }

    print_step("Write committed"
               " | key=" + craq::get_field_or(tail_ack, "key", "") +
               " | version=" + craq::get_field_or(tail_ack, "version", "") +
               " | request_id=" + request_id);
    return true;
}

bool read_key(const std::string& node, const std::string& key) {
    std::string host;
    int port = 0;
    if (!craq::split_host_port(node, host, port)) {
        print_error_step("invalid --node host:port");
        return false;
    }

    Message req;
    req.type = "CLIENT_READ";
    req.fields["key"] = key;

    std::string err;
    std::string response;
    print_step("Sending CLIENT_READ"
               " | key=" + key +
               " | route=leader -> " + endpoint(host, port));
    if (!craq::send_request(host, port, craq::serialize_message(req), 3000, response, err)) {
        print_error_step("Read request failed | route=leader -> " + endpoint(host, port) + " | error=" + err);
        return false;
    }

    Message resp;
    if (!parse_message_or_print(response, resp)) {
        return false;
    }

    if (resp.type != "READ_OK") {
        print_error_step("Read failed | reason=" + craq::get_field_or(resp, "message", "unknown"));
        return false;
    }

    print_step("Read result"
               " | key=" + key +
               " | value=" + craq::get_field_or(resp, "value", "") +
               " | version=" + craq::get_field_or(resp, "version", "") +
               " | route=" + endpoint(host, port) + " -> leader");
    return true;
}

bool dump_node(const std::string& node) {
    std::string host;
    int port = 0;
    if (!craq::split_host_port(node, host, port)) {
        print_error_step("invalid --node host:port");
        return false;
    }

    Message req;
    req.type = "DUMP";

    std::string err;
    std::string response;
    print_step("Sending DUMP"
               " | route=leader -> " + endpoint(host, port));
    if (!craq::send_request(host, port, craq::serialize_message(req), 3000, response, err)) {
        print_error_step("Dump request failed | route=leader -> " + endpoint(host, port) + " | error=" + err);
        return false;
    }

    Message resp;
    if (!parse_message_or_print(response, resp)) {
        return false;
    }

    if (resp.type != "DUMP_OK") {
        print_error_step("Dump failed | reason=" + craq::get_field_or(resp, "message", "unknown"));
        return false;
    }

    print_step("Dump response"
               " | route=" + endpoint(host, port) + " -> leader" +
               " | node_id=" + craq::get_field_or(resp, "node_id", "") +
               " | is_head=" + craq::get_field_or(resp, "is_head", "") +
               " | is_tail=" + craq::get_field_or(resp, "is_tail", ""));
    std::cout << "state=" << craq::get_field_or(resp, "state", "") << "\n\n";
    return true;
}

std::string get_arg(const std::vector<std::string>& args, const std::string& key,
                    const std::string& fallback = "") {
    for (size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == key) {
            return args[i + 1];
        }
    }
    return fallback;
}

void print_usage() {
    std::cerr << "Usage:\n"
              << "  craq_leader configure --config <path>\n"
              << "  craq_leader write --head <host:port> --key <k> --value <v> --leader-host <ip> --ack-port <p> [--timeout-ms <ms>]\n"
              << "  craq_leader read --node <host:port> --key <k>\n"
              << "  craq_leader dump --node <host:port>\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }

    const std::string cmd = args[0];

    if (cmd == "configure") {
        const std::string config = get_arg(args, "--config", "");
        if (config.empty()) {
            print_usage();
            return 1;
        }
        return configure_cluster(config) ? 0 : 1;
    }

    if (cmd == "write") {
        const std::string head = get_arg(args, "--head", "");
        const std::string key = get_arg(args, "--key", "");
        const std::string value = get_arg(args, "--value", "");
        const std::string leader_host = get_arg(args, "--leader-host", "");
        const std::string ack_port_s = get_arg(args, "--ack-port", "");
        const std::string timeout_s = get_arg(args, "--timeout-ms", "5000");

        if (head.empty() || key.empty() || leader_host.empty() || ack_port_s.empty()) {
            print_usage();
            return 1;
        }

        int ack_port = 0;
        int timeout_ms = 5000;
        try {
            ack_port = std::stoi(ack_port_s);
            timeout_ms = std::stoi(timeout_s);
        } catch (...) {
            std::cerr << "invalid numeric args for --ack-port or --timeout-ms\n";
            return 1;
        }

        return write_key(head, key, value, leader_host, ack_port, timeout_ms) ? 0 : 1;
    }

    if (cmd == "read") {
        const std::string node = get_arg(args, "--node", "");
        const std::string key = get_arg(args, "--key", "");
        if (node.empty() || key.empty()) {
            print_usage();
            return 1;
        }
        return read_key(node, key) ? 0 : 1;
    }

    if (cmd == "dump") {
        const std::string node = get_arg(args, "--node", "");
        if (node.empty()) {
            print_usage();
            return 1;
        }
        return dump_node(node) ? 0 : 1;
    }

    print_usage();
    return 1;
}
