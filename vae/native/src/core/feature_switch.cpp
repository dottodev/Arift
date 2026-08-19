#include "feature_switch.h"

#include "arift_log.h"

namespace arift {

FeatureSwitch& FeatureSwitch::instance() {
    static FeatureSwitch fs;
    return fs;
}

int FeatureSwitch::set(int feature, bool enabled) {
    if (feature < 1 || feature > kFeatureCount) return -1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (enabled) {
            mask_.fetch_or(1ULL << feature);
        } else {
            mask_.fetch_and(~(1ULL << feature));
        }
        ++toggle_counts_[feature];
    }
    Config::instance().setBool("features", featureName(feature), enabled);
    ARIFT_DEBUG(kTagCore, "Feature %s -> %s",
                featureName(feature), enabled ? "ON" : "OFF");
    return 0;
}

bool FeatureSwitch::isEnabled(int feature) const {
    if (feature < 1 || feature > kFeatureCount) return false;
    return (mask_.load() & (1ULL << feature)) != 0;
}

uint64_t FeatureSwitch::mask() const {
    return mask_.load();
}

void FeatureSwitch::loadFromConfig() {
    uint64_t m = 0;
    for (int f = 1; f <= kFeatureCount; ++f) {
        if (Config::instance().getBool("features", featureName(f), false)) {
            m |= (1ULL << f);
        }
    }
    mask_.store(m);
    ARIFT_DEBUG(kTagCore, "Loaded feature mask 0x%llx from config",
                static_cast<unsigned long long>(m));
}

void FeatureSwitch::persistAll() {
    for (int f = 1; f <= kFeatureCount; ++f) {
        bool enabled = isEnabled(f);
        Config::instance().setBool("features", featureName(f), enabled);
    }
    Config::instance().save();
}

void FeatureSwitch::reset() {
    mask_.store(0);
}

uint64_t FeatureSwitch::toggleCount(int feature) const {
    if (feature < 1 || feature > kFeatureCount) return 0;
    std::lock_guard<std::mutex> lock(mutex_);
    return toggle_counts_[feature];
}

uint64_t FeatureSwitch::totalToggles() const {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t total = 0;
    for (int f = 1; f <= kFeatureCount; ++f) total += toggle_counts_[f];
    return total;
}

}  // namespace arift