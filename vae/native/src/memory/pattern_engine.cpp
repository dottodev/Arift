#include "pattern_engine.h"

#include <cstring>
#include <sstream>

#include "arift_log.h"
#include "arift_utils.h"

namespace arift {

Pattern Pattern::fromString(const std::string& hexWithWildcards) {
    Pattern p;
    std::string cur;
    for (size_t i = 0; i < hexWithWildcards.size(); ++i) {
        char c = hexWithWildcards[i];
        if (c == ' ' || c == '\t' || c == ',') {
            if (!cur.empty()) {
                if (cur == "?" || cur == "??" || cur == "xx") {
                    p.bytes.push_back(0);
                    p.mask.push_back(false);
                } else {
                    uint32_t v = 0;
                    if (sscanf(cur.c_str(), "%x", &v) == 1) {
                        p.bytes.push_back(static_cast<uint8_t>(v & 0xFF));
                        p.mask.push_back(true);
                    }
                }
                cur.clear();
            }
            continue;
        }
        cur.push_back(c);
    }
    if (!cur.empty()) {
        if (cur == "?" || cur == "??" || cur == "xx") {
            p.bytes.push_back(0);
            p.mask.push_back(false);
        } else {
            uint32_t v = 0;
            if (sscanf(cur.c_str(), "%x", &v) == 1) {
                p.bytes.push_back(static_cast<uint8_t>(v & 0xFF));
                p.mask.push_back(true);
            }
        }
    }
    return p;
}

bool Pattern::matches(const uint8_t* data) const {
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (mask[i] && data[i] != bytes[i]) return false;
    }
    return true;
}

std::string Pattern::toString() const {
    std::ostringstream oss;
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (i) oss << " ";
        if (mask[i]) {
            char buf[8];
            snprintf(buf, sizeof(buf), "%02X", bytes[i]);
            oss << buf;
        } else {
            oss << "??";
        }
    }
    return oss.str();
}

bool PatternEngine::matchAt(const uint8_t* region, size_t regionSize, size_t off,
                            const Pattern& pattern) {
    if (off + pattern.size() > regionSize) return false;
    return pattern.matches(region + off);
}

uintptr_t PatternEngine::findFirst(const uint8_t* region, size_t size,
                                   const Pattern& pattern) {
    if (pattern.size() == 0 || size < pattern.size()) return 0;
    size_t limit = size - pattern.size() + 1;
    for (size_t off = 0; off < limit; ++off) {
        if (matchAt(region, size, off, pattern)) {
            return reinterpret_cast<uintptr_t>(region + off);
        }
    }
    return 0;
}

std::vector<uintptr_t> PatternEngine::findAll(const uint8_t* region, size_t size,
                                              const Pattern& pattern,
                                              size_t maxResults) {
    std::vector<uintptr_t> out;
    if (pattern.size() == 0 || size < pattern.size()) return out;
    size_t limit = size - pattern.size() + 1;
    for (size_t off = 0; off < limit && out.size() < maxResults; ++off) {
        if (matchAt(region, size, off, pattern)) {
            out.push_back(reinterpret_cast<uintptr_t>(region + off));
        }
    }
    return out;
}

std::vector<PatternEngine::BatchResult> PatternEngine::scanBatch(
    const uint8_t* region, size_t size,
    const std::vector<const Pattern*>& patterns,
    size_t maxResultsPerPattern) {
    std::vector<BatchResult> results;
    results.reserve(patterns.size());
    for (const auto* p : patterns) {
        BatchResult r;
        r.pattern = p;
        if (p && p->size() > 0 && size >= p->size()) {
            size_t limit = size - p->size() + 1;
            for (size_t off = 0; off < limit && r.hits.size() < maxResultsPerPattern; ++off) {
                if (matchAt(region, size, off, *p)) {
                    r.hits.push_back(reinterpret_cast<uintptr_t>(region + off));
                }
            }
        }
        results.push_back(std::move(r));
    }
    return results;
}

bool PatternEngine::verify(const uint8_t* region, size_t regionSize,
                           uintptr_t addr, const Pattern& pattern) {
    uintptr_t regionStart = reinterpret_cast<uintptr_t>(region);
    uintptr_t regionEnd = regionStart + regionSize;
    if (addr < regionStart || addr + pattern.size() > regionEnd) return false;
    size_t off = addr - regionStart;
    return matchAt(region, regionSize, off, pattern);
}

}  // namespace arift