#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

#include "arift_config.h"

namespace arift {

// Thread-safe feature switch board. Single source of truth for what is
// currently active; every module checks through this gate.
class FeatureSwitch {
public:
    static FeatureSwitch& instance();

    int set(int feature, bool enabled);
    bool isEnabled(int feature) const;
    uint64_t mask() const;

    void loadFromConfig();
    void persistAll();
    void reset();

    // Per-feature delta-tick counters for telemetry.
    uint64_t toggleCount(int feature) const;
    uint64_t totalToggles() const;

private:
    FeatureSwitch() = default;

    mutable std::mutex mutex_;
    std::atomic<uint64_t> mask_{0};
    uint64_t toggle_counts_[kFeatureCount + 1] = {0};
};

}  // namespace arift