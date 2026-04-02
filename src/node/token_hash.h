#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace crown::token {

// Stable token hash for CROWN ownership. This must be deterministic across
// processes and platforms, so we avoid std::hash.
inline uint64_t fnv1a64(std::string_view key) {
    constexpr uint64_t kOffsetBasis = 14695981039346656037ull;
    constexpr uint64_t kPrime = 1099511628211ull;

    uint64_t hash = kOffsetBasis;
    for (unsigned char c : key) {
        hash ^= static_cast<uint64_t>(c);
        hash *= kPrime;
    }
    return hash;
}

// Parse decimal or 0x-prefixed hex uint64 token text.
inline bool parse_token(const std::string& text, uint64_t& out) {
    if (text.empty()) return false;

    try {
        size_t idx = 0;
        int base = 10;
        if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
            base = 16;
        }
        const auto parsed = std::stoull(text, &idx, base);
        if (idx != text.size()) return false;
        out = static_cast<uint64_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

// Inclusive token-range check on a uint64 ring. If start > end, the range
// wraps across 2^64-1 back to 0.
inline bool token_in_range(uint64_t token, uint64_t start, uint64_t end) {
    if (start <= end) {
        return token >= start && token <= end;
    }
    return token >= start || token <= end;
}

} // namespace crown::token
