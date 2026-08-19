#pragma once

#include <cstdint>
#include <string>

#include "arift_config.h"

namespace arift {

// Public API of the injection core. This is the surface consumed by the
// JNI bridge (jni_bridge.cpp) and the host app.
class AriftCore {
public:
    static AriftCore& instance();

    // Lifecycle
    int init(const std::string& configDir);
    int shutdown();
    int status() const;

    // Attachment
    int attach(int pid, uintptr_t libBase);
    int detach();
    bool isAttached() const { return attached_; }
    int targetPid() const { return pid_; }
    uintptr_t targetBase() const { return lib_base_; }

    // Feature control
    int setFeature(int feature, bool enabled);
    bool isFeatureEnabled(int feature) const;
    uint64_t featuresMask() const;

    // ESP
    int espSetRenderMode(int mode);
    int espSetBoxes(bool v);
    int espSetHealthBars(bool v);
    int espSetNames(bool v);
    int espSetCooldowns(bool v);
    int espSetObjectives(bool v);
    int espSetDistance(bool v);

    // Map hack
    int mapHackSetFogBypass(bool v);
    int mapHackSetMinimapOverride(bool v);
    int mapHackSetVisionRadius(float r);

    // Rank booster
    int rbSetEnabled(bool v);
    int rbSetAggression(int level);
    int rbSetTargetRank(int rank);
    std::string rbSnapshot() const;
    int rbPump();

    // Diagnostics
    std::string lastError() const { return last_error_; }
    void setLastError(const std::string& err) { last_error_ = err; }
    std::string diagDump() const;

    // Version
    static const char* version() { return "1.0.0"; }

private:
    AriftCore() = default;
    bool attached_ = false;
    int pid_ = -1;
    uintptr_t lib_base_ = 0;
    std::string last_error_;
    bool initialized_ = false;
};

}  // namespace arift