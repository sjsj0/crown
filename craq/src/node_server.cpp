#include "node_server.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>

#include "net.hpp"
#include "protocol.hpp"

namespace craq {

namespace {

bool parse_int(const std::string& s, int& out) {
    try {
        out = std::stoi(s);
        return true;
    } catch (...) {
        return false;
    }
}

std::string endpoint(const std::string& host, int port) {
    return host + ":" + std::to_string(port);
}

const char* bool_to_yes_no(bool v) {
    return v ? "yes" : "no";
}

template <typename Cfg>
std::string node_name(const Cfg& cfg) {
    return cfg.node_id.empty() ? "unconfigured" : cfg.node_id;
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

template <typename Cfg>
void log_cfg_line(const Cfg& cfg, const std::string& msg) {
    std::cout << "[" << now_ts() << "] [node " << node_name(cfg) << "] " << msg
              << "\n\n";
}

std::string make_ok(const std::string& msg = "ok") {
    Message m;
    m.type = "OK";
    m.fields["message"] = msg;
    return serialize_message(m);
}

std::string make_err(const std::string& err) {
    Message m;
    m.type = "ERR";
    m.fields["message"] = err;
    return serialize_message(m);
}

} // namespace

class CraqNodeServer::TcpServerHolder {
public:
    explicit TcpServerHolder(std::unique_ptr<TcpServer> server) : server_(std::move(server)) {}
    std::unique_ptr<TcpServer> server_;
};

CraqNodeServer::CraqNodeServer(std::string host, int port)
    : host_(std::move(host)), port_(port), server_holder_(nullptr) {}

bool CraqNodeServer::start(std::string& error) {
    auto server = std::make_unique<TcpServer>(host_, port_, [this](const std::string& line) {
        return this->on_request_line(line);
    });

    server_holder_ = new TcpServerHolder(std::move(server));
    return server_holder_->server_->start(error);
}

void CraqNodeServer::stop() {
    if (server_holder_ != nullptr) {
        server_holder_->server_->stop();
        delete server_holder_;
        server_holder_ = nullptr;
    }
}

std::string CraqNodeServer::on_request_line(const std::string& line) {
    Message msg;
    std::string error;
    if (!parse_message(line, msg, error)) {
        return make_err("parse error: " + error);
    }

    if (msg.type == "CONFIG") {
        return handle_config(line);
    }
    if (msg.type == "CLIENT_WRITE") {
        return handle_client_write(line);
    }
    if (msg.type == "REPL_WRITE") {
        return handle_repl_write(line);
    }
    if (msg.type == "ACK") {
        return handle_ack(line);
    }
    if (msg.type == "CLIENT_READ") {
        return handle_client_read(line);
    }
    if (msg.type == "TAIL_READ") {
        return handle_tail_read(line);
    }
    if (msg.type == "DUMP") {
        return handle_dump(line);
    }

    return make_err("unknown message type: " + msg.type);
}

std::string CraqNodeServer::handle_config(const std::string& line) {
    Message msg;
    std::string error;
    if (!parse_message(line, msg, error)) {
        return make_err(error);
    }

    RuntimeConfig next_cfg;
    next_cfg.mode = get_field_or(msg, "mode", "CRAQ");
    next_cfg.node_id = get_field_or(msg, "node_id", "");
    next_cfg.configured = true;
    next_cfg.is_head = get_field_or(msg, "is_head", "0") == "1";
    next_cfg.is_tail = get_field_or(msg, "is_tail", "0") == "1";
    next_cfg.next_host = get_field_or(msg, "next_host", "");
    next_cfg.prev_host = get_field_or(msg, "prev_host", "");
    next_cfg.tail_host = get_field_or(msg, "tail_host", "");

    if (!parse_int(get_field_or(msg, "next_port", "0"), next_cfg.next_port)) {
        next_cfg.next_port = 0;
    }
    if (!parse_int(get_field_or(msg, "prev_port", "0"), next_cfg.prev_port)) {
        next_cfg.prev_port = 0;
    }
    if (!parse_int(get_field_or(msg, "tail_port", "0"), next_cfg.tail_port)) {
        next_cfg.tail_port = 0;
    }

    {
        std::lock_guard<std::mutex> lock(cfg_mu_);
        cfg_ = next_cfg;
    }

    log_cfg_line(next_cfg, "CONFIG applied"
                             " | mode=" + next_cfg.mode +
                             " | is_head=" + bool_to_yes_no(next_cfg.is_head) +
                             " | is_tail=" + bool_to_yes_no(next_cfg.is_tail) +
                             " | prev=" + endpoint(next_cfg.prev_host, next_cfg.prev_port) +
                             " | next=" + endpoint(next_cfg.next_host, next_cfg.next_port) +
                             " | tail=" + endpoint(next_cfg.tail_host, next_cfg.tail_port));

    return make_ok("configured");
}

std::string CraqNodeServer::handle_client_write(const std::string& line) {
    Message msg;
    std::string error;
    if (!parse_message(line, msg, error)) {
        return make_err(error);
    }

    RuntimeConfig cfg;
    {
        std::lock_guard<std::mutex> lock(cfg_mu_);
        cfg = cfg_;
    }

    if (!cfg.configured) {
        return make_err("node not configured");
    }
    if (cfg.mode != "CRAQ") {
        return make_err("mode not implemented in this binary: " + cfg.mode);
    }
    if (!cfg.is_head) {
        return make_err("CLIENT_WRITE must hit head node");
    }

    const std::string key = get_field_or(msg, "key", "");
    const std::string value = get_field_or(msg, "value", "");
    const std::string client_host = get_field_or(msg, "client_host", "");
    const std::string request_id = get_field_or(msg, "request_id", "");
    int client_port = 0;
    if (key.empty() || client_host.empty() || request_id.empty() ||
        !parse_int(get_field_or(msg, "client_port", "0"), client_port)) {
        return make_err("invalid CLIENT_WRITE payload");
    }

    log_cfg_line(cfg, "CLIENT_WRITE received"
                         " | request_id=" + request_id +
                         " | key=" + key +
                         " | from_client_ack_listener=" + endpoint(client_host, client_port));

    const int version = store_.next_version(key);
    store_.apply_dirty(key, version, value);

    log_cfg_line(cfg, "Assigned version and marked DIRTY"
                         " | key=" + key +
                         " | version=" + std::to_string(version));

    Message repl;
    repl.type = "REPL_WRITE";
    repl.fields["key"] = key;
    repl.fields["value"] = value;
    repl.fields["version"] = std::to_string(version);
    repl.fields["client_host"] = client_host;
    repl.fields["client_port"] = std::to_string(client_port);
    repl.fields["request_id"] = request_id;

    if (cfg.is_tail) {
        log_cfg_line(cfg, "Single-node chain path: this head is also tail");
        store_.mark_clean(key, version);
        log_cfg_line(cfg, "Marked CLEAN at tail"
                             " | key=" + key +
                             " | version=" + std::to_string(version));
        std::string err;
        log_cfg_line(cfg, "Sending CLIENT_ACK"
                             " | request_id=" + request_id +
                             " | to=" + endpoint(client_host, client_port));
        if (!send_client_ack(key, version, request_id, client_host, client_port, err)) {
            return make_err("tail ack to client failed: " + err);
        }
        log_cfg_line(cfg, "CLIENT_ACK delivered"
                             " | request_id=" + request_id +
                             " | key=" + key +
                             " | version=" + std::to_string(version));
        return make_ok("write committed on single-node chain");
    }

    if (cfg.next_host.empty() || cfg.next_port == 0) {
        return make_err("head missing next node config");
    }

    std::string forward_err;
    log_cfg_line(cfg, "Forwarding REPL_WRITE"
                         " | request_id=" + request_id +
                         " | key=" + key +
                         " | version=" + std::to_string(version) +
                         " | route=" + endpoint(host_, port_) + " -> " + endpoint(cfg.next_host, cfg.next_port));
    if (!forward_to_next(serialize_message(repl), forward_err)) {
        return make_err("forward to next failed: " + forward_err);
    }

    log_cfg_line(cfg, "Forward acknowledged by next hop"
                         " | request_id=" + request_id +
                         " | key=" + key +
                         " | version=" + std::to_string(version));

    Message ok;
    ok.type = "OK";
    ok.fields["message"] = "write accepted";
    ok.fields["version"] = std::to_string(version);
    return serialize_message(ok);
}

std::string CraqNodeServer::handle_repl_write(const std::string& line) {
    Message msg;
    std::string error;
    if (!parse_message(line, msg, error)) {
        return make_err(error);
    }

    RuntimeConfig cfg;
    {
        std::lock_guard<std::mutex> lock(cfg_mu_);
        cfg = cfg_;
    }

    if (!cfg.configured) {
        return make_err("node not configured");
    }
    if (cfg.mode != "CRAQ") {
        return make_err("mode not implemented in this binary: " + cfg.mode);
    }

    const std::string key = get_field_or(msg, "key", "");
    const std::string value = get_field_or(msg, "value", "");
    const std::string client_host = get_field_or(msg, "client_host", "");
    const std::string request_id = get_field_or(msg, "request_id", "");
    int version = 0;
    int client_port = 0;
    if (key.empty() || request_id.empty() || client_host.empty() ||
        !parse_int(get_field_or(msg, "version", "0"), version) ||
        !parse_int(get_field_or(msg, "client_port", "0"), client_port)) {
        return make_err("invalid REPL_WRITE payload");
    }

    log_cfg_line(cfg, "REPL_WRITE received"
                         " | request_id=" + request_id +
                         " | key=" + key +
                         " | version=" + std::to_string(version));

    store_.apply_dirty(key, version, value);

    log_cfg_line(cfg, "Marked DIRTY"
                         " | key=" + key +
                         " | version=" + std::to_string(version));

    if (!cfg.is_tail) {
        if (cfg.next_host.empty() || cfg.next_port == 0) {
            return make_err("intermediate node missing next config");
        }
        std::string forward_err;
        log_cfg_line(cfg, "Forwarding REPL_WRITE to next"
                             " | request_id=" + request_id +
                             " | key=" + key +
                             " | version=" + std::to_string(version) +
                             " | route=" + endpoint(host_, port_) + " -> " + endpoint(cfg.next_host, cfg.next_port));
        if (!forward_to_next(line, forward_err)) {
            return make_err("forward to next failed: " + forward_err);
        }
        log_cfg_line(cfg, "Next hop accepted REPL_WRITE"
                             " | request_id=" + request_id +
                             " | key=" + key +
                             " | version=" + std::to_string(version));
        return make_ok("repl forwarded");
    }

    store_.mark_clean(key, version);
    log_cfg_line(cfg, "TAIL committed version"
                         " | request_id=" + request_id +
                         " | key=" + key +
                         " | version=" + std::to_string(version));

    std::string up_err;
    if (!cfg.is_head) {
        log_cfg_line(cfg, "Starting ACK back-propagation"
                             " | key=" + key +
                             " | version=" + std::to_string(version) +
                             " | route=" + endpoint(host_, port_) + " -> " + endpoint(cfg.prev_host, cfg.prev_port));
        if (!forward_ack_upstream(key, version, up_err)) {
            return make_err("upstream ACK failed: " + up_err);
        }
        log_cfg_line(cfg, "Upstream ACK accepted"
                             " | key=" + key +
                             " | version=" + std::to_string(version));
    }

    std::string client_ack_err;
    log_cfg_line(cfg, "Sending CLIENT_ACK from tail"
                         " | request_id=" + request_id +
                         " | to=" + endpoint(client_host, client_port));
    if (!send_client_ack(key, version, request_id, client_host, client_port, client_ack_err)) {
        return make_err("client ACK failed: " + client_ack_err);
    }

    log_cfg_line(cfg, "CLIENT_ACK delivered"
                         " | request_id=" + request_id +
                         " | key=" + key +
                         " | version=" + std::to_string(version));

    return make_ok("tail committed and acked");
}

std::string CraqNodeServer::handle_ack(const std::string& line) {
    Message msg;
    std::string error;
    if (!parse_message(line, msg, error)) {
        return make_err(error);
    }

    RuntimeConfig cfg;
    {
        std::lock_guard<std::mutex> lock(cfg_mu_);
        cfg = cfg_;
    }

    if (!cfg.configured) {
        return make_err("node not configured");
    }

    const std::string key = get_field_or(msg, "key", "");
    int version = 0;
    if (key.empty() || !parse_int(get_field_or(msg, "version", "0"), version)) {
        return make_err("invalid ACK payload");
    }

    log_cfg_line(cfg, "ACK received from downstream"
                         " | key=" + key +
                         " | version=" + std::to_string(version));

    store_.mark_clean(key, version);

    log_cfg_line(cfg, "Marked CLEAN after ACK"
                         " | key=" + key +
                         " | version=" + std::to_string(version));

    if (!cfg.is_head) {
        std::string up_err;
        log_cfg_line(cfg, "Forwarding ACK upstream"
                             " | key=" + key +
                             " | version=" + std::to_string(version) +
                             " | route=" + endpoint(host_, port_) + " -> " + endpoint(cfg.prev_host, cfg.prev_port));
        if (!forward_ack_upstream(key, version, up_err)) {
            return make_err("upstream ACK failed: " + up_err);
        }
        log_cfg_line(cfg, "Upstream ACK accepted"
                             " | key=" + key +
                             " | version=" + std::to_string(version));
    }

    return make_ok("ack applied");
}

std::string CraqNodeServer::handle_client_read(const std::string& line) {
    Message msg;
    std::string error;
    if (!parse_message(line, msg, error)) {
        return make_err(error);
    }

    RuntimeConfig cfg;
    {
        std::lock_guard<std::mutex> lock(cfg_mu_);
        cfg = cfg_;
    }

    if (!cfg.configured) {
        return make_err("node not configured");
    }
    if (cfg.mode != "CRAQ") {
        return make_err("mode not implemented in this binary: " + cfg.mode);
    }

    const std::string key = get_field_or(msg, "key", "");
    if (key.empty()) {
        return make_err("missing key");
    }

    log_cfg_line(cfg, "CLIENT_READ received"
                         " | key=" + key +
                         " | node_role=" + std::string(cfg.is_tail ? "tail" : (cfg.is_head ? "head" : "middle")));

    if (cfg.is_tail) {
        CleanValue cv = store_.get_clean(key);
        if (!cv.exists) {
            return make_err("key not found");
        }
        log_cfg_line(cfg, "Serving READ locally from tail"
                             " | key=" + key +
                             " | version=" + std::to_string(cv.version));
        Message out;
        out.type = "READ_OK";
        out.fields["key"] = key;
        out.fields["version"] = std::to_string(cv.version);
        out.fields["value"] = cv.value;
        return serialize_message(out);
    }

    Message tail_req;
    tail_req.type = "TAIL_READ";
    tail_req.fields["key"] = key;

    log_cfg_line(cfg, "Forwarding read-consistency check to tail"
                         " | key=" + key +
                         " | route=" + endpoint(host_, port_) + " -> " + endpoint(cfg.tail_host, cfg.tail_port));

    std::string resp;
    if (!send_request(cfg.tail_host, cfg.tail_port, serialize_message(tail_req), 2000, resp, error)) {
        return make_err("tail query failed: " + error);
    }

    log_cfg_line(cfg, "Tail returned latest clean version"
                         " | key=" + key);
    return resp;
}

std::string CraqNodeServer::handle_tail_read(const std::string& line) {
    Message msg;
    std::string error;
    if (!parse_message(line, msg, error)) {
        return make_err(error);
    }

    RuntimeConfig cfg;
    {
        std::lock_guard<std::mutex> lock(cfg_mu_);
        cfg = cfg_;
    }

    if (!cfg.is_tail) {
        return make_err("TAIL_READ allowed only on tail");
    }

    const std::string key = get_field_or(msg, "key", "");
    if (key.empty()) {
        return make_err("missing key");
    }

    log_cfg_line(cfg, "TAIL_READ received"
                         " | key=" + key +
                         " | serving from tail clean state");

    CleanValue cv = store_.get_clean(key);
    if (!cv.exists) {
        return make_err("key not found");
    }

    Message out;
    out.type = "READ_OK";
    out.fields["key"] = key;
    out.fields["version"] = std::to_string(cv.version);
    out.fields["value"] = cv.value;
    return serialize_message(out);
}

std::string CraqNodeServer::handle_dump(const std::string&) {
    Message out;
    out.type = "DUMP_OK";
    out.fields["state"] = store_.dump_state();

    RuntimeConfig cfg;
    {
        std::lock_guard<std::mutex> lock(cfg_mu_);
        cfg = cfg_;
    }
    out.fields["node_id"] = cfg.node_id;
    out.fields["mode"] = cfg.mode;
    out.fields["is_head"] = cfg.is_head ? "1" : "0";
    out.fields["is_tail"] = cfg.is_tail ? "1" : "0";

    return serialize_message(out);
}

bool CraqNodeServer::forward_to_next(const std::string& line, std::string& error) {
    RuntimeConfig cfg;
    {
        std::lock_guard<std::mutex> lock(cfg_mu_);
        cfg = cfg_;
    }

    std::string resp;
    Message in;
    std::string parse_err;
    if (parse_message(line, in, parse_err)) {
        log_cfg_line(cfg, "Outgoing message to next"
                             " | type=" + in.type +
                             " | route=" + endpoint(host_, port_) + " -> " + endpoint(cfg.next_host, cfg.next_port));
    }
    if (!send_request(cfg.next_host, cfg.next_port, line, 2000, resp, error)) {
        return false;
    }

    Message m;
    if (!parse_message(resp, m, error)) {
        error = "invalid response from next node";
        return false;
    }
    if (m.type != "OK") {
        error = get_field_or(m, "message", "unknown error");
        return false;
    }
    log_cfg_line(cfg, "Next hop responded OK"
                         " | route=" + endpoint(cfg.next_host, cfg.next_port) + " -> " + endpoint(host_, port_));
    return true;
}

bool CraqNodeServer::forward_ack_upstream(const std::string& key, int version, std::string& error) {
    RuntimeConfig cfg;
    {
        std::lock_guard<std::mutex> lock(cfg_mu_);
        cfg = cfg_;
    }

    if (cfg.prev_host.empty() || cfg.prev_port == 0) {
        error = "prev node not configured";
        return false;
    }

    Message ack;
    ack.type = "ACK";
    ack.fields["key"] = key;
    ack.fields["version"] = std::to_string(version);

    std::string resp;
    log_cfg_line(cfg, "Sending ACK upstream"
                         " | key=" + key +
                         " | version=" + std::to_string(version) +
                         " | route=" + endpoint(host_, port_) + " -> " + endpoint(cfg.prev_host, cfg.prev_port));
    if (!send_request(cfg.prev_host, cfg.prev_port, serialize_message(ack), 2000, resp, error)) {
        return false;
    }

    Message m;
    if (!parse_message(resp, m, error)) {
        error = "invalid ACK response";
        return false;
    }
    if (m.type != "OK") {
        error = get_field_or(m, "message", "upstream ACK rejected");
        return false;
    }
    log_cfg_line(cfg, "Upstream node accepted ACK"
                         " | key=" + key +
                         " | version=" + std::to_string(version));
    return true;
}

bool CraqNodeServer::send_client_ack(const std::string& key, int version, const std::string& request_id,
                                     const std::string& client_host, int client_port, std::string& error) {
    Message ack;
    ack.type = "CLIENT_ACK";
    ack.fields["status"] = "ok";
    ack.fields["key"] = key;
    ack.fields["version"] = std::to_string(version);
    ack.fields["request_id"] = request_id;

    std::string resp;
    RuntimeConfig cfg;
    {
        std::lock_guard<std::mutex> lock(cfg_mu_);
        cfg = cfg_;
    }
    log_cfg_line(cfg, "Sending CLIENT_ACK callback"
                         " | request_id=" + request_id +
                         " | route=" + endpoint(host_, port_) + " -> " + endpoint(client_host, client_port));
    if (!send_request(client_host, client_port, serialize_message(ack), 2000, resp, error)) {
        return false;
    }

    Message m;
    if (!parse_message(resp, m, error)) {
        error = "client ACK listener returned invalid payload";
        return false;
    }
    if (m.type != "OK") {
        error = get_field_or(m, "message", "client did not accept ACK");
        return false;
    }

    log_cfg_line(cfg, "Client accepted ACK"
                         " | request_id=" + request_id +
                         " | key=" + key +
                         " | version=" + std::to_string(version));

    return true;
}

} // namespace craq
