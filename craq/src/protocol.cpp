#include "protocol.hpp"

#include <sstream>
#include <vector>

namespace craq {

namespace {

std::string escape_field(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (char c : input) {
        if (c == '\\' || c == '|' || c == '=' || c == '\n' || c == '\r') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    return out;
}

std::string unescape_field(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    bool escaped = false;
    for (char c : input) {
        if (escaped) {
            out.push_back(c);
            escaped = false;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            continue;
        }
        out.push_back(c);
    }
    return out;
}

std::vector<std::string> split_unescaped(const std::string& text, char sep) {
    std::vector<std::string> parts;
    std::string cur;
    bool escaped = false;
    for (char c : text) {
        if (escaped) {
            cur.push_back(c);
            escaped = false;
            continue;
        }
        if (c == '\\') {
            cur.push_back(c);
            escaped = true;
            continue;
        }
        if (c == sep) {
            parts.push_back(cur);
            cur.clear();
            continue;
        }
        cur.push_back(c);
    }
    parts.push_back(cur);
    return parts;
}

bool split_pair(const std::string& text, std::string& key, std::string& value) {
    bool escaped = false;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            continue;
        }
        if (c == '=') {
            key = text.substr(0, i);
            value = text.substr(i + 1);
            return true;
        }
    }
    return false;
}

} // namespace

std::string serialize_message(const Message& msg) {
    std::ostringstream oss;
    oss << escape_field(msg.type);
    for (const auto& kv : msg.fields) {
        oss << "|" << escape_field(kv.first) << "=" << escape_field(kv.second);
    }
    return oss.str();
}

bool parse_message(const std::string& line, Message& out, std::string& error) {
    if (line.empty()) {
        error = "empty message";
        return false;
    }

    auto parts = split_unescaped(line, '|');
    if (parts.empty()) {
        error = "invalid message";
        return false;
    }

    out = Message{};
    out.type = unescape_field(parts[0]);

    for (size_t i = 1; i < parts.size(); ++i) {
        std::string key;
        std::string value;
        if (!split_pair(parts[i], key, value)) {
            error = "invalid field pair";
            return false;
        }
        out.fields[unescape_field(key)] = unescape_field(value);
    }
    return true;
}

std::string get_field_or(const Message& msg, const std::string& key, const std::string& fallback) {
    const auto it = msg.fields.find(key);
    if (it == msg.fields.end()) {
        return fallback;
    }
    return it->second;
}

} // namespace craq
