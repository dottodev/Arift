#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "arift_utils.h"

namespace arift {
namespace enemylag {

// Feature identifier used by FeatureSwitch (see feature_switch.h).
constexpr int kFeatureEnemyLag = 5;

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

struct EnemyLagConfig {
    bool enabled = false;
    // Base artificial latency added to enemy packets (milliseconds).
    int delayMs = 300;
    // Random jitter around the base delay (milliseconds).
    int jitterMs = 150;
    // Probability (0..1) that a given enemy packet is delayed at all.
    double applyChance = 0.85;
    // Probability (0..1) that a packet is dropped instead of delayed.
    double dropChance = 0.05;
    // Restrict manipulation to skill/attack opcodes only.
    bool combatOnly = true;
    // Ramp the delay in over the first N seconds of a match.
    int rampSeconds = 20;
    // Periodic pause (ms) where no manipulation occurs.
    int pauseMs = 12000;
    // Cooldown between manipulation bursts (ms).
    int burstCooldownMs = 3000;
    // Cap on how many packets per second are manipulated.
    int maxPacketsPerSec = 40;
    // Whether to obfuscate manipulated frames.
    bool obfuscate = true;
    // Whether to randomize the delay pattern (anti-pattern detection).
    bool randomizePattern = true;
    // In-memory whitelist of opcodes never touched (always forwarded).
    std::vector<uint8_t> safeOpcodes;
};

// ---------------------------------------------------------------------------
// Wire frame model (mirrors the game's packet shape at the proxy layer)
// ---------------------------------------------------------------------------

struct WireFrame {
    uint8_t opcode = 0;
    uint32_t seq = 0;
    uint64_t srcEntity = 0;
    uint64_t dstEntity = 0;
    bool isEnemy = false;
    bool isSkill = false;
    bool isMovement = false;
    int64_t capturedAtMs = 0;
    std::vector<uint8_t> payload;

    uint32_t headerSize() const { return 1 + 4 + 8 + 8; }
};

// A frame scheduled for delayed delivery.
struct PendingFrame {
    WireFrame frame;
    int64_t releaseAtMs = 0;
    int delayMs = 0;
    bool obfuscated = false;
};

// ---------------------------------------------------------------------------
// Per-match enemy tracking
// ---------------------------------------------------------------------------

struct EnemyEntity {
    uint64_t id = 0;
    std::string name;
    bool confirmed = false;
    int64_t firstSeenMs = 0;
    int64_t lastSeenMs = 0;
    int64_t packetsSeen = 0;
    int64_t packetsDelayed = 0;
    int64_t packetsDropped = 0;
    double lagScore = 0.0;
    int roleHint = 0;  // 0 unknown, 1 tank, 2 jungler, 3 mage, 4 mm, 5 support

    int64_t lastAttackMs = 0;
    int64_t lastSkillMs = 0;
    int64_t lastMoveMs = 0;
};

// ---------------------------------------------------------------------------
// Runtime stats (UI / JNI)
// ---------------------------------------------------------------------------

struct EnemyLagStats {
    bool active = false;
    int64_t framesSeen = 0;
    int64_t framesDelayed = 0;
    int64_t framesDropped = 0;
    int64_t framesForwarded = 0;
    int64_t framesObfuscated = 0;
    int64_t enemiesTracked = 0;
    double appliedDelayAvgMs = 0.0;
    double lastMinuteRate = 0.0;
    int64_t lastBurstMs = 0;
    int64_t lastPauseUntilMs = 0;
    int64_t packetsThisSecond = 0;
    int64_t startedAtMs = 0;
};

// ---------------------------------------------------------------------------
// Latency statistics (shared by enemy_lag.cpp / packet_proxy.cpp)
// ---------------------------------------------------------------------------

struct LatencyModel {
    int64_t samples = 0;
    double sumMs = 0.0;
    double sumSqMs = 0.0;
    int maxMs = 0;
    int minMs = 0;

    void add(int ms) {
        double d = static_cast<double>(ms);
        sumMs += d;
        sumSqMs += d * d;
        samples += 1;
        if (minMs == 0 || ms < minMs) minMs = ms;
        if (ms > maxMs) maxMs = ms;
    }

    double meanMs() const {
        return samples > 0 ? sumMs / static_cast<double>(samples) : 0.0;
    }

    double stdMs() const {
        if (samples < 2) return 0.0;
        double m = meanMs();
        double var = sumSqMs / static_cast<double>(samples) - m * m;
        return var > 0.0 ? std::sqrt(var) : 0.0;
    }

    double hitchIndex() const {
        if (maxMs <= 0) return 0.0;
        return utils::clamp(static_cast<double>(maxMs) / 400.0, 0.0, 3.0);
    }
};

LatencyModel& sessionLatencyModel();
bool configFromCacheString(const std::string& s, EnemyLagConfig* out);
std::string diagDump(const EnemyLagConfig& cfg, const EnemyLagStats& stats);

// Session state machine (packet_proxy.cpp).
enum class ProxyState : int {
    kDetached = 0,
    kAttached = 1,
    kRamping = 2,
    kActive = 3,
    kPaused = 4,
    kRecovering = 5,
};

const char* proxyStateName(ProxyState s);
ProxyState deriveState(const EnemyLagStats& s, int64_t nowMs,
                       const EnemyLagConfig& cfg);

// Frame codec helpers (packet_proxy.cpp).
std::vector<uint8_t> obfuscateFrame(const WireFrame& frame, uint64_t key);
bool deobfuscateFrame(const std::vector<uint8_t>& blob, uint64_t key,
                      WireFrame* out);

// Config sanity helpers (packet_proxy.cpp).
void sanitizeConfig(EnemyLagConfig* cfg);
bool configValid(const EnemyLagConfig& cfg);

// Stream telemetry (packet_proxy.cpp).
struct StreamTelemetry {
    int64_t captured = 0;
    int64_t released = 0;
    int64_t held = 0;
    int64_t maxHeldMs = 0;
    double avgHoldMs = 0.0;
};
StreamTelemetry& streamTelemetry();
std::string streamTelemetryLine();
std::string proxyHealthLine(int64_t nowMs);
void resetSubsystems();

// ---------------------------------------------------------------------------
// PacketProxy — intercepts frames, applies policy, re-injects delayed copies
// ---------------------------------------------------------------------------

class PacketProxy {
public:
    static PacketProxy& instance();

    void setConfig(const EnemyLagConfig& cfg);
    const EnemyLagConfig& config() const { return cfg_; }

    void beginMatch(int64_t nowMs);
    void endMatch(int64_t nowMs);

    // Called by the capture hook for every outbound frame.
    // Returns true if the caller should drop the original (rare).
    bool onFrame(const WireFrame& frame);

    // Deliver frames whose release time has passed.
    void pump(int64_t nowMs);

    // Called on a clock tick; enforces pause/cooldown windows.
    void tick(int64_t nowMs);

    // Learning: who is an enemy this match.
    void registerEnemy(uint64_t id, const std::string& name);
    void confirmEnemy(uint64_t id);
    bool isEnemy(uint64_t id) const;
    const std::vector<EnemyEntity>& enemies() const { return enemies_; }
    std::vector<EnemyEntity>& mutableEnemies() { return enemies_; }

    // Policy.
    bool shouldDelay(const WireFrame& frame, int64_t nowMs) const;
    int effectiveDelayMs(const WireFrame& frame, int64_t nowMs) const;
    bool shouldDrop(const WireFrame& frame, int64_t nowMs) const;
    bool inPauseWindow(int64_t nowMs) const;
    bool inBurstCooldown(int64_t nowMs) const;

    // Rate limiting.
    void consumeBudget();
    bool budgetExhausted() const;

    // Callbacks (injected by the host so the proxy stays transport-agnostic).
    std::function<void(const WireFrame&)> onDeliver;

    // Stats.
    EnemyLagStats snapshot() const;
    std::string statusLine() const;

    // Pending delivery queue depth.
    size_t pendingCount() const { return pending_.size(); }

    // Manual pause override (0 = no pause).
    void setPauseFor(int64_t untilMs) { pause_until_ms_ = untilMs; }

    // Anti-pattern: recompute the random seed periodically.
    void reseed(int64_t nowMs);

private:
    PacketProxy() = default;
    EnemyLagConfig cfg_;
    std::vector<EnemyEntity> enemies_;
    std::vector<PendingFrame> pending_;
    int64_t match_start_ms_ = 0;
    int64_t last_burst_ms_ = 0;
    int64_t pause_until_ms_ = 0;
    int64_t second_start_ms_ = 0;
    int64_t packets_this_second_ = 0;
    int64_t frames_seen_ = 0;
    int64_t frames_delayed_ = 0;
    int64_t frames_dropped_ = 0;
    int64_t frames_forwarded_ = 0;
    int64_t frames_obfuscated_ = 0;
    double delay_sum_ms_ = 0.0;
    int delay_count_ = 0;
    uint64_t seed_ = 0x9E3779B97F4A7C15ULL;
};

// ---------------------------------------------------------------------------
// EnemyLag — orchestrates the feature lifecycle and policy decisions
// ---------------------------------------------------------------------------

class EnemyLag {
public:
    static EnemyLag& instance();

    void enable(const EnemyLagConfig& cfg);
    void disable();
    bool enabled() const { return enabled_; }

    // Match lifecycle.
    void onMatchStart(int64_t nowMs);
    void onMatchEnd(int64_t nowMs);

    // Frame path (called from the hook layer via PacketProxy).
    bool onFrame(const WireFrame& frame);

    // UI-facing.
    EnemyLagStats stats() const;
    std::string statusLine() const;
    std::string statsBlob() const;

    // Intensity controls (hold-to-ramp style).
    void setIntensity(int percent);
    int intensity() const { return intensity_; }

    // Target override (delay only a specific enemy id).
    void focusEnemy(uint64_t id);
    void clearFocus();
    uint64_t focusedEnemy() const { return focus_id_; }

    // Periodic upkeep.
    void tick();

private:
    EnemyLag() = default;
    bool enabled_ = false;
    int intensity_ = 100;
    uint64_t focus_id_ = 0;
    EnemyLagConfig cfg_;
    int64_t started_at_ms_ = 0;
    int64_t last_tick_ms_ = 0;
};

}  // namespace enemylag
}  // namespace arift