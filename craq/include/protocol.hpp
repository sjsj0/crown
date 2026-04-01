#pragma once

#include <string>
#include <unordered_map>

namespace craq {

struct Message {
    std::string type;
    std::unordered_map<std::string, std::string> fields;
};

std::string serialize_message(const Message& msg);
bool parse_message(const std::string& line, Message& out, std::string& error);

std::string get_field_or(const Message& msg, const std::string& key, const std::string& fallback = "");

} // namespace craq
