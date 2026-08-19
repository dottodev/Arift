#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace arift {

// Wildcard-capable byte pattern: 0xFF bytes act as wildcards ("?").
struct Pattern {
    std::vector<uint8_t> bytes;   // pattern bytes
    std::vector<bool> mask;       // true = must match

    static Pattern fromString(const std::string& hexWithWildcards);
    bool matches(const uint8_t* data) const;
    size_t size() const { return bytes.size(); }
    std::string toString() const;
};

class PatternEngine {
public:
    // Classic naive search — reliable baseline, slower on huge regions.
    static uintptr_t findFirst(const uint8_t* region, size_t size,
                               const Pattern& pattern);

    // Shift-or style quick search using byte buckets (good for short patterns).
    static std::vector<uintptr_t> findAll(const uint8_t* region, size_t size,
                                          const Pattern& pattern,
                                          size_t maxResults = 64);

    // Multi-pattern batch scan with a single pass (SIMD-friendly ordering).
    struct BatchResult {
        const Pattern* pattern = nullptr;
        std::vector<uintptr_t> hits;
    };
    static std::vector<BatchResult> scanBatch(const uint8_t* region, size_t size,
                                              const std::vector<const Pattern*>& patterns,
                                              size_t maxResultsPerPattern = 32);

    // Verify a candidate address contains the pattern.
    static bool verify(const uint8_t* region, size_t regionSize,
                       uintptr_t addr, const Pattern& pattern);

private:
    static bool matchAt(const uint8_t* region, size_t regionSize, size_t off,
                        const Pattern& pattern);
};

}  // namespace arift