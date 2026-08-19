#pragma once

#include <atomic>
#include <cstdint>

#include "arift_thread.h"
#include "esp_config.h"
#include "esp_entities.h"
#include "esp_renderer.h"

namespace arift {

// ESP module entry point: owns the reader + renderer, drives the refresh
// loop on a dedicated thread, and registers itself with the cheat registry.
class EspManager {
public:
    static EspManager& instance();

    int start();
    int stop();
    bool running() const { return running_.load(); }

    EspEntityReader& reader() { return reader_; }
    EspRenderer& renderer() { return renderer_; }
    EspConfig& config() { return config_; }

    // Latest rendered frame (consumed by the overlay adapter).
    const RenderFrame* lastFrame() const;

    // Diagnostics string.
    std::string diag() const;

    // Feature wiring
    void setRenderMode(int mode);
    void setDrawBoxes(bool v);
    void setDrawHealthBars(bool v);
    void setDrawNames(bool v);
    void setDrawCooldowns(bool v);
    void setDrawObjectives(bool v);
    void setDrawDistance(bool v);
    void refreshNow();

    // Metrics
    uint64_t framesRendered() const { return frames_.load(); }
    uint64_t entitiesTracked() const { return entities_.load(); }
    uint64_t refreshCount() const { return refreshes_.load(); }

private:
    EspManager() = default;
    ~EspManager() { stop(); }

    void loop();
    void renderOnce();

    Thread thread_{"arift-esp"};
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> frames_{0};
    std::atomic<uint64_t> entities_{0};
    std::atomic<uint64_t> refreshes_{0};

    EspConfig& config_ = EspConfig::instance();
    EspEntityReader reader_;
    EspRenderer renderer_{&reader_};
    mutable std::mutex frame_mutex_;
    RenderFrame last_frame_;
};

}  // namespace arift