#pragma once

#include <atomic>
#include <cstdint>

#include "arift_thread.h"
#include "esp_types.h"
#include "fog_bypass.h"
#include "maphack_types.h"
#include "minimap_override.h"

namespace arift {

// Map hack module: coordinates the fog bypass + minimap override and runs
// its vision-recompute loop.
class MapHackManager {
public:
    static MapHackManager& instance();

    int start();
    int stop();
    bool running() const { return running_.load(); }

    FogBypass& fogBypass() { return fog_; }
    MinimapOverride& minimap() { return minimap_; }

    // Feed entity data from the ESP reader (shared memory model).
    void setEntities(const std::vector<PlayerSnapshot>& players,
                     const std::vector<ObjectiveSnapshot>& objectives);

    void setFogBypass(bool v);
    void setMinimapOverride(bool v);
    void setVisionRadius(float radius);

    // Diagnostics
    std::string diag() const;

    // Metrics
    uint64_t framesProcessed() const { return frames_.load(); }

private:
    MapHackManager() = default;
    ~MapHackManager() { stop(); }

    void loop();

    Thread thread_{"arift-maphack"};
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> frames_{0};

    FogBypass fog_;
    MinimapOverride minimap_;
    MapHackSettings settings_;

    // Shared entity snapshots (guarded by mutex).
    std::mutex entity_mutex_;
    std::vector<PlayerSnapshot> players_;
    std::vector<ObjectiveSnapshot> objectives_;
};

}  // namespace arift