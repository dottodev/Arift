#include "arift_core_api.h"

#include <cstring>

#include "arift_log.h"
#include "arift_utils.h"
#include "feature_switch.h"
#include "memory_scanner.h"

namespace arift {

AriftCore& AriftCore::instance() {
    static AriftCore core;
    return core;
}

int AriftCore::init(const std::string& configDir) {
    if (initialized_) return 0;
    if (!Config::instance().init(configDir)) {
        setLastError("config init failed");
        return -1;
    }
    FeatureSwitch::instance().loadFromConfig();
    initialized_ = true;
    ARIFT_INFO(kTagCore, "Arift core initialized (v%s)", version());
    return 0;
}

int AriftCore::shutdown() {
    if (!initialized_) return 0;
    FeatureSwitch::instance().persistAll();
    Config::instance().shutdown();
    initialized_ = false;
    attached_ = false;
    ARIFT_INFO(kTagCore, "Arift core shut down");
    return 0;
}

int AriftCore::status() const {
    if (!initialized_) return 0;
    if (attached_) return 2;
    return 1;
}

int AriftCore::attach(int pid, uintptr_t libBase) {
    if (!initialized_) {
        setLastError("not initialized");
        return -1;
    }
    if (attached_) {
        setLastError("already attached");
        return -1;
    }
    if (pid <= 0 || libBase == 0) {
        setLastError("invalid pid or base");
        return -1;
    }
    ProcessMemory mem;
    if (!mem.open(pid)) {
        char buf[192];
        snprintf(buf, sizeof(buf), "mem open failed (errno=%d: %s)",
                 mem.lastErrno(), strerror(mem.lastErrno()));
        setLastError(buf);
        ARIFT_ERROR(kTagCore, "attach probe failed: %s", buf);
        return -1;
    }
    uint32_t magic = 0;
    if (!mem.read32(libBase, magic)) {
        char buf[192];
        snprintf(buf, sizeof(buf), "mem read @ base failed (errno=%d: %s)",
                 mem.lastErrno(), strerror(mem.lastErrno()));
        setLastError(buf);
        ARIFT_ERROR(kTagCore, "attach probe failed: %s", buf);
        return -1;
    }
    pid_ = pid;
    lib_base_ = libBase;
    attached_ = true;
    ARIFT_INFO(kTagCore, "Attached to pid=%d base=%llx probe=0x%08x",
               pid, static_cast<unsigned long long>(libBase), magic);
    return 0;
}

int AriftCore::detach() {
    if (!attached_) return 0;
    attached_ = false;
    pid_ = -1;
    lib_base_ = 0;
    ARIFT_INFO(kTagCore, "Detached");
    return 0;
}

int AriftCore::setFeature(int feature, bool enabled) {
    int rc = FeatureSwitch::instance().set(feature, enabled);
    if (rc != 0) setLastError("unknown feature");
    return rc;
}

bool AriftCore::isFeatureEnabled(int feature) const {
    return FeatureSwitch::instance().isEnabled(feature);
}

uint64_t AriftCore::featuresMask() const {
    return FeatureSwitch::instance().mask();
}

int AriftCore::espSetRenderMode(int mode) {
    Config::instance().setInt("esp", "render_mode", mode);
    Config::instance().save();
    return 0;
}

int AriftCore::espSetBoxes(bool v) {
    Config::instance().setBool("esp", "draw_boxes", v);
    Config::instance().save();
    return 0;
}

int AriftCore::espSetHealthBars(bool v) {
    Config::instance().setBool("esp", "draw_health", v);
    Config::instance().save();
    return 0;
}

int AriftCore::espSetNames(bool v) {
    Config::instance().setBool("esp", "draw_names", v);
    Config::instance().save();
    return 0;
}

int AriftCore::espSetCooldowns(bool v) {
    Config::instance().setBool("esp", "draw_cooldowns", v);
    Config::instance().save();
    return 0;
}

int AriftCore::espSetObjectives(bool v) {
    Config::instance().setBool("esp", "draw_objectives", v);
    Config::instance().save();
    return 0;
}

int AriftCore::espSetDistance(bool v) {
    Config::instance().setBool("esp", "draw_distance", v);
    Config::instance().save();
    return 0;
}

int AriftCore::mapHackSetFogBypass(bool v) {
    Config::instance().setBool("maphack", "fog_bypass", v);
    Config::instance().save();
    return 0;
}

int AriftCore::mapHackSetMinimapOverride(bool v) {
    Config::instance().setBool("maphack", "minimap_override", v);
    Config::instance().save();
    return 0;
}

int AriftCore::mapHackSetVisionRadius(float r) {
    Config::instance().setFloat("maphack", "vision_radius", r);
    Config::instance().save();
    return 0;
}

int AriftCore::rbSetEnabled(bool v) {
    Config::instance().setBool("rankbooster", "enabled", v);
    Config::instance().save();
    return 0;
}

int AriftCore::rbSetAggression(int level) {
    Config::instance().setInt("rankbooster", "aggression", level);
    Config::instance().save();
    return 0;
}

int AriftCore::rbSetTargetRank(int rank) {
    Config::instance().setInt("rankbooster", "target_rank", rank);
    Config::instance().save();
    return 0;
}

std::string AriftCore::rbSnapshot() const {
    return "{ \"enabled\": " +
           std::string(FeatureSwitch::instance().isEnabled(kFeatureRankBooster)
                           ? "true" : "false") +
           " }";
}

int AriftCore::rbPump() {
    return 0;
}

std::string AriftCore::diagDump() const {
    std::string out;
    out += "version: " + std::string(version()) + "\n";
    out += "initialized: " + std::string(initialized_ ? "yes" : "no") + "\n";
    out += "attached: " + std::string(attached_ ? "yes" : "no") + "\n";
    if (attached_) {
        out += "pid: " + std::to_string(pid_) + "\n";
        char buf[32];
        snprintf(buf, sizeof(buf), "base: 0x%llx\n",
                 static_cast<unsigned long long>(lib_base_));
        out += buf;
    }
    out += "features: 0x" + utils::hex(featuresMask()) + "\n";
    out += "logs: " + std::to_string(Logger::instance().logCount()) + "\n";
    out += "errors: " + std::to_string(Logger::instance().errorCount()) + "\n";
    return out;
}

}  // namespace arift