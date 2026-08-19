#include "esp_manager.h"

#include "arift_log.h"
#include "arift_time.h"
#include "arift_utils.h"
#include "cheat_registry.h"
#include "feature_switch.h"

namespace arift {

EspManager& EspManager::instance() {
    static EspManager mgr;
    return mgr;
}

int EspManager::start() {
    if (running_.load()) return 0;

    config_.load();
    reader_.setOffsets(reader_.offsets());

    // Default: simulation mode so the pipeline is testable end-to-end
    // before real offsets land (real mode takes over at attach time).
    reader_.useSimulation(true, 10);
    renderer_.setScreen(1080, 2340);

    running_.store(true);
    if (!thread_.start([this] { loop(); })) {
        running_.store(false);
        return -1;
    }
    ARIFT_INFO(kTagEsp, "ESP module started");
    return 0;
}

int EspManager::stop() {
    if (!running_.load()) return 0;
    running_.store(false);
    thread_.join();
    ARIFT_INFO(kTagEsp, "ESP module stopped");
    return 0;
}

void EspManager::loop() {
    FrameGovernor governor(static_cast<double>(
        config_.settings().maxRenderFps > 0 ? config_.settings().maxRenderFps : 30));

    int64_t lastRefresh = 0;
    while (running_.load()) {
        int64_t now = utils::monotonicMs();
        if (now - lastRefresh >= config_.settings().visionScanMs) {
            lastRefresh = now;
            size_t n = reader_.refresh();
            entities_.store(n);
            refreshes_.fetch_add(1);
        }

        renderOnce();
        governor.tick();
    }
}

void EspManager::renderOnce() {
    if (!FeatureSwitch::instance().isEnabled(kFeatureEsp) &&
        !config_.settings().enabled) {
        return;
    }
    RenderFrame frame = renderer_.render();
    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        last_frame_ = std::move(frame);
    }
    frames_.fetch_add(1);
}

const RenderFrame* EspManager::lastFrame() const {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    return &last_frame_;
}

std::string EspManager::diag() const {
    std::string out;
    out += "esp: running=" + std::string(running_.load() ? "yes" : "no") + "\n";
    out += "  frames=" + std::to_string(frames_.load()) + "\n";
    out += "  refreshes=" + std::to_string(refreshes_.load()) + "\n";
    out += "  entities=" + std::to_string(entities_.load()) + "\n";
    out += "  players=" + std::to_string(reader_.players().size()) + "\n";
    out += "  objectives=" + std::to_string(reader_.objectives().size()) + "\n";
    out += "  sim=" + std::string(reader_.usingSimulation() ? "on" : "off") + "\n";
    out += "  render_fps_target=" +
           std::to_string(config_.settings().maxRenderFps) + "\n";
    return out;
}

void EspManager::setRenderMode(int mode) {
    config_.settings().renderMode = static_cast<EspRenderMode>(mode);
    config_.save();
}

void EspManager::setDrawBoxes(bool v) {
    config_.settings().drawBoxes = v;
    config_.save();
}

void EspManager::setDrawHealthBars(bool v) {
    config_.settings().drawHealthBars = v;
    config_.save();
}

void EspManager::setDrawNames(bool v) {
    config_.settings().drawNames = v;
    config_.save();
}

void EspManager::setDrawCooldowns(bool v) {
    config_.settings().drawCooldowns = v;
    config_.save();
}

void EspManager::setDrawObjectives(bool v) {
    config_.settings().drawObjectives = v;
    config_.save();
}

void EspManager::setDrawDistance(bool v) {
    config_.settings().drawDistance = v;
    config_.save();
}

void EspManager::refreshNow() {
    reader_.refresh();
    renderOnce();
}

}  // namespace arift