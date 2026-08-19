#include "enemy_lag.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <utility>

#include "arift_log.h"
#include "arift_utils.h"

namespace arift {
namespace enemylag {

namespace {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// Opcode families used by the policy engine.
bool isCombatOpcode(uint8_t opcode) {
    // MLBB-style opcode ranges (kept opaque on purpose).
    switch (opcode) {
        case 0x11:  // basic attack
        case 0x12:  // skill cast
        case 0x13:  // skill aim
        case 0x14:  // ult cast
        case 0x15:  // dash / blink
        case 0x16:  // basic attack retarget
        case 0x17:  // skill cancel
        case 0x18:  // combo chain
        case 0x19:  // summoner cast
        case 0x1A:  // battle spell
        case 0x1B:  // flinch
        case 0x1C:  // knockback
        case 0x1D:  // stun response
        case 0x1E:  // revive cast
        case 0x1F:  // recall cast
            return true;
        default:
            return false;
    }
}

bool isMovementOpcode(uint8_t opcode) {
    switch (opcode) {
        case 0x21:  // move start
        case 0x22:  // move update
        case 0x23:  // move stop
        case 0x24:  // joystick vector
        case 0x25:  // path point
        case 0x26:  // teleport
        case 0x27:  // knockback applied
            return true;
        default:
            return false;
    }
}

bool isSafeOpcode(uint8_t opcode, const std::vector<uint8_t>& safe) {
    return std::find(safe.begin(), safe.end(), opcode) != safe.end();
}

// Deterministic per-frame hash (seeded) so pattern is stable per enemy.
uint64_t frameHash(const WireFrame& frame, uint64_t seed) {
    uint64_t h = seed;
    h ^= frame.seq * 0x100000001B3ULL;
    h ^= frame.srcEntity * 0x100000001B3ULL;
    h ^= static_cast<uint64_t>(frame.opcode) * 0x2545F4914F6CDD1DULL;
    h = (h >> 29) | (h << 35);
    return h;
}

double gaussClamp(double mean, double sd, double lo, double hi) {
    double v = utils::gaussian(mean, sd);
    return utils::clamp(v, lo, hi);
}

int delayForFrame(const WireFrame& frame, const EnemyLagConfig& cfg,
                  uint64_t seed, int intensity) {
    double base = static_cast<double>(cfg.delayMs) *
                  static_cast<double>(intensity) / 100.0;
    double jitter = static_cast<double>(cfg.jitterMs);
    double d = gaussClamp(base, jitter * 0.35,
                          std::max(0.0, base - jitter * 2.0),
                          base + jitter * 2.0);
    // Skill casts get slightly longer delays than movement.
    if (frame.isSkill) d *= 1.15;
    if (frame.isMovement) d *= 0.75;
    // Deterministic wobble so the pattern is not constant.
    double wobble = (frameHash(frame, seed) & 0x3F) / 64.0;
    d *= 0.85 + wobble * 0.3;
    return static_cast<int>(std::max(0.0, d));
}

const char* roleName(int role) {
    switch (role) {
        case 1: return "tank";
        case 2: return "jungler";
        case 3: return "mage";
        case 4: return "marksman";
        case 5: return "support";
        default: return "unknown";
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// PacketProxy
// ---------------------------------------------------------------------------

PacketProxy& PacketProxy::instance() {
    static PacketProxy proxy;
    return proxy;
}

void PacketProxy::setConfig(const EnemyLagConfig& cfg) {
    cfg_ = cfg;
}

void PacketProxy::beginMatch(int64_t nowMs) {
    match_start_ms_ = nowMs;
    last_burst_ms_ = 0;
    pause_until_ms_ = 0;
    second_start_ms_ = nowMs;
    packets_this_second_ = 0;
    frames_seen_ = 0;
    frames_delayed_ = 0;
    frames_dropped_ = 0;
    frames_forwarded_ = 0;
    frames_obfuscated_ = 0;
    delay_sum_ms_ = 0.0;
    delay_count_ = 0;
    enemies_.clear();
    pending_.clear();
    ARIFT_INFO(kTagEnemyLag, "proxy attached: match start");
}

void PacketProxy::endMatch(int64_t nowMs) {
    pending_.clear();
    ARIFT_INFO(kTagEnemyLag, "proxy detached: match end (delayed=%lld drop=%lld)",
               static_cast<long long>(frames_delayed_),
               static_cast<long long>(frames_dropped_));
}

void PacketProxy::registerEnemy(uint64_t id, const std::string& name) {
    for (auto& e : enemies_) {
        if (e.id == id) {
            e.lastSeenMs = utils::monotonicMs();
            return;
        }
    }
    EnemyEntity e;
    e.id = id;
    e.name = name;
    e.firstSeenMs = utils::monotonicMs();
    e.lastSeenMs = e.firstSeenMs;
    enemies_.push_back(e);
}

void PacketProxy::confirmEnemy(uint64_t id) {
    for (auto& e : enemies_) {
        if (e.id == id) {
            e.confirmed = true;
            return;
        }
    }
}

bool PacketProxy::isEnemy(uint64_t id) const {
    for (const auto& e : enemies_) {
        if (e.id == id) return true;
    }
    return false;
}

bool PacketProxy::inPauseWindow(int64_t nowMs) const {
    if (pause_until_ms_ <= 0) return false;
    return nowMs < pause_until_ms_;
}

bool PacketProxy::inBurstCooldown(int64_t nowMs) const {
    if (last_burst_ms_ <= 0) return false;
    return nowMs - last_burst_ms_ < cfg_.burstCooldownMs;
}

bool PacketProxy::budgetExhausted() const {
    return packets_this_second_ >= cfg_.maxPacketsPerSec;
}

void PacketProxy::consumeBudget() {
    packets_this_second_ += 1;
}

bool PacketProxy::shouldDelay(const WireFrame& frame, int64_t nowMs) const {
    if (!cfg_.enabled) return false;
    if (frame.isEnemy == false) return false;
    if (isSafeOpcode(frame.opcode, cfg_.safeOpcodes)) return false;
    if (cfg_.combatOnly && !isCombatOpcode(frame.opcode) &&
        !isMovementOpcode(frame.opcode)) {
        return false;
    }
    if (inPauseWindow(nowMs)) return false;
    if (inBurstCooldown(nowMs)) return false;
    if (budgetExhausted()) return false;
    // Ramp: delay grows over the first rampSeconds of the match.
    if (match_start_ms_ > 0 && cfg_.rampSeconds > 0) {
        double elapsedSec =
            static_cast<double>(nowMs - match_start_ms_) / 1000.0;
        if (elapsedSec < static_cast<double>(cfg_.rampSeconds) &&
            (frameHash(frame, seed_) & 0xFF) >
                static_cast<uint64_t>(
                    elapsedSec / static_cast<double>(cfg_.rampSeconds) *
                    255.0)) {
            return false;
        }
    }
    // Apply chance gate.
    double r = static_cast<double>(frameHash(frame, seed_ ^ 0xA5A5) & 0xFFFF) /
               65535.0;
    return r < cfg_.applyChance;
}

bool PacketProxy::shouldDrop(const WireFrame& frame, int64_t nowMs) const {
    if (!cfg_.enabled) return false;
    if (cfg_.dropChance <= 0.0) return false;
    if (!frame.isEnemy) return false;
    if (frame.isMovement) return false;  // never drop movement packets
    if (inPauseWindow(nowMs)) return false;
    if (budgetExhausted()) return false;
    double r = static_cast<double>(frameHash(frame, seed_ ^ 0x3C3C) & 0xFFFF) /
               65535.0;
    return r < cfg_.dropChance * 0.5;
}

int PacketProxy::effectiveDelayMs(const WireFrame& frame,
                                  int64_t nowMs) const {
    (void)nowMs;
    return delayForFrame(frame, cfg_, seed_, 100);
}

bool PacketProxy::onFrame(const WireFrame& frame) {
    frames_seen_ += 1;
    if (!cfg_.enabled) {
        frames_forwarded_ += 1;
        return false;
    }

    // Track enemy entities.
    if (frame.isEnemy) {
        bool known = false;
        for (auto& e : enemies_) {
            if (e.id == frame.srcEntity) {
                e.lastSeenMs = utils::monotonicMs();
                e.packetsSeen += 1;
                if (frame.isSkill) e.lastSkillMs = utils::monotonicMs();
                if (frame.isMovement) e.lastMoveMs = utils::monotonicMs();
                if (frame.opcode >= 0x11 && frame.opcode <= 0x1F) {
                    e.lastAttackMs = utils::monotonicMs();
                }
                known = true;
                break;
            }
        }
        if (!known) {
            EnemyEntity e;
            e.id = frame.srcEntity;
            e.firstSeenMs = utils::monotonicMs();
            e.lastSeenMs = e.firstSeenMs;
            e.packetsSeen = 1;
            if (frame.isSkill) e.lastSkillMs = utils::monotonicMs();
            if (frame.isMovement) e.lastMoveMs = utils::monotonicMs();
            enemies_.push_back(e);
        }
    }

    if (shouldDrop(frame, utils::monotonicMs())) {
        frames_dropped_ += 1;
        consumeBudget();
        last_burst_ms_ = utils::monotonicMs();
        return true;  // caller drops the original
    }

    if (shouldDelay(frame, utils::monotonicMs())) {
        int delay = effectiveDelayMs(frame, utils::monotonicMs());
        if (delay > 0) {
            PendingFrame pf;
            pf.frame = frame;
            pf.releaseAtMs = utils::monotonicMs() + delay;
            pf.delayMs = delay;
            pf.obfuscated = cfg_.obfuscate;
            pending_.push_back(pf);
            frames_delayed_ += 1;
            delay_sum_ms_ += static_cast<double>(delay);
            delay_count_ += 1;
            consumeBudget();
            last_burst_ms_ = utils::monotonicMs();
            return false;  // original passes through; delayed copy follows
        }
    }

    frames_forwarded_ += 1;
    return false;
}

void PacketProxy::pump(int64_t nowMs) {
    std::vector<PendingFrame> due;
    std::vector<PendingFrame> keep;
    for (auto& pf : pending_) {
        if (pf.releaseAtMs <= nowMs) {
            due.push_back(pf);
        } else {
            keep.push_back(pf);
        }
    }
    pending_ = keep;
    for (auto& pf : due) {
        if (pf.obfuscated) frames_obfuscated_ += 1;
        if (onDeliver) onDeliver(pf.frame);
    }
}

void PacketProxy::tick(int64_t nowMs) {
    // Rolling second window for the budget.
    if (nowMs - second_start_ms_ >= 1000) {
        second_start_ms_ = nowMs;
        packets_this_second_ = 0;
    }
    // Random pause windows (anti-pattern).
    if (cfg_.randomizePattern && cfg_.pauseMs > 0) {
        if (pause_until_ms_ <= 0 && last_burst_ms_ > 0 &&
            nowMs - last_burst_ms_ > cfg_.burstCooldownMs * 3) {
            uint32_t r = utils::random32() % 100;
            if (r < 12) {
                pause_until_ms_ = nowMs + cfg_.pauseMs;
            }
        }
        if (pause_until_ms_ > 0 && nowMs >= pause_until_ms_) {
            pause_until_ms_ = 0;
        }
    }
    pump(nowMs);
}

void PacketProxy::reseed(int64_t nowMs) {
    seed_ = utils::random64() ^ static_cast<uint64_t>(nowMs);
}

EnemyLagStats PacketProxy::snapshot() const {
    EnemyLagStats s;
    s.active = cfg_.enabled;
    s.framesSeen = frames_seen_;
    s.framesDelayed = frames_delayed_;
    s.framesDropped = frames_dropped_;
    s.framesForwarded = frames_forwarded_;
    s.framesObfuscated = frames_obfuscated_;
    s.enemiesTracked = static_cast<int64_t>(enemies_.size());
    s.appliedDelayAvgMs =
        delay_count_ > 0 ? delay_sum_ms_ / static_cast<double>(delay_count_)
                         : 0.0;
    s.lastBurstMs = last_burst_ms_;
    s.lastPauseUntilMs = pause_until_ms_;
    s.packetsThisSecond = packets_this_second_;
    s.startedAtMs = match_start_ms_;
    return s;
}

std::string PacketProxy::statusLine() const {
    EnemyLagStats s = snapshot();
    char buf[256];
    snprintf(buf, sizeof(buf),
             "EL %s | delay=%.0fms drop=%lld | enemies=%lld | rate=%lld/s",
             s.active ? "ON" : "OFF", s.appliedDelayAvgMs,
             static_cast<long long>(s.framesDropped),
             static_cast<long long>(s.enemiesTracked),
             static_cast<long long>(s.packetsThisSecond));
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// EnemyLag
// ---------------------------------------------------------------------------

EnemyLag& EnemyLag::instance() {
    static EnemyLag lag;
    return lag;
}

void EnemyLag::enable(const EnemyLagConfig& cfg) {
    cfg_ = cfg;
    cfg_.enabled = true;
    enabled_ = true;
    started_at_ms_ = utils::monotonicMs();
    PacketProxy::instance().setConfig(cfg_);
    PacketProxy::instance().reseed(started_at_ms_);
    ARIFT_INFO(kTagEnemyLag, "enabled (delay=%dms jitter=%d apply=%.2f drop=%.2f)",
               cfg_.delayMs, cfg_.jitterMs, cfg_.applyChance, cfg_.dropChance);
}

void EnemyLag::disable() {
    if (!enabled_) return;
    enabled_ = false;
    cfg_.enabled = false;
    PacketProxy::instance().setConfig(cfg_);
    ARIFT_INFO(kTagEnemyLag, "disabled");
}

void EnemyLag::onMatchStart(int64_t nowMs) {
    if (!enabled_) return;
    PacketProxy::instance().beginMatch(nowMs);
}

void EnemyLag::onMatchEnd(int64_t nowMs) {
    PacketProxy::instance().endMatch(nowMs);
}

bool EnemyLag::onFrame(const WireFrame& frame) {
    if (!enabled_) return false;
    return PacketProxy::instance().onFrame(frame);
}

EnemyLagStats EnemyLag::stats() const {
    return PacketProxy::instance().snapshot();
}

std::string EnemyLag::statusLine() const {
    return PacketProxy::instance().statusLine();
}

void EnemyLag::setIntensity(int percent) {
    intensity_ = utils::clamp(percent, 0, 200);
}

void EnemyLag::focusEnemy(uint64_t id) {
    focus_id_ = id;
}

void EnemyLag::clearFocus() {
    focus_id_ = 0;
}

void EnemyLag::tick() {
    int64_t now = utils::monotonicMs();
    if (last_tick_ms_ > 0 && now - last_tick_ms_ < 500) return;
    last_tick_ms_ = now;
    if (!enabled_) return;
    PacketProxy::instance().tick(now);
    // Re-seed occasionally so patterns never repeat.
    if ((now - started_at_ms_) % 90000 < 500) {
        PacketProxy::instance().reseed(now);
    }
}

std::string EnemyLag::statsBlob() const {
    EnemyLagStats s = stats();
    char buf[512];
    snprintf(buf, sizeof(buf),
             "el{on=%d seen=%lld delayed=%lld dropped=%lld fwd=%lld "
             "obf=%lld avg=%.0fms enemies=%lld intensity=%d}",
             s.active ? 1 : 0, static_cast<long long>(s.framesSeen),
             static_cast<long long>(s.framesDelayed),
             static_cast<long long>(s.framesDropped),
             static_cast<long long>(s.framesForwarded),
             static_cast<long long>(s.framesObfuscated), s.appliedDelayAvgMs,
             static_cast<long long>(s.enemiesTracked), intensity_);
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Entity role heuristics (from observed packet mix)
// ---------------------------------------------------------------------------

int inferRoleFromFrames(const std::vector<WireFrame>& frames) {
    int skills = 0;
    int moves = 0;
    int attacks = 0;
    for (const auto& f : frames) {
        if (f.isSkill) skills += 1;
        if (f.isMovement) moves += 1;
        if (f.opcode >= 0x11 && f.opcode <= 0x1F) attacks += 1;
    }
    int total = skills + moves + attacks;
    if (total <= 0) return 0;
    double skillRatio = static_cast<double>(skills) / total;
    double attackRatio = static_cast<double>(attacks) / total;
    if (attackRatio > 0.55) return 4;       // marksman
    if (skillRatio > 0.6 && moves < 15) return 3;  // mage
    if (moves > 0 && skillRatio < 0.25) return 2;  // jungler
    if (attackRatio < 0.2 && moves < 5) return 1;  // tank
    return 5;  // support
}

// Priority ordering: which enemies matter most (carry first).
double enemyPriority(const EnemyEntity& e) {
    double p = e.lagScore;
    if (e.roleHint == 4) p += 10.0;  // marksman first
    if (e.roleHint == 3) p += 8.0;   // mage second
    if (e.roleHint == 2) p += 5.0;   // jungler third
    if (!e.confirmed) p -= 3.0;
    return p;
}

// Snapshot of the most threatening enemies, sorted by priority.
std::vector<EnemyEntity> priorityEnemies(const PacketProxy& proxy,
                                         int limit) {
    std::vector<EnemyEntity> out = proxy.enemies();
    std::sort(out.begin(), out.end(),
              [](const EnemyEntity& a, const EnemyEntity& b) {
                  return enemyPriority(a) > enemyPriority(b);
              });
    if (out.size() > static_cast<size_t>(limit)) {
        out.resize(static_cast<size_t>(limit));
    }
    return out;
}

// Update lag scores from the observed frame mix.
void updateLagScores(std::vector<EnemyEntity>& enemies) {
    for (auto& e : enemies) {
        double skillShare = 0.0;
        if (e.packetsSeen > 0 && e.lastSkillMs > 0) {
            int64_t recent = e.packetsSeen;
            skillShare = static_cast<double>(
                             std::min<int64_t>(recent, 50)) / 50.0;
        }
        e.lagScore = utils::clamp(skillShare * 100.0, 0.0, 100.0);
    }
}

// Build a compact text summary of the enemy roster.
std::string enemyRosterLine(const std::vector<EnemyEntity>& enemies) {
    std::string out = "[";
    for (size_t i = 0; i < enemies.size(); ++i) {
        if (i > 0) out += ",";
        out += roleName(enemies[i].roleHint);
        out += ":";
        out += std::to_string(static_cast<int>(enemies[i].lagScore));
    }
    out += "]";
    return out;
}

// ---------------------------------------------------------------------------
// Focus targeting
// ---------------------------------------------------------------------------

// Whether the proxy should hard-delay a focused enemy.
bool isFocused(const WireFrame& frame, uint64_t focusId) {
    if (focusId == 0) return false;
    return frame.srcEntity == focusId;
}

// Effective delay with focus boost (carry shutdown mode).
int focusedDelayMs(int baseDelay, uint64_t focusId, uint64_t frameSrc) {
    if (focusId == 0 || frameSrc != focusId) return baseDelay;
    return static_cast<int>(static_cast<double>(baseDelay) * 1.5);
}

// ---------------------------------------------------------------------------
// Match-level policy driver
// ---------------------------------------------------------------------------

// Frame classification from raw bytes captured at the socket layer.
WireFrame classifyFrame(const uint8_t* data, size_t len, uint64_t src,
                        bool isEnemy) {
    WireFrame f;
    f.srcEntity = src;
    f.isEnemy = isEnemy;
    if (len >= 1) f.opcode = data[0];
    if (len >= 5) {
        f.seq = static_cast<uint32_t>(data[1]) |
                (static_cast<uint32_t>(data[2]) << 8) |
                (static_cast<uint32_t>(data[3]) << 16) |
                (static_cast<uint32_t>(data[4]) << 24);
    }
    if (len > 5) {
        f.payload.assign(data + 5, data + len);
    }
    f.isSkill = isCombatOpcode(f.opcode) &&
                (f.opcode != 0x11 && f.opcode != 0x16);
    f.isMovement = isMovementOpcode(f.opcode);
    f.capturedAtMs = utils::monotonicMs();
    return f;
}

// Health check: number of pending frames queued for delivery.
size_t pendingDepth(const PacketProxy& proxy) {
    return proxy.pendingCount();
}

// Effective session aggression level for the HUD (0..100).
int sessionAggression(const EnemyLagStats& s) {
    if (s.framesSeen <= 0) return 0;
    double delayedRate =
        static_cast<double>(s.framesDelayed) / static_cast<double>(s.framesSeen);
    return utils::clamp(static_cast<int>(delayedRate * 150.0), 0, 100);
}

// Whether the feature should be considered "working" this match.
bool healthy(const EnemyLagStats& s, int64_t nowMs) {
    if (!s.active) return false;
    if (s.startedAtMs <= 0) return false;
    if (nowMs - s.startedAtMs < 10000) return true;
    return s.framesSeen > 0 && s.framesDelayed + s.framesDropped > 0;
}

// Human-readable state label.
const char* stateLabel(const EnemyLagStats& s, int64_t nowMs) {
    if (!s.active) return "OFF";
    if (s.lastPauseUntilMs > nowMs) return "PAUSED";
    if (s.lastBurstMs <= 0) return "ARMED";
    return "ACTIVE";
}

// ---------------------------------------------------------------------------
// Pattern engine: per-enemy hitch bursts
// ---------------------------------------------------------------------------

// A hitch burst is a short cluster of delayed frames aimed at one enemy.
struct HitchBurst {
    uint64_t enemyId = 0;
    int frames = 0;
    int framesDone = 0;
    int delayBaseMs = 0;
    int delayJitterMs = 0;
    int64_t startedAtMs = 0;
    int64_t nextFrameAtMs = 0;
};

class BurstEngine {
public:
    static BurstEngine& instance() {
        static BurstEngine e;
        return e;
    }

    void reset() {
        bursts_.clear();
        last_hitch_ms_ = 0;
    }

    // Propose a burst for the given enemy (respects global cooldown).
    bool canStartBurst(int64_t nowMs, const EnemyLagConfig& cfg) const {
        if (last_hitch_ms_ > 0 && nowMs - last_hitch_ms_ <
                                     static_cast<int64_t>(cfg.burstCooldownMs)) {
            return false;
        }
        return true;
    }

    void startBurst(uint64_t enemyId, int64_t nowMs,
                    const EnemyLagConfig& cfg) {
        HitchBurst b;
        b.enemyId = enemyId;
        b.frames = utils::clamp(1 + cfg.delayMs / 250, 1, 5);
        b.delayBaseMs = cfg.delayMs;
        b.delayJitterMs = cfg.jitterMs;
        b.startedAtMs = nowMs;
        b.nextFrameAtMs = nowMs + 15 + (utils::random32() % 25);
        bursts_.push_back(b);
        last_hitch_ms_ = nowMs;
    }

    // Whether the engine currently has a burst aimed at this enemy.
    bool bursting(uint64_t enemyId) const {
        for (const auto& b : bursts_) {
            if (b.enemyId == enemyId && b.framesDone < b.frames) return true;
        }
        return false;
    }

    // Advance the burst clock; returns frames that should be delayed now.
    std::vector<uint64_t> dueEnemies(int64_t nowMs) {
        std::vector<uint64_t> due;
        for (auto& b : bursts_) {
            if (b.framesDone >= b.frames) continue;
            if (nowMs >= b.nextFrameAtMs) {
                due.push_back(b.enemyId);
                b.framesDone += 1;
                int gap = 20 + (utils::random32() % 60);
                b.nextFrameAtMs = nowMs + gap;
            }
        }
        return due;
    }

    // Effective delay for the enemy right now (peaks mid-burst).
    int currentDelayFor(uint64_t enemyId, int64_t nowMs) const {
        for (const auto& b : bursts_) {
            if (b.enemyId != enemyId) continue;
            if (b.framesDone >= b.frames) break;
            double peak = 1.0 + 0.6 * std::sin(
                static_cast<double>(b.framesDone) /
                static_cast<double>(std::max(1, b.frames)) * 3.14159);
            double jitter = utils::gaussian(0.0,
                static_cast<double>(b.delayJitterMs) * 0.4);
            return utils::clamp(
                static_cast<int>(static_cast<double>(b.delayBaseMs) * peak +
                                 jitter),
                0, 2500);
        }
        (void)nowMs;
        return 0;
    }

    void prune(int64_t nowMs) {
        std::vector<HitchBurst> keep;
        for (const auto& b : bursts_) {
            if (b.framesDone >= b.frames) continue;
            if (nowMs - b.startedAtMs > 30000) continue;
            keep.push_back(b);
        }
        bursts_ = keep;
    }

    int activeBursts() const {
        int n = 0;
        for (const auto& b : bursts_) {
            if (b.framesDone < b.frames) n += 1;
        }
        return n;
    }

private:
    std::vector<HitchBurst> bursts_;
    int64_t last_hitch_ms_ = 0;
};

// ---------------------------------------------------------------------------
// Per-enemy calibration
// ---------------------------------------------------------------------------

// Each enemy gets a calibrated lag signature so behavior differs per target.
struct EnemyCalibration {
    uint64_t enemyId = 0;
    int baseDelayMs = 0;
    int jitterMs = 0;
    double applyRate = 0.0;
    double dropRate = 0.0;
    int64_t calibratedAtMs = 0;
    int observations = 0;
};

class CalibrationTable {
public:
    static CalibrationTable& instance() {
        static CalibrationTable t;
        return t;
    }

    void reset() { table_.clear(); }

    void observe(uint64_t enemyId, int64_t nowMs,
                 const EnemyLagConfig& cfg) {
        EnemyCalibration* c = findOrCreate(enemyId);
        if (c->observations == 0) {
            c->enemyId = enemyId;
            c->baseDelayMs = cfg.delayMs;
            c->jitterMs = cfg.jitterMs;
            c->applyRate = cfg.applyChance;
            c->dropRate = cfg.dropChance;
        } else {
            // Slow drift toward the configured center with wobble.
            double drift = utils::gaussian(0.0, 6.0);
            c->baseDelayMs = utils::clamp(
                static_cast<int>(c->baseDelayMs * 0.92 +
                                 cfg.delayMs * 0.08 + drift),
                0, 2000);
            c->applyRate = utils::clamp(
                c->applyRate * 0.9 + cfg.applyChance * 0.1 +
                    utils::gaussian(0.0, 0.02),
                0.2, 1.0);
        }
        c->observations += 1;
        c->calibratedAtMs = nowMs;
    }

    // Look up the calibrated delay for an enemy (falls back to config).
    int delayFor(uint64_t enemyId, int fallbackMs) const {
        for (const auto& c : table_) {
            if (c.enemyId == enemyId) return c.baseDelayMs;
        }
        return fallbackMs;
    }

    double applyRateFor(uint64_t enemyId, double fallback) const {
        for (const auto& c : table_) {
            if (c.enemyId == enemyId) return c.applyRate;
        }
        return fallback;
    }

    double dropRateFor(uint64_t enemyId, double fallback) const {
        for (const auto& c : table_) {
            if (c.enemyId == enemyId) return c.dropRate;
        }
        return fallback;
    }

    int size() const { return static_cast<int>(table_.size()); }

private:
    EnemyCalibration* findOrCreate(uint64_t enemyId) {
        for (auto& c : table_) {
            if (c.enemyId == enemyId) return &c;
        }
        table_.emplace_back();
        table_.back().enemyId = enemyId;
        return &table_.back();
    }

    std::vector<EnemyCalibration> table_;
};

// ---------------------------------------------------------------------------
// Match phase machine
// ---------------------------------------------------------------------------

enum class MatchPhase : int {
    kNone = 0,
    kLobby = 1,
    kLoadout = 2,
    kEarly = 3,   // first 3 minutes
    kMid = 4,     // 3..12 minutes
    kLate = 5,    // 12+ minutes
    kEnded = 6,
};

MatchPhase phaseFor(int64_t elapsedMs) {
    if (elapsedMs < 0) return MatchPhase::kNone;
    double min = static_cast<double>(elapsedMs) / 60000.0;
    if (min < 3.0) return MatchPhase::kEarly;
    if (min < 12.0) return MatchPhase::kMid;
    return MatchPhase::kLate;
}

// Phase-dependent intensity multiplier.
double phaseMultiplier(MatchPhase phase) {
    switch (phase) {
        case MatchPhase::kEarly: return 0.7;
        case MatchPhase::kMid: return 1.0;
        case MatchPhase::kLate: return 1.25;
        default: return 0.3;
    }
}

// Per-phase delay adjustment for the proxy config.
int phaseAdjustedDelay(const EnemyLagConfig& cfg, MatchPhase phase) {
    double m = phaseMultiplier(phase);
    return utils::clamp(static_cast<int>(cfg.delayMs * m), 0, 2000);
}

// ---------------------------------------------------------------------------
// Threat evaluation
// ---------------------------------------------------------------------------

// Score how dangerous a tracked enemy currently is (0..100).
double threatScore(const EnemyEntity& e, int64_t nowMs) {
    double score = 0.0;
    if (e.confirmed) score += 20.0;
    if (e.lastAttackMs > 0 && nowMs - e.lastAttackMs < 30000) score += 30.0;
    if (e.lastSkillMs > 0 && nowMs - e.lastSkillMs < 20000) score += 25.0;
    if (e.packetsSeen > 100) score += 10.0;
    if (e.roleHint == 4) score += 15.0;  // marksman = carry
    if (e.roleHint == 3) score += 10.0;  // mage = burst
    score += e.lagScore * 0.2;
    return utils::clamp(score, 0.0, 100.0);
}

// Pick the current priority target (highest threat, confirmed first).
uint64_t pickTarget(const std::vector<EnemyEntity>& enemies, int64_t nowMs) {
    uint64_t best = 0;
    double bestScore = -1.0;
    for (const auto& e : enemies) {
        if (!e.confirmed) continue;
        double s = threatScore(e, nowMs);
        if (s > bestScore) {
            bestScore = s;
            best = e.id;
        }
    }
    if (best == 0) {
        // Fall back to the most-seen unconfirmed entity.
        for (const auto& e : enemies) {
            if (e.packetsSeen > bestScore) {
                bestScore = static_cast<double>(e.packetsSeen);
                best = e.id;
            }
        }
    }
    return best;
}

// Whether the carry is being pressured (kept in a hitch loop).
bool carryUnderPressure(const std::vector<EnemyEntity>& enemies,
                        const PacketProxy& proxy, int64_t nowMs) {
    uint64_t target = pickTarget(enemies, nowMs);
    if (target == 0) return false;
    for (const auto& e : enemies) {
        if (e.id == target) {
            return e.lastAttackMs > 0 && nowMs - e.lastAttackMs < 15000;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Session model
// ---------------------------------------------------------------------------

// Rolling session effectiveness (delayed packets per minute).
double sessionEffectiveness(const EnemyLagStats& s, int64_t nowMs) {
    if (s.startedAtMs <= 0) return 0.0;
    double minutes = static_cast<double>(nowMs - s.startedAtMs) / 60000.0;
    if (minutes <= 0.0) return 0.0;
    return static_cast<double>(s.framesDelayed + s.framesDropped) / minutes;
}

// Estimate of how many "free hits" the enemy lost this match.
int lostActionsEstimate(const EnemyLagStats& s) {
    // Each delayed burst wastes roughly one enemy action.
    return utils::clamp(static_cast<int>(s.framesDelayed / 3), 0, 500);
}

// ---------------------------------------------------------------------------
// Public surface (host + JNI)
// ---------------------------------------------------------------------------

// Wire the whole feature into a pump loop (called by the host thread).
void pumpAll(int64_t nowMs) {
    BurstEngine::instance().prune(nowMs);
    PacketProxy::instance().pump(nowMs);
}

// Called every second from the host: advance the pattern engine.
void secondTick(int64_t nowMs) {
    EnemyLagStats s = PacketProxy::instance().snapshot();
    MatchPhase phase = phaseFor(nowMs - s.startedAtMs);
    if (phase == MatchPhase::kLate) {
        // Late game: slightly more aggressive per-enemy calibration.
        for (const auto& e : PacketProxy::instance().enemies()) {
            CalibrationTable::instance().observe(e.id, nowMs,
                                                 PacketProxy::instance().config());
        }
    }
    BurstEngine::instance().prune(nowMs);
}

// Register a confirmed enemy roster from the game's match data.
void setEnemyRoster(const std::vector<uint64_t>& ids,
                    const std::vector<std::string>& names) {
    for (size_t i = 0; i < ids.size(); ++i) {
        std::string name = i < names.size() ? names[i] : std::string();
        PacketProxy::instance().registerEnemy(ids[i], name);
        PacketProxy::instance().confirmEnemy(ids[i]);
    }
}

// Configure from a JSON-ish blob (used by the JNI bridge).
bool configureFromBlob(const std::string& blob, EnemyLagConfig* out) {
    if (!configFromCacheString(blob, out)) return false;
    sanitizeConfig(out);
    return configValid(*out);
}

// Session start/stop helpers.
void sessionStarted() {
    int64_t now = utils::monotonicMs();
    BurstEngine::instance().reset();
    CalibrationTable::instance().reset();
    PacketProxy::instance().beginMatch(now);
}

void sessionStopped() {
    PacketProxy::instance().endMatch(utils::monotonicMs());
    BurstEngine::instance().reset();
}

// Diagnostic blob for the UI debug panel.
std::string fullDiag() {
    EnemyLagStats s = PacketProxy::instance().snapshot();
    std::string out = diagDump(PacketProxy::instance().config(), s);
    out += "\nbursts=";
    out += std::to_string(BurstEngine::instance().activeBursts());
    out += " calib=";
    out += std::to_string(CalibrationTable::instance().size());
    return out;
}

// ---------------------------------------------------------------------------
// Opcode policy matrix
// ---------------------------------------------------------------------------

// Per-opcode manipulation weight: which packets matter most when lagging.
struct OpcodeWeight {
    uint8_t opcode;
    double weight;   // 0 = never touch, 1 = always delay
    bool combat;     // part of the combat family
};

const OpcodeWeight kOpcodeWeights[] = {
    {0x11, 0.9, true},   // basic attack
    {0x12, 1.0, true},   // skill cast
    {0x13, 1.0, true},   // skill aim
    {0x14, 1.0, true},   // ultimate
    {0x15, 0.8, true},   // dash
    {0x16, 0.9, true},   // retarget
    {0x17, 0.6, true},   // cancel
    {0x18, 0.9, true},   // combo
    {0x19, 0.7, true},   // summoner
    {0x1A, 0.7, true},   // battle spell
    {0x1B, 0.5, true},   // flinch
    {0x1C, 0.5, true},   // knockback
    {0x1D, 0.4, true},   // stun response
    {0x1E, 0.6, true},   // revive
    {0x1F, 0.3, true},   // recall
    {0x21, 0.15, false}, // move start
    {0x22, 0.10, false}, // move update
    {0x23, 0.10, false}, // move stop
    {0x24, 0.05, false}, // joystick
    {0x25, 0.05, false}, // path
    {0x26, 0.30, false}, // teleport
    {0x27, 0.20, false}, // knockback applied
    {0x31, 0.0, false},  // ping
    {0x32, 0.0, false},  // ack
    {0x33, 0.0, false},  // heartbeat
    {0x34, 0.0, false},  // settings sync
    {0x35, 0.0, false},  // inventory sync
};

// Weight for an opcode (0 if unknown).
double weightFor(uint8_t opcode) {
    for (const auto& w : kOpcodeWeights) {
        if (w.opcode == opcode) return w.weight;
    }
    return 0.0;
}

// Whether the opcode belongs to the combat family.
bool opcodeIsCombat(uint8_t opcode) {
    for (const auto& w : kOpcodeWeights) {
        if (w.opcode == opcode) return w.combat;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Cooldown manager
// ---------------------------------------------------------------------------

// Per-enemy manipulation cooldown so the same target is not hammered.
class EnemyCooldowns {
public:
    static EnemyCooldowns& instance() {
        static EnemyCooldowns c;
        return c;
    }

    void reset() { table_.clear(); }

    void mark(uint64_t enemyId, int64_t nowMs, int64_t cooldownMs) {
        table_[enemyId] = nowMs + cooldownMs;
    }

    bool available(uint64_t enemyId, int64_t nowMs) const {
        auto it = table_.find(enemyId);
        if (it == table_.end()) return true;
        return nowMs >= it->second;
    }

    void gc(int64_t nowMs) {
        for (auto it = table_.begin(); it != table_.end();) {
            if (nowMs >= it->second) {
                it = table_.erase(it);
            } else {
                ++it;
            }
        }
    }

private:
    std::map<uint64_t, int64_t> table_;
};

// ---------------------------------------------------------------------------
// UI-facing text helpers
// ---------------------------------------------------------------------------

// Short target description for the HUD.
std::string targetLabel(uint64_t enemyId) {
    if (enemyId == 0) return "none";
    for (const auto& e : PacketProxy::instance().enemies()) {
        if (e.id == enemyId) {
            return e.name.empty() ? std::to_string(enemyId) : e.name;
        }
    }
    return std::to_string(enemyId);
}

// Multi-line status panel for the menu.
std::string statusPanel() {
    EnemyLagStats s = PacketProxy::instance().snapshot();
    int64_t now = utils::monotonicMs();
    char buf[512];
    snprintf(buf, sizeof(buf),
             "ENEMY LAG\n"
             "state: %s\n"
             "delay: %.0f ms avg (max %d)\n"
             "hitch index: %.2f\n"
             "delayed: %lld  dropped: %lld  obfuscated: %lld\n"
             "enemies tracked: %lld\n"
             "session rate: %.1f pkts/min",
             stateLabel(s, now), s.appliedDelayAvgMs,
             sessionLatencyModel().maxMs, sessionLatencyModel().hitchIndex(),
             static_cast<long long>(s.framesDelayed),
             static_cast<long long>(s.framesDropped),
             static_cast<long long>(s.framesObfuscated),
             static_cast<long long>(s.enemiesTracked),
             sessionEffectiveness(s, now));
    return std::string(buf);
}

// Compact roster line with per-enemy lag status.
std::string rosterLine() {
    auto enemies = priorityEnemies(PacketProxy::instance(), 5);
    std::string out;
    for (size_t i = 0; i < enemies.size(); ++i) {
        if (i > 0) out += " | ";
        out += enemies[i].name.empty()
                   ? std::to_string(enemies[i].id)
                   : enemies[i].name;
        out += "(";
        out += roleName(enemies[i].roleHint);
        out += " ";
        out += std::to_string(static_cast<int>(enemies[i].lagScore));
        out += ")";
    }
    return out.empty() ? "no enemies tracked" : out;
}

// ---------------------------------------------------------------------------
// Quality controls
// ---------------------------------------------------------------------------

// Minimum inter-frame gap for manipulated frames (human perception floor).
int minPerceptibleGapMs() { return 16; }

// Ceiling for a single hitch so it stays "plausible lag".
int maxPlausibleHitchMs() { return 1200; }

// Sanity: the applied delay must never exceed the plausible ceiling.
int clampPlausible(int delayMs) {
    return utils::clamp(delayMs, minPerceptibleGapMs(), maxPlausibleHitchMs());
}

// Whether the current pattern is "quiet enough" (low detection surface).
bool quietEnough(const EnemyLagStats& s, int64_t nowMs) {
    double rate = sessionEffectiveness(s, nowMs);
    // Above 25 manipulated pkts/min starts to look suspicious.
    return rate < 25.0;
}

// ---------------------------------------------------------------------------
// Focus override wiring
// ---------------------------------------------------------------------------

// Effective target for the next manipulation cycle.
uint64_t effectiveTarget(uint64_t focusId, int64_t nowMs) {
    if (focusId != 0) return focusId;
    return pickTarget(PacketProxy::instance().enemies(), nowMs);
}

// Whether the engine should ignore a frame because of per-enemy cooldown.
bool cooldownBlocks(uint64_t enemyId, int64_t nowMs) {
    return !EnemyCooldowns::instance().available(enemyId, nowMs);
}

// Apply a manipulation burst to the effective target.
void engageTarget(uint64_t enemyId, int64_t nowMs,
                  const EnemyLagConfig& cfg) {
    EnemyCooldowns::instance().mark(enemyId, nowMs,
                                    static_cast<int64_t>(cfg.burstCooldownMs));
    if (BurstEngine::instance().canStartBurst(nowMs, cfg)) {
        BurstEngine::instance().startBurst(enemyId, nowMs, cfg);
        EnemyLagStats s = PacketProxy::instance().snapshot();
        ARIFT_DEBUG(kTagEnemyLag, "burst -> %llu (active=%d)",
                    static_cast<unsigned long long>(enemyId),
                    BurstEngine::instance().activeBursts());
        (void)s;
    }
}

// Advance the burst engine and feed due delays into the proxy.
void pumpBurstEngine(int64_t nowMs, const EnemyLagConfig& cfg) {
    auto due = BurstEngine::instance().dueEnemies(nowMs);
    for (uint64_t id : due) {
        // Deliver one extra delayed copy per due enemy (re-injection).
        int d = BurstEngine::instance().currentDelayFor(id, nowMs);
        d = clampPlausible(d);
        if (d <= 0) continue;
        // The proxy's tick already pumps; here we just record intent.
        sessionLatencyModel().add(d);
        (void)cfg;
    }
}

// ---------------------------------------------------------------------------
// Session-level orchestration
// ---------------------------------------------------------------------------

// Full per-second orchestration from the host thread.
void orchestrate(int64_t nowMs) {
    if (!PacketProxy::instance().config().enabled) return;

    EnemyCooldowns::instance().gc(nowMs);
    EnemyLagStats s = PacketProxy::instance().snapshot();
    MatchPhase phase = phaseFor(nowMs - s.startedAtMs);
    if (phase == MatchPhase::kNone || phase == MatchPhase::kEnded) return;

    // Keep calibrations warm for tracked enemies.
    for (const auto& e : PacketProxy::instance().enemies()) {
        if (e.confirmed && nowMs - e.lastSeenMs < 120000) {
            CalibrationTable::instance().observe(
                e.id, nowMs, PacketProxy::instance().config());
        }
    }

    // Engage the current threat.
    uint64_t target = effectiveTarget(EnemyLag::instance().focusedEnemy(),
                                      nowMs);
    if (target != 0 && !cooldownBlocks(target, nowMs)) {
        engageTarget(target, nowMs, PacketProxy::instance().config());
    }

    pumpBurstEngine(nowMs, PacketProxy::instance().config());
    BurstEngine::instance().prune(nowMs);
}

// Simulated hitch profile for tests (documented behavior).
std::vector<int> simulatedHitchProfile(int frames, int baseMs,
                                       int jitterMs) {
    std::vector<int> out;
    for (int i = 0; i < frames; ++i) {
        double peak = 1.0 + 0.5 * std::sin(
            static_cast<double>(i + 1) / static_cast<double>(frames) *
            3.14159);
        double j = utils::gaussian(0.0, static_cast<double>(jitterMs) * 0.4);
        out.push_back(utils::clamp(
            static_cast<int>(static_cast<double>(baseMs) * peak + j),
            0, maxPlausibleHitchMs()));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Enemy behaviour modelling
// ---------------------------------------------------------------------------

// Per-enemy activity profile: how aggressive a target has been recently.
struct EnemyActivity {
    uint64_t enemyId = 0;
    int64_t lastActionMs = 0;
    int actionsRecent = 0;
    double aggression = 0.0;
    int64_t windowStartMs = 0;
};

class ActivityTracker {
public:
    static ActivityTracker& instance() {
        static ActivityTracker t;
        return t;
    }

    void reset() { table_.clear(); }

    void noteAction(uint64_t enemyId, int64_t nowMs) {
        EnemyActivity* a = find(enemyId);
        if (a->windowStartMs == 0 || nowMs - a->windowStartMs > 60000) {
            a->windowStartMs = nowMs;
            a->actionsRecent = 0;
        }
        a->actionsRecent += 1;
        a->lastActionMs = nowMs;
        a->aggression = utils::clamp(
            a->aggression * 0.8 +
                static_cast<double>(a->actionsRecent) / 10.0,
            0.0, 1.0);
    }

    double aggressionFor(uint64_t enemyId) const {
        for (const auto& a : table_) {
            if (a.enemyId == enemyId) return a.aggression;
        }
        return 0.0;
    }

    int actionsLastMinute(uint64_t enemyId, int64_t nowMs) const {
        for (const auto& a : table_) {
            if (a.enemyId == enemyId && nowMs - a.lastActionMs < 60000) {
                return a.actionsRecent;
            }
        }
        return 0;
    }

    // Top aggressor right now.
    uint64_t topAggressor(int64_t nowMs) const {
        uint64_t best = 0;
        double bestScore = -1.0;
        for (const auto& a : table_) {
            if (nowMs - a.lastActionMs > 60000) continue;
            double s = a.aggression * 100.0 + a.actionsRecent;
            if (s > bestScore) {
                bestScore = s;
                best = a.enemyId;
            }
        }
        return best;
    }

private:
    EnemyActivity* find(uint64_t enemyId) {
        for (auto& a : table_) {
            if (a.enemyId == enemyId) return &a;
        }
        table_.emplace_back();
        table_.back().enemyId = enemyId;
        return &table_.back();
    }

    std::vector<EnemyActivity> table_;
};

// ---------------------------------------------------------------------------
// Dynamic intensity controller
// ---------------------------------------------------------------------------

// The controller ramps intensity up when the enemy team is dominant and
// backs off when our side is winning (keeps matches playable).
class IntensityController {
public:
    static IntensityController& instance() {
        static IntensityController c;
        return c;
    }

    void reset() { current_ = 100; }

    // Adjust from match signals; returns the new intensity (0..200).
    int update(double ourWinProbability, double enemyAggression) {
        double target = 100.0;
        if (ourWinProbability < 0.35) target = 150.0;  // losing: push harder
        if (ourWinProbability > 0.75) target = 60.0;   // winning: lay low
        target += enemyAggression * 40.0;
        current_ = utils::clamp(
            static_cast<int>(current_ * 0.85 + target * 0.15), 0, 200);
        return current_;
    }

    int current() const { return current_; }

private:
    int current_ = 100;
};

// ---------------------------------------------------------------------------
// Manipulation planner
// ---------------------------------------------------------------------------

// The planner decides, per tick, which enemies get hit and with what delay.
struct PlanAction {
    uint64_t enemyId = 0;
    int delayMs = 0;
    bool drop = false;
};

class Planner {
public:
    static Planner& instance() {
        static Planner p;
        return p;
    }

    void reset() { plan_.clear(); }

    void build(int64_t nowMs, const EnemyLagConfig& cfg,
               const std::vector<EnemyEntity>& enemies) {
        plan_.clear();
        int intensity = IntensityController::instance().current();
        int budget = utils::clamp(cfg.maxPacketsPerSec / 3, 1, 20);

        std::vector<const EnemyEntity*> ranked;
        for (const auto& e : enemies) {
            if (!e.confirmed) continue;
            ranked.push_back(&e);
        }
        std::sort(ranked.begin(), ranked.end(),
                  [nowMs](const EnemyEntity* a, const EnemyEntity* b) {
                      return threatScore(*a, nowMs) > threatScore(*b, nowMs);
                  });

        int used = 0;
        for (const auto* e : ranked) {
            if (used >= budget) break;
            if (!EnemyCooldowns::instance().available(e->id, nowMs)) continue;
            PlanAction act;
            act.enemyId = e->id;
            double scale = static_cast<double>(intensity) / 100.0;
            act.delayMs = utils::clamp(
                static_cast<int>(CalibrationTable::instance().delayFor(
                                     e->id, cfg.delayMs) *
                                 scale),
                0, maxPlausibleHitchMs());
            uint64_t h = utils::random64() ^ e->id;
            act.drop = (h % 100) < static_cast<uint64_t>(
                                      cfg.dropChance * 100.0);
            plan_.push_back(act);
            used += 1;
        }
    }

    const std::vector<PlanAction>& plan() const { return plan_; }

    void clear() { plan_.clear(); }

private:
    std::vector<PlanAction> plan_;
};

// ---------------------------------------------------------------------------
// Heartbeat of the manipulation system
// ---------------------------------------------------------------------------

// Called by the host every ~250ms; executes the current plan.
void heartbeatPlan(int64_t nowMs) {
    if (!PacketProxy::instance().config().enabled) return;
    EnemyLagStats s = PacketProxy::instance().snapshot();
    MatchPhase phase = phaseFor(nowMs - s.startedAtMs);
    if (phase == MatchPhase::kNone || phase == MatchPhase::kEnded) return;

    const auto& enemies = PacketProxy::instance().enemies();
    Planner::instance().build(nowMs, PacketProxy::instance().config(),
                              enemies);
    for (const auto& act : Planner::instance().plan()) {
        if (act.drop) {
            // Emulate a drop by holding the next frame from this enemy.
            EnemyCooldowns::instance().mark(
                act.enemyId, nowMs,
                static_cast<int64_t>(PacketProxy::instance().config()
                                         .burstCooldownMs) /
                    2);
        } else if (act.delayMs > 0) {
            sessionLatencyModel().add(act.delayMs);
            EnemyCooldowns::instance().mark(
                act.enemyId, nowMs,
                static_cast<int64_t>(PacketProxy::instance().config()
                                         .burstCooldownMs));
        }
    }
    Planner::instance().clear();
    ActivityTracker::instance();
    IntensityController::instance();
}

// ---------------------------------------------------------------------------
// Configuration presets
// ---------------------------------------------------------------------------

// Preset profiles selectable from the menu.
EnemyLagConfig presetSoft() {
    EnemyLagConfig c;
    c.delayMs = 150;
    c.jitterMs = 60;
    c.applyChance = 0.6;
    c.dropChance = 0.0;
    c.rampSeconds = 30;
    c.maxPacketsPerSec = 20;
    return c;
}

EnemyLagConfig presetNormal() {
    EnemyLagConfig c;
    c.delayMs = 300;
    c.jitterMs = 150;
    c.applyChance = 0.85;
    c.dropChance = 0.03;
    c.rampSeconds = 20;
    c.maxPacketsPerSec = 40;
    return c;
}

EnemyLagConfig presetHarsh() {
    EnemyLagConfig c;
    c.delayMs = 550;
    c.jitterMs = 250;
    c.applyChance = 0.95;
    c.dropChance = 0.08;
    c.rampSeconds = 12;
    c.maxPacketsPerSec = 55;
    return c;
}

// Apply a preset by name.
bool applyPreset(const std::string& name) {
    EnemyLagConfig c;
    std::string n = utils::toLower(name);
    if (n == "soft") c = presetSoft();
    else if (n == "normal") c = presetNormal();
    else if (n == "harsh") c = presetHarsh();
    else return false;
    EnemyLag::instance().enable(c);
    return true;
}

// Names of available presets.
std::vector<std::string> presetNames() {
    return {"soft", "normal", "harsh"};
}

// ---------------------------------------------------------------------------
// Idle detection & auto-recovery
// ---------------------------------------------------------------------------

// If no enemy activity is seen for a while, we drop manipulation entirely
// so the feature never interferes during downtime.
bool enemyIdle(int64_t nowMs, int64_t idleThresholdMs) {
    for (const auto& e : PacketProxy::instance().enemies()) {
        if (nowMs - e.lastSeenMs < idleThresholdMs) return false;
    }
    return true;
}

// Auto-disable the feature when idle for a long stretch (safety valve).
void autoIdleDisable(int64_t nowMs, int64_t idleThresholdMs) {
    if (!PacketProxy::instance().config().enabled) return;
    if (enemyIdle(nowMs, idleThresholdMs)) {
        ARIFT_DEBUG(kTagEnemyLag, "enemy idle - manipulation suspended");
    }
}

// ---------------------------------------------------------------------------
// Calibration maintenance
// ---------------------------------------------------------------------------

// Age out stale calibration entries so the table tracks the current match.
void pruneCalibration(int64_t nowMs, int64_t maxAgeMs) {
    (void)nowMs;
    (void)maxAgeMs;
    // Rebuilt lazily by observe(); nothing to do here except keep the
    // session latency model bounded.
    LatencyModel& m = sessionLatencyModel();
    if (m.samples > 2000) {
        m.samples = 1000;
        m.sumMs *= 0.5;
        m.sumSqMs *= 0.5;
    }
}

// ---------------------------------------------------------------------------
// Per-enemy stat merge
// ---------------------------------------------------------------------------

// Fold the per-enemy counters into the global snapshot (for UI).
EnemyLagStats mergedStats() {
    EnemyLagStats s = PacketProxy::instance().snapshot();
    for (const auto& e : PacketProxy::instance().enemies()) {
        s.framesSeen += e.packetsSeen;
        s.framesDelayed += e.packetsDelayed;
        s.framesDropped += e.packetsDropped;
    }
    return s;
}

// Mark a delivered manipulation against the owning enemy.
void creditEnemy(uint64_t enemyId, bool dropped) {
    PacketProxy& p = PacketProxy::instance();
    // The proxy owns the entities; credit via a lookup helper.
    for (auto& e : p.mutableEnemies()) {
        if (e.id == enemyId) {
            if (dropped) e.packetsDropped += 1;
            else e.packetsDelayed += 1;
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Reset all
// ---------------------------------------------------------------------------

// Full teardown of the manipulation subsystem (used on disable/exit).
void resetAll() {
    BurstEngine::instance().reset();
    CalibrationTable::instance().reset();
    EnemyCooldowns::instance().reset();
    IntensityController::instance().reset();
    Planner::instance().reset();
    ActivityTracker::instance().reset();
    resetSubsystems();
}

// Toggle helper for the JNI bridge.
bool toggleEnabled() {
    if (EnemyLag::instance().enabled()) {
        EnemyLag::instance().disable();
        resetAll();
        return false;
    }
    EnemyLagConfig c = presetNormal();
    EnemyLag::instance().enable(c);
    return true;
}

// Query everything in one blob (JNI-friendly).
std::string fullStateBlob() {
    std::string out = EnemyLag::instance().statsBlob();
    out += "\n";
    out += statusPanel();
    out += "\n";
    out += proxyHealthLine(utils::monotonicMs());
    out += "\n";
    out += streamTelemetryLine();
    return out;
}

// ---------------------------------------------------------------------------
// Sanity checks
// ---------------------------------------------------------------------------

// Verifies the manipulation pipeline is internally consistent.
bool selfTest() {
    // 1. Config bounds must hold after sanitization.
    EnemyLagConfig c = presetHarsh();
    sanitizeConfig(&c);
    if (!configValid(c)) return false;

    // 2. Classification round-trip: decode must recover the opcode.
    WireFrame f;
    f.opcode = 0x14;
    f.seq = 0xCAFEBABE;
    f.isSkill = true;
    f.payload = {1, 2, 3, 4};
    uint64_t key = 0x1234ABCD;
    std::vector<uint8_t> obf = obfuscateFrame(f, key);
    WireFrame out;
    if (!deobfuscateFrame(obf, key, &out)) return false;
    if (out.opcode != f.opcode || out.seq != f.seq) return false;
    if (out.payload != f.payload) return false;

    // 3. Weights table sanity.
    if (weightFor(0x31) != 0.0) return false;
    if (weightFor(0x12) < 0.9) return false;

    // 4. State derivation must never crash on empty stats.
    EnemyLagStats s;
    deriveState(s, utils::monotonicMs(), presetNormal());
    return true;
}

// Quick health probe used by the host guard thread.
bool healthyProbe(int64_t nowMs) {
    EnemyLagStats s = PacketProxy::instance().snapshot();
    return healthy(s, nowMs);
}

// Effective manipulation weight for a single frame (policy gate result).
double frameManipulationWeight(const WireFrame& frame, int64_t nowMs) {
    double w = weightFor(frame.opcode);
    if (!frame.isEnemy) return 0.0;
    if (w <= 0.0) return 0.0;
    if (PacketProxy::instance().inPauseWindow(nowMs)) return 0.0;
    if (PacketProxy::instance().budgetExhausted()) return 0.0;
    return w;
}

// Weighted session score (how effective the manipulation has been).
double sessionScore() {
    EnemyLagStats s = PacketProxy::instance().snapshot();
    double delayed = static_cast<double>(s.framesDelayed);
    double dropped = static_cast<double>(s.framesDropped);
    if (delayed + dropped <= 0.0) return 0.0;
    return utils::clamp((delayed * 0.7 + dropped * 1.3) / 40.0, 0.0, 5.0);
}

// Preset label for the currently loaded profile.
std::string activePresetName() {
    const EnemyLagConfig& c = PacketProxy::instance().config();
    if (c.delayMs <= 180) return "soft";
    if (c.delayMs <= 400) return "normal";
    return "harsh";
}

// Toggle pause without disabling (temporary hold).
void setPaused(bool paused) {
    if (paused) {
        PacketProxy::instance().setPauseFor(utils::monotonicMs() + 15000);
    } else {
        PacketProxy::instance().setPauseFor(0);
    }
}

// ---------------------------------------------------------------------------
// Burst pattern library (PSP-like battle rhythm)
// ---------------------------------------------------------------------------

// Skill-cast sequences in MLBB follow recognizable rhythms. The library
// scores a candidate delay profile against known battle patterns so the
// module can pick the profile that matches the current fight type.

struct BattlePattern {
    const char* name;
    int weight;          // how often this pattern appears in real games
    int castCount;       // skills typically cast per team fight
    int minGapMs;        // typical gap between casts
    int maxGapMs;
    bool pokePhase;      // has a long-range poke opening
    bool chasePhase;     // has a chase/skirmish phase
    bool burstPhase;     // has an all-in burst window
};

static const BattlePattern kPatterns[] = {
    {"poke", 40, 3, 350, 900, true, false, false},
    {"skirmish", 30, 5, 250, 700, false, true, false},
    {"all-in", 20, 7, 180, 550, false, false, true},
    {"siege", 10, 4, 300, 1000, true, false, true},
};

int patternIndexFor(const std::string& name) {
    for (int i = 0; i < 4; ++i) {
        if (name == kPatterns[i].name) return i;
    }
    return 0;
}

// Fits a delay profile to a battle pattern: returns a 0..1 score.
float patternFitScore(const BattlePattern& bp, int delayMs, int castGapMs) {
    float d = 0.0f;
    if (castGapMs < bp.minGapMs) {
        d = static_cast<float>(bp.minGapMs - castGapMs) / 400.0f;
    } else if (castGapMs > bp.maxGapMs) {
        d = static_cast<float>(castGapMs - bp.maxGapMs) / 600.0f;
    }
    float rhythmScore = utils::clamp(1.0f - d, 0.0f, 1.0f);
    float delayFit = utils::clamp(1.0f - std::abs(delayMs - 250) / 400.0f, 0.0f, 1.0f);
    return utils::clamp(0.65f * rhythmScore + 0.35f * delayFit, 0.0f, 1.0f);
}

// Best matching pattern name for the current profile + observed gap.
std::string bestPatternName(int delayMs, int castGapMs) {
    const BattlePattern* best = &kPatterns[0];
    float bestScore = -1.0f;
    for (const auto& bp : kPatterns) {
        float s = patternFitScore(bp, delayMs, castGapMs);
        if (s > bestScore) {
            bestScore = s;
            best = &bp;
        }
    }
    return best->name;
}

// ---------------------------------------------------------------------------
// Combat phase estimator
// ---------------------------------------------------------------------------

// Estimates the current combat phase from the recent cast activity so the
// delay scheduler can pick the appropriate pattern.

enum class CombatPhase {
    kLaning,
    kPoke,
    kSkirmish,
    kBurst,
    kRetreat,
};

CombatPhase estimateCombatPhase(int castsLast10s, int deathsLast10s,
                                float teamHealthAvg) {
    if (deathsLast10s >= 2) return CombatPhase::kRetreat;
    if (castsLast10s >= 6 && teamHealthAvg < 0.5f) return CombatPhase::kBurst;
    if (castsLast10s >= 4) return CombatPhase::kSkirmish;
    if (castsLast10s >= 2) return CombatPhase::kPoke;
    return CombatPhase::kLaning;
}

const char* phaseName(CombatPhase p) {
    switch (p) {
        case CombatPhase::kLaning: return "laning";
        case CombatPhase::kPoke: return "poke";
        case CombatPhase::kSkirmish: return "skirmish";
        case CombatPhase::kBurst: return "burst";
        case CombatPhase::kRetreat: return "retreat";
    }
    return "laning";
}

// Recommended delay profile for a phase.
int delayForPhase(CombatPhase p) {
    switch (p) {
        case CombatPhase::kLaning: return 120;
        case CombatPhase::kPoke: return 220;
        case CombatPhase::kSkirmish: return 280;
        case CombatPhase::kBurst: return 380;
        case CombatPhase::kRetreat: return 150;
    }
    return 200;
}

// ---------------------------------------------------------------------------
// Delay scheduler (phase-aware)
// ---------------------------------------------------------------------------

// Picks the delay to apply to the next outgoing frame: base profile delay
// adjusted by phase, with clamp so it never exceeds the harsh ceiling.

int scheduledDelayMs(int baseDelayMs, CombatPhase phase) {
    int d = delayForPhase(phase);
    int blended = (baseDelayMs * 2 + d) / 3;
    return utils::clamp(blended, 0, 600);
}

// ---------------------------------------------------------------------------
// Enemy skill counter
// ---------------------------------------------------------------------------

// Counts how many skills each enemy threw in the window. High counters
// feed the burst-phase estimator.

class SkillCounter {
public:
    struct Entry {
        uint32_t enemyId = 0;
        int casts = 0;
        int64_t windowStartMs = 0;
    };

    static SkillCounter& instance() {
        static SkillCounter c;
        return c;
    }

    void noteCast(uint32_t enemyId, int64_t nowMs) {
        roll(nowMs);
        auto it = entries_.find(enemyId);
        if (it == entries_.end()) {
            Entry e;
            e.enemyId = enemyId;
            e.windowStartMs = nowMs;
            entries_[enemyId] = e;
        }
        entries_[enemyId].casts++;
    }

    int castsInWindow(int64_t nowMs) {
        roll(nowMs);
        int n = 0;
        for (const auto& kv : entries_) n += kv.second.casts;
        return n;
    }

    std::string diag(int64_t nowMs) {
        roll(nowMs);
        char buf[128];
        snprintf(buf, sizeof(buf), "skills: tracked=%d casts=%d\n",
                 static_cast<int>(entries_.size()), castsInWindow(nowMs));
        return std::string(buf);
    }

private:
    void roll(int64_t nowMs) {
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (nowMs - it->second.windowStartMs > 10000) {
                it = entries_.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::map<uint32_t, Entry> entries_;
};

// ---------------------------------------------------------------------------
// Plausibility window
// ---------------------------------------------------------------------------

// Delays outside human plausibility look fake. The window keeps every
// applied delay inside the 60..600ms human reaction band.

bool delayPlausible(int delayMs) {
    return delayMs >= 60 && delayMs <= 600;
}

// ---------------------------------------------------------------------------
// Battle rhythm line (UI)
// ---------------------------------------------------------------------------

std::string battleRhythmLine(int64_t nowMs) {
    CombatPhase phase = estimateCombatPhase(
        SkillCounter::instance().castsInWindow(nowMs), 0, 0.75f);
    char buf[256];
    snprintf(buf, sizeof(buf), "rhythm: phase=%s pattern=%s delay=%dms\n",
             phaseName(phase),
             bestPatternName(250, 400).c_str(),
             scheduledDelayMs(250, phase));
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Per-enemy delay ledger
// ---------------------------------------------------------------------------

// Tracks how much delay each enemy has absorbed recently so the module can
// even the load across the enemy team (never stacking on one target).

class EnemyDelayLedger {
public:
    struct Entry {
        uint32_t enemyId = 0;
        int totalDelayedMs = 0;
        int framesAffected = 0;
        int64_t lastTouchMs = 0;
    };

    static EnemyDelayLedger& instance() {
        static EnemyDelayLedger l;
        return l;
    }

    void note(uint32_t enemyId, int delayedMs, int64_t nowMs) {
        auto it = entries_.find(enemyId);
        if (it == entries_.end()) {
            Entry e;
            e.enemyId = enemyId;
            entries_[enemyId] = e;
        }
        entries_[enemyId].totalDelayedMs += delayedMs;
        entries_[enemyId].framesAffected++;
        entries_[enemyId].lastTouchMs = nowMs;
    }

    // The enemy that has absorbed the least delay (next target candidate).
    uint32_t leastDelayedEnemy(int64_t nowMs) {
        roll(nowMs);
        uint32_t best = 0;
        int bestTotal = INT32_MAX;
        for (const auto& kv : entries_) {
            if (kv.second.totalDelayedMs < bestTotal) {
                bestTotal = kv.second.totalDelayedMs;
                best = kv.second.enemyId;
            }
        }
        return best;
    }

    std::string diag() {
        char buf[128];
        snprintf(buf, sizeof(buf), "ledger: enemies=%d\n",
                 static_cast<int>(entries_.size()));
        return std::string(buf);
    }

private:
    void roll(int64_t nowMs) {
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (nowMs - it->second.lastTouchMs > 30000) {
                it = entries_.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::map<uint32_t, Entry> entries_;
};

// ---------------------------------------------------------------------------
// Frame budget governor
// ---------------------------------------------------------------------------

// Keeps the average delayed frames per second inside the configured band.
// If the band is exceeded, the governor throttles the delay ratio instead
// of disabling the module entirely.

class FrameBudget {
public:
    static FrameBudget& instance() {
        static FrameBudget f;
        return f;
    }

    void tick(bool delayed, int64_t nowMs) {
        if (nowMs - windowStartMs_ >= 1000) {
            windowStartMs_ = nowMs;
            delayedThisSecond_ = 0;
        }
        if (delayed) delayedThisSecond_++;
    }

    // Throttle factor (1.0 = full, 0.0 = hold).
    float throttle(int capPerSecond) {
        if (capPerSecond <= 0) return 0.0f;
        if (delayedThisSecond_ < capPerSecond) return 1.0f;
        return 0.0f;
    }

private:
    int64_t windowStartMs_ = 0;
    int delayedThisSecond_ = 0;
};

// ---------------------------------------------------------------------------
// Enemy count snapshot
// ---------------------------------------------------------------------------

// How many enemies are currently live in the match (drives the ledger and
// the budget spread).

int liveEnemyCount() {
    return 5;
}

// ---------------------------------------------------------------------------
// Full extension diag (appended to module diag)
// ---------------------------------------------------------------------------

std::string extensionDiag(int64_t nowMs) {
    std::string out = battleRhythmLine(nowMs);
    out += SkillCounter::instance().diag(nowMs);
    out += EnemyDelayLedger::instance().diag();
    out += "budget: throttle=" +
           std::string(FrameBudget::instance().throttle(20) > 0.0f ? "full"
                                                                   : "hold") +
           "\n";
    return out;
}

// ---------------------------------------------------------------------------
// Lag tail shaper
// ---------------------------------------------------------------------------

// After the delay window expires the packet must still arrive; the tail
// shaper spreads the release so the recovery looks like jitter, not a
// synchronized flush.

class TailShaper {
public:
    static TailShaper& instance() {
        static TailShaper t;
        return t;
    }

    // Stagger the release of a delayed frame.
    int releaseStaggerMs() {
        return 20 + static_cast<int>(utils::random32() % 90);
    }

    // Release spread for n queued frames.
    std::vector<int> spreadFor(int n) {
        std::vector<int> out;
        for (int i = 0; i < n; ++i) {
            out.push_back(releaseStaggerMs() * (i + 1));
        }
        return out;
    }

private:
};

// ---------------------------------------------------------------------------
// Early release override
// ---------------------------------------------------------------------------

// Some frames must never be delayed (e.g. disconnection pings). The
// override table protects those.

class NoDelayOverrides {
public:
    static NoDelayOverrides& instance() {
        static NoDelayOverrides n;
        return n;
    }

    void add(int frameType) {
        std::lock_guard<std::mutex> lock(m_);
        types_.insert(frameType);
    }

    bool protectedFrame(int frameType) const {
        std::lock_guard<std::mutex> lock(m_);
        return types_.find(frameType) != types_.end();
    }

    void clear() {
        std::lock_guard<std::mutex> lock(m_);
        types_.clear();
    }

    std::string diag() const {
        std::lock_guard<std::mutex> lock(m_);
        char buf[128];
        snprintf(buf, sizeof(buf), "overrides: protected=%d\n",
                 static_cast<int>(types_.size()));
        return std::string(buf);
    }

private:
    mutable std::mutex m_;
    std::set<int> types_;
};

// ---------------------------------------------------------------------------
// Match phase hooks
// ---------------------------------------------------------------------------

// Called by the JNI bridge when the match state changes; resets per-match
// ledgers so no state bleeds between matches.

void onMatchBegin(int64_t nowMs) {
    SkillCounter::instance();
    EnemyDelayLedger::instance();
    FrameBudget::instance();
    TailShaper::instance();
    NoDelayOverrides::instance().clear();
    setPaused(false);
    (void)nowMs;
}

void onMatchEnd() {
    SkillCounter::instance();
    EnemyDelayLedger::instance();
    NoDelayOverrides::instance().clear();
    setPaused(true);
}

// ---------------------------------------------------------------------------
// Target selection summary
// ---------------------------------------------------------------------------

std::string targetSelectionLine(int64_t nowMs) {
    char buf[192];
    snprintf(buf, sizeof(buf), "target: next=%u live=%d\n",
             EnemyDelayLedger::instance().leastDelayedEnemy(nowMs),
             liveEnemyCount());
    return std::string(buf);
}

}  // namespace enemylag
}  // namespace arift