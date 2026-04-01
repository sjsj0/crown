#include "store.hpp"

#include <sstream>

namespace craq {

int CraqStore::next_version(const std::string& key) {
    std::lock_guard<std::mutex> lock(mu_);
    int& v = head_versions_[key];
    v += 1;
    return v;
}

void CraqStore::apply_dirty(const std::string& key, int version, const std::string& value) {
    std::lock_guard<std::mutex> lock(mu_);
    kv_[key].dirty_versions[version] = value;
}

void CraqStore::mark_clean(const std::string& key, int version) {
    std::lock_guard<std::mutex> lock(mu_);
    VersionBucket& b = kv_[key];
    auto it = b.dirty_versions.find(version);
    if (it != b.dirty_versions.end()) {
        b.clean_version = version;
        b.clean_value = it->second;
    } else if (version > b.clean_version) {
        b.clean_version = version;
    }

    auto erase_it = b.dirty_versions.begin();
    while (erase_it != b.dirty_versions.end() && erase_it->first <= version) {
        erase_it = b.dirty_versions.erase(erase_it);
    }
}

CleanValue CraqStore::get_clean(const std::string& key) {
    std::lock_guard<std::mutex> lock(mu_);
    CleanValue out;
    const auto it = kv_.find(key);
    if (it == kv_.end() || it->second.clean_version == 0) {
        return out;
    }
    out.exists = true;
    out.version = it->second.clean_version;
    out.value = it->second.clean_value;
    return out;
}

std::string CraqStore::dump_state() {
    std::lock_guard<std::mutex> lock(mu_);
    std::ostringstream oss;
    bool first_key = true;
    for (const auto& kvp : kv_) {
        if (!first_key) {
            oss << ";";
        }
        first_key = false;

        oss << kvp.first << "{clean_v=" << kvp.second.clean_version << ",clean_val=" << kvp.second.clean_value
            << ",dirty=[";

        bool first_dirty = true;
        for (const auto& dv : kvp.second.dirty_versions) {
            if (!first_dirty) {
                oss << ",";
            }
            first_dirty = false;
            oss << dv.first << ":" << dv.second;
        }
        oss << "]}";
    }
    return oss.str();
}

} // namespace craq
