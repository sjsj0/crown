#pragma once

#include <map>
#include <mutex>
#include <string>
#include <unordered_map>

namespace craq {

struct CleanValue {
    bool exists{false};
    int version{0};
    std::string value;
};

class CraqStore {
public:
    int next_version(const std::string& key);
    void apply_dirty(const std::string& key, int version, const std::string& value);
    void mark_clean(const std::string& key, int version);
    CleanValue get_clean(const std::string& key);
    std::string dump_state();

private:
    struct VersionBucket {
        int clean_version{0};
        std::string clean_value;
        std::map<int, std::string> dirty_versions;
    };

    std::mutex mu_;
    std::unordered_map<std::string, int> head_versions_;
    std::unordered_map<std::string, VersionBucket> kv_;
};

} // namespace craq
