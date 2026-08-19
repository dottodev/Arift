#include "autoaim.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

#include "arift_log.h"
#include "arift_utils.h"
#include "feature_switch.h"

namespace arift {
namespace autoaim {

namespace {

float distXz(const Vec3& a, const Vec3& b) {
    float dx = a.x - b.x;
    float dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}

float clamp01(float v) {
    return std::max(0.0f, std::min(1.0f, v));
}

bool featureActive() {
    return FeatureSwitch::instance().isEnabled(kFeatureAutoAim);
}

// Squishiness = armor + magic resist (lower = squishier).
float squishiness(const PlayerSnapshot& p) {
    return p.armor + p.magicResist;
}

// Composite priority: squishy targets get a boost.
float heroPriorityWeight(const PlayerSnapshot& p) {
    if (p.kind == EntityKind::kUnknown) return 0.5f;
    return 1.0f;
}

}  // namespace

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------

void enable(bool on) {
    AutoAim::instance().setEnabled(on);
    FeatureSwitch::instance().set(kFeatureAutoAim, on);
}

bool enabled() {
    return AutoAim::instance().running() && featureActive();
}

void setMode(int mode) {
    AutoAim::instance().setMode(mode);
}

void setFocus(uint32_t heroId) {
    AutoAim::instance().setFocus(heroId);
}

// ---------------------------------------------------------------------------
// Target scoring
// ---------------------------------------------------------------------------

// Scores every enemy by the active mode and returns the best one.

TargetScore scoreTarget(const PlayerSnapshot& t, const PlayerSnapshot& self,
                        TargetMode mode, uint32_t focusId) {
    TargetScore s;
    s.heroId = t.id;
    s.heroName = t.heroName;
    s.distance = distXz(t.position, self.position);
    s.hpRatio = t.healthRatio();
    s.armor = t.armor;
    s.magicResist = t.magicResist;
    s.alive = t.alive;
    s.visible = !t.inFog;
    if (!t.alive || t.inFog) {
        s.score = -1.0f;
        return s;
    }
    switch (mode) {
        case TargetMode::kLowestHp:
            s.score = (1.0f - s.hpRatio) * 100.0f - s.distance * 0.01f;
            break;
        case TargetMode::kClosest:
            s.score = -s.distance;
            break;
        case TargetMode::kPriority:
            s.score = heroPriorityWeight(t) * 60.0f - s.distance * 0.01f;
            break;
        case TargetMode::kSquishiest:
            s.score = (60.0f - squishiness(t)) * 1.5f - s.distance * 0.01f;
            break;
        case TargetMode::kFocus:
            s.score = (t.id == focusId) ? 1000.0f : -1000.0f;
            break;
    }
    return s;
}

TargetScore bestTarget(const std::vector<PlayerSnapshot>& players,
                       const PlayerSnapshot& self, TargetMode mode,
                       uint32_t focusId) {
    TargetScore best;
    best.score = -1e9f;
    for (const auto& p : players) {
        if (p.team != EntityTeam::kEnemy) continue;
        TargetScore s = scoreTarget(p, self, mode, focusId);
        if (s.score > best.score) best = s;
    }
    return best;
}

// ---------------------------------------------------------------------------
// Lock building
// ---------------------------------------------------------------------------

TargetLock buildLock(const TargetScore& s, const PlayerSnapshot& target,
                     const PlayerSnapshot& self, int64_t nowMs) {
    TargetLock lock;
    if (s.heroId == 0) return lock;
    lock.locked = true;
    lock.heroId = s.heroId;
    lock.heroName = s.heroName;
    lock.position = target.position;
    lock.velocity = target.velocity;
    lock.distance = s.distance;
    lock.hpRatio = s.hpRatio;
    lock.priorityScore = s.score;
    lock.lockedAtMs = nowMs;
    return lock;
}

TargetLock lockTarget(const std::vector<PlayerSnapshot>& players,
                      const PlayerSnapshot& self, int64_t nowMs) {
    if (!enabled()) return TargetLock();
    AutoAimConfig cfg = AutoAim::instance().cfg();
    TargetScore s = bestTarget(players, self, cfg.mode, cfg.focusHeroId);
    if (s.heroId == 0 || s.score < 0.0f) return TargetLock();
    for (const auto& p : players) {
        if (p.id == s.heroId) {
            return buildLock(s, p, self, nowMs);
        }
    }
    return TargetLock();
}

// ---------------------------------------------------------------------------
// Lead prediction
// ---------------------------------------------------------------------------

// Computes where a moving target will be when a projectile arrives.

Vec3 predictedPosition(const TargetLock& t, float travelSpeed,
                       float castTimeMs, bool lead) {
    Vec3 aim = t.position;
    if (!lead || travelSpeed <= 0.0f) return aim;
    float dist = distXz(t.position, t.velocity);
    (void)dist;
    float gap = distXz(t.position, Vec3());
    (void)gap;
    float timeToHit = t.distance / travelSpeed;
    if (timeToHit < 0.0f) return aim;
    timeToHit += castTimeMs / 1000.0f;
    aim.x += t.velocity.x * timeToHit;
    aim.z += t.velocity.z * timeToHit;
    return aim;
}

// ---------------------------------------------------------------------------
// Aim solver
// ---------------------------------------------------------------------------

AimSolution solveAim(const TargetLock& target, const PlayerSnapshot& self,
                     const SkillShotParams& params, int64_t nowMs) {
    AimSolution sol;
    if (!target.locked) return sol;
    AutoAimConfig cfg = AutoAim::instance().cfg();
    bool lead = cfg.leadMovingTargets && params.travelSpeed > 0.0f;
    Vec3 aim = predictedPosition(target, params.travelSpeed,
                                 params.castTimeMs, lead);
    float dx = aim.x - self.position.x;
    float dz = aim.z - self.position.z;
    float len = std::sqrt(dx * dx + dz * dz);
    if (len < 1.0f) return sol;
    sol.direction.x = dx / len;
    sol.direction.z = dz / len;
    sol.aimPoint = aim;
    sol.valid = len <= params.maxRange || !cfg.clampToScreen;
    float leadT = 0.0f;
    if (lead && params.travelSpeed > 0.0f) {
        leadT = target.distance / params.travelSpeed;
    }
    sol.usesLead = lead && leadT > 0.05f;
    sol.leadSeconds = leadT;
    float hpFactor = 1.0f - target.hpRatio * 0.3f;
    sol.confidence = clamp01(hpFactor * (1.0f - target.distance / 1500.0f));
    return sol;
}

// ---------------------------------------------------------------------------
// Aim execution
// ---------------------------------------------------------------------------

// Records the aim action; a real deployment converts the direction into
// a joystick/tap command.

void aimAt(const AimSolution& solution) {
    if (!solution.valid) return;
    char buf[128];
    snprintf(buf, sizeof(buf), "aim (%.2f,%.2f) conf=%.2f lead=%.1fs",
             solution.direction.x, solution.direction.z,
             solution.confidence, solution.leadSeconds);
    ARIFT_DEBUG(kTagAutoAim, "%s", buf);
}

// ---------------------------------------------------------------------------
// Basic-attack assist
// ---------------------------------------------------------------------------

// Assists basic attacks when the target is inside basic range.

bool basicAttackWanted(const TargetLock& t, const PlayerSnapshot& self,
                       float basicRange) {
    if (!t.locked) return false;
    return t.distance <= basicRange && t.hpRatio > 0.0f;
}

// ---------------------------------------------------------------------------
// Skill selection
// ---------------------------------------------------------------------------

// Picks the skill to use against the locked target (damage skills first).

int pickSkill(const PlayerSnapshot& self, const TargetLock& target,
              const SkillShotParams params[], int count) {
    for (int i = 0; i < count; ++i) {
        const SkillShotParams& p = params[i];
        if (p.maxRange <= 0.0f) continue;
        if (target.distance <= p.maxRange) return i;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Diag lines
// ---------------------------------------------------------------------------

std::string autoAimDiag() {
    AutoAim& m = AutoAim::instance();
    TargetLock lock = m.currentLock();
    char buf[256];
    snprintf(buf, sizeof(buf),
             "autoaim: %s mode=%d lock=%s(%u) dist=%.0f hp=%.0f%%\n",
             m.running() ? "ON" : "OFF", static_cast<int>(m.cfg().mode),
             lock.locked ? lock.heroName.c_str() : "none", lock.heroId,
             lock.distance, lock.hpRatio * 100.0f);
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// AutoAim implementation
// ---------------------------------------------------------------------------

AutoAim& AutoAim::instance() {
    static AutoAim a;
    return a;
}

int AutoAim::start() {
    if (running_.load()) return 0;
    running_.store(true);
    if (!thread_.start([this] { loop(); })) {
        running_.store(false);
        return -1;
    }
    ARIFT_INFO(kTagAutoAim, "Auto Aim started");
    return 0;
}

int AutoAim::stop() {
    if (!running_.load()) return 0;
    running_.store(false);
    thread_.join();
    ARIFT_INFO(kTagAutoAim, "Auto Aim stopped");
    return 0;
}

void AutoAim::setEnabled(bool v) {
    cfg_.enabled = v;
    if (v) {
        start();
    } else {
        stop();
    }
}

void AutoAim::setMode(int m) {
    cfg_.mode = static_cast<TargetMode>(std::max(0, std::min(4, m)));
}

void AutoAim::setFocus(uint32_t heroId) {
    cfg_.focusHeroId = heroId;
    cfg_.mode = TargetMode::kFocus;
}

void AutoAim::feed(const std::vector<PlayerSnapshot>& players,
                   const PlayerSnapshot& self, int64_t nowMs) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    players_ = players;
    self_ = self;
    nowMs_ = nowMs;
}

TargetLock AutoAim::currentLock() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return lock_;
}

AimSolution AutoAim::currentSolution() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return solution_;
}

void AutoAim::loop() {
    int64_t last = 0;
    while (running_.load()) {
        int64_t now = utils::monotonicMs();
        if (now - last >= 100) {
            last = now;
            std::vector<PlayerSnapshot> players;
            PlayerSnapshot self;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                players = players_;
                self = self_;
            }
            TargetLock lock = lockTarget(players, self, now);
            SkillShotParams p;
            p.travelSpeed = 1000.0f;
            p.maxRange = cfg_.skillShotRange;
            p.castTimeMs = 200.0f;
            AimSolution sol = solveAim(lock, self, p, now);
            std::lock_guard<std::mutex> lock2(state_mutex_);
            lock_ = lock;
            solution_ = sol;
        }
        Thread::sleepMs(20);
    }
}

const AutoAimConfig& AutoAim::cfg() const {
    return cfg_;
}

std::string AutoAim::diag() const {
    return autoAimDiag();
}

// ---------------------------------------------------------------------------
// Aim smoothing
// ---------------------------------------------------------------------------

// Instant aim snapping looks robotic; smoothing interpolates the aim
// direction over several frames.

Vec3 smoothedDirection(const Vec3& current, const Vec3& desired,
                       float smoothing) {
    float k = clamp01(smoothing);
    Vec3 out;
    out.x = current.x + (desired.x - current.x) * k;
    out.z = current.z + (desired.z - current.z) * k;
    float len = std::sqrt(out.x * out.x + out.z * out.z);
    if (len > 0.0f) {
        out.x /= len;
        out.z /= len;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Aim error budget
// ---------------------------------------------------------------------------

// Real aim has error; the budget adds human-like scatter that scales
// with distance and fades when the target is low HP.

float aimErrorUnits(float distance, float hpRatio) {
    float base = distance * 0.02f;
    float hpPenalty = (1.0f - hpRatio) * 15.0f;
    return std::max(0.0f, base + hpPenalty);
}

Vec3 jitteredAimPoint(const Vec3& aimPoint, const Vec3& selfPos,
                      float errorUnits) {
    if (errorUnits <= 0.0f) return aimPoint;
    float dx = aimPoint.x - selfPos.x;
    float dz = aimPoint.z - selfPos.z;
    float len = std::sqrt(dx * dx + dz * dz);
    if (len < 1.0f) return aimPoint;
    Vec3 out = aimPoint;
    float perpX = -dz / len * errorUnits;
    float perpZ = dx / len * errorUnits;
    float side = (utils::random32() % 2 == 0) ? 1.0f : -1.0f;
    out.x += perpX * side;
    out.z += perpZ * side;
    return out;
}

// ---------------------------------------------------------------------------
// Target switching
// ---------------------------------------------------------------------------

// Keeps the current target while it is alive; switches only when the
// score gap is large (avoids flapping between targets).

class TargetStickiness {
public:
    static TargetStickiness& instance() {
        static TargetStickiness t;
        return t;
    }

    void remember(uint32_t heroId, int64_t nowMs) {
        heroId_ = heroId;
        lastMs_ = nowMs;
    }

    uint32_t held(uint32_t newBest, float scoreGap) {
        if (heroId_ == 0) return newBest;
        if (newBest == heroId_) return newBest;
        if (scoreGap < 15.0f) return heroId_;
        return newBest;
    }

    void clear() { heroId_ = 0; }

private:
    uint32_t heroId_ = 0;
    int64_t lastMs_ = 0;
};

// ---------------------------------------------------------------------------
// Threat escape aim
// ---------------------------------------------------------------------------

// When the hero is low HP, aim flips to a defensive posture: prefer the
// nearest threat and stop overextending.

bool defensivePosture(const PlayerSnapshot& self) {
    return self.healthRatio() < 0.3f;
}

// ---------------------------------------------------------------------------
// Skill-shot width check
// ---------------------------------------------------------------------------

// A skill with width can hit multiple targets; the check reports how
// many enemies are on the skill line.

int enemiesOnLine(const std::vector<PlayerSnapshot>& players,
                  const Vec3& from, const Vec3& to, float width,
                  uint32_t exceptId) {
    int n = 0;
    Vec3 dir(to.x - from.x, 0.0f, to.z - from.z);
    float len = std::sqrt(dir.x * dir.x + dir.z * dir.z);
    if (len < 1.0f) return 0;
    dir.x /= len;
    dir.z /= len;
    for (const auto& p : players) {
        if (p.team != EntityTeam::kEnemy) continue;
        if (p.id == exceptId) continue;
        if (!p.alive || p.inFog) continue;
        Vec3 rel(p.position.x - from.x, 0.0f, p.position.z - from.z);
        float along = rel.x * dir.x + rel.z * dir.z;
        if (along < 0.0f || along > len) continue;
        float perpX = rel.x - dir.x * along;
        float perpZ = rel.z - dir.z * along;
        float perp = std::sqrt(perpX * perpX + perpZ * perpZ);
        if (perp <= width) n++;
    }
    return n;
}

// ---------------------------------------------------------------------------
// Multi-hit selection
// ---------------------------------------------------------------------------

// Chooses the aim point that maximizes hits: either the single target
// or the midpoint of a cluster.

Vec3 multiHitAimPoint(const std::vector<PlayerSnapshot>& players,
                      const PlayerSnapshot& self,
                      const TargetLock& target, float skillWidth,
                      float maxRange) {
    Vec3 single = target.position;
    int singleHits = enemiesOnLine(players, self.position, single,
                                   skillWidth, target.heroId) + 1;
    Vec3 best = single;
    int bestHits = singleHits;
    for (const auto& p : players) {
        if (p.team != EntityTeam::kEnemy) continue;
        if (!p.alive || p.inFog) continue;
        float d = distXz(p.position, self.position);
        if (d > maxRange) continue;
        Vec3 mid((single.x + p.position.x) * 0.5f,
                 0.0f, (single.z + p.position.z) * 0.5f);
        int hits = enemiesOnLine(players, self.position, mid,
                                 skillWidth, 0) + 1;
        if (hits > bestHits) {
            bestHits = hits;
            best = mid;
        }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Aim clamp
// ---------------------------------------------------------------------------

// Clamps the aim point to the maximum skill range.

Vec3 clampToRange(const Vec3& from, const Vec3& aim, float maxRange) {
    float d = distXz(from, aim);
    if (d <= maxRange) return aim;
    float t = maxRange / d;
    Vec3 out;
    out.x = from.x + (aim.x - from.x) * t;
    out.z = from.z + (aim.z - from.z) * t;
    return out;
}

// ---------------------------------------------------------------------------
// Aim report line
// ---------------------------------------------------------------------------

std::string aimReportLine(const TargetLock& lock, const AimSolution& sol) {
    char buf[192];
    snprintf(buf, sizeof(buf),
             "aim: %s conf=%.2f lead=%.1fs dir=(%.2f,%.2f)\n",
             lock.locked ? lock.heroName.c_str() : "none",
             sol.confidence, sol.leadSeconds, sol.direction.x,
             sol.direction.z);
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Target switching (full pipeline)
// ---------------------------------------------------------------------------

TargetLock chooseLockWithStickiness(
    const std::vector<PlayerSnapshot>& players, const PlayerSnapshot& self,
    TargetMode mode, uint32_t focusId, int64_t nowMs) {
    TargetScore best = bestTarget(players, self, mode, focusId);
    if (best.heroId == 0) {
        TargetStickiness::instance().clear();
        return TargetLock();
    }
    float gap = 0.0f;
    uint32_t chosen = TargetStickiness::instance().held(best.heroId, gap);
    TargetStickiness::instance().remember(chosen, nowMs);
    for (const auto& p : players) {
        if (p.id == chosen) {
            return buildLock(best, p, self, nowMs);
        }
    }
    return TargetLock();
}

// ---------------------------------------------------------------------------
// Aim cadence
// ---------------------------------------------------------------------------

// Caps aim corrections per second to avoid jitter.

class AimCadence {
public:
    static AimCadence& instance() {
        static AimCadence c;
        return c;
    }

    bool allowed(int64_t nowMs, int minGapMs) {
        if (nowMs - lastMs_ < minGapMs) return false;
        lastMs_ = nowMs;
        return true;
    }

private:
    int64_t lastMs_ = 0;
};

// ---------------------------------------------------------------------------
// Full aim pipeline
// ---------------------------------------------------------------------------

// Runs the complete chain: lock -> solve -> smooth -> execute.

AimSolution runAimPipeline(const std::vector<PlayerSnapshot>& players,
                           const PlayerSnapshot& self,
                           const SkillShotParams& params, int64_t nowMs) {
    AimSolution none;
    if (!enabled()) return none;
    if (!AimCadence::instance().allowed(nowMs, 150)) return none;
    TargetLock lock = lockTarget(players, self, nowMs);
    if (!lock.locked) return none;
    AimSolution sol = solveAim(lock, self, params, nowMs);
    if (!sol.valid) return none;
    if (params.width > 0.0f) {
        Vec3 multi = multiHitAimPoint(players, self, lock, params.width,
                                      params.maxRange);
        sol.aimPoint = clampToRange(self.position, multi, params.maxRange);
    }
    aimAt(sol);
    return sol;
}

// ---------------------------------------------------------------------------
// Aim assist profiles
// ---------------------------------------------------------------------------

// Preset profiles for different hero classes: each profile tunes the
// smoothing, lead and range preferences.

struct AimProfile {
    const char* name;
    float smoothing;
    float skillShotRange;
    float basicRange;
    bool lead;
    int minGapMs;
};

static const AimProfile kAimProfiles[] = {
    {"marksman", 0.70f, 900.0f, 300.0f, true, 120},
    {"mage", 0.60f, 1000.0f, 200.0f, true, 180},
    {"assassin", 0.85f, 700.0f, 260.0f, true, 100},
    {"fighter", 0.75f, 650.0f, 240.0f, true, 140},
    {"tank", 0.50f, 500.0f, 180.0f, false, 200},
};

const AimProfile* profileFor(const std::string& role) {
    for (const auto& p : kAimProfiles) {
        if (role == p.name) return &p;
    }
    return &kAimProfiles[0];
}

// ---------------------------------------------------------------------------
// Aim trace
// ---------------------------------------------------------------------------

// A short history of aim decisions, used by diagnostics and by the
// smoothing logic to detect flapping.

class AimTrace {
public:
    static AimTrace& instance() {
        static AimTrace t;
        return t;
    }

    void push(const AimSolution& sol, int64_t nowMs) {
        if (traces_.size() > 64) traces_.erase(traces_.begin());
        traces_.push_back(std::make_pair(nowMs, sol));
    }

    // Flap count: direction reversals in the last window.
    int flapCount(int64_t windowMs) {
        int flaps = 0;
        for (size_t i = 1; i < traces_.size(); ++i) {
            if (traces_[i].first < traces_[i - 1].first + windowMs) {
                const AimSolution& a = traces_[i - 1].second;
                const AimSolution& b = traces_[i].second;
                float dot = a.direction.x * b.direction.x +
                            a.direction.z * b.direction.z;
                if (dot < -0.6f) flaps++;
            }
        }
        return flaps;
    }

    std::string diag() const {
        char buf[128];
        snprintf(buf, sizeof(buf), "aim_trace: samples=%d\n",
                 static_cast<int>(traces_.size()));
        return std::string(buf);
    }

private:
    std::vector<std::pair<int64_t, AimSolution>> traces_;
};

// ---------------------------------------------------------------------------
// Confidence gating
// ---------------------------------------------------------------------------

// Low-confidence solutions are suppressed instead of executed.

bool confidenceGate(const AimSolution& sol, float minConfidence) {
    return sol.valid && sol.confidence >= minConfidence;
}

// ---------------------------------------------------------------------------
// Basic attack solver
// ---------------------------------------------------------------------------

// Minimal solver for basic attacks: aim straight at the target with no
// lead (projectiles are fast) and high smoothing.

AimSolution solveBasicAttack(const TargetLock& target,
                             const PlayerSnapshot& self) {
    AimSolution sol;
    if (!target.locked) return sol;
    float dx = target.position.x - self.position.x;
    float dz = target.position.z - self.position.z;
    float len = std::sqrt(dx * dx + dz * dz);
    if (len < 1.0f) return sol;
    sol.direction.x = dx / len;
    sol.direction.z = dz / len;
    sol.aimPoint = target.position;
    sol.valid = target.distance <= 350.0f;
    sol.confidence = clamp01(1.0f - target.distance / 350.0f);
    return sol;
}

// ---------------------------------------------------------------------------
// Target history
// ---------------------------------------------------------------------------

// Remembers which heroes were engaged recently so the aim never snaps
// to a hero that was just killed.

class TargetHistory {
public:
    static TargetHistory& instance() {
        static TargetHistory h;
        return h;
    }

    void noteDead(uint32_t heroId, int64_t nowMs) {
        dead_[heroId] = nowMs;
    }

    bool recentlyDead(uint32_t heroId, int64_t nowMs) const {
        auto it = dead_.find(heroId);
        if (it == dead_.end()) return false;
        return nowMs - it->second < 30000;
    }

    void prune(int64_t nowMs) {
        for (auto it = dead_.begin(); it != dead_.end();) {
            if (nowMs - it->second >= 30000) {
                it = dead_.erase(it);
            } else {
                ++it;
            }
        }
    }

private:
    std::map<uint32_t, int64_t> dead_;
};

// ---------------------------------------------------------------------------
// Engaged enemies filter
// ---------------------------------------------------------------------------

// Returns only enemies that are currently visible and alive.

std::vector<PlayerSnapshot> engagedEnemies(
    const std::vector<PlayerSnapshot>& players) {
    std::vector<PlayerSnapshot> out;
    for (const auto& p : players) {
        if (p.team != EntityTeam::kEnemy) continue;
        if (!p.alive || p.inFog) continue;
        out.push_back(p);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Aim assist stats
// ---------------------------------------------------------------------------

class AimStats {
public:
    static AimStats& instance() {
        static AimStats s;
        return s;
    }

    void noteShot(bool hit, int64_t nowMs) {
        shots_++;
        if (hit) hits_++;
        if (nowMs - windowMs_ >= 60000) {
            windowMs_ = nowMs;
            windowShots_ = 0;
            windowHits_ = 0;
        }
        windowShots_++;
        if (hit) windowHits_++;
    }

    double accuracy() const {
        if (shots_ == 0) return 0.0;
        return static_cast<double>(hits_) / shots_;
    }

    double windowAccuracy() const {
        if (windowShots_ == 0) return 0.0;
        return static_cast<double>(windowHits_) / windowShots_;
    }

    std::string diag() const {
        char buf[128];
        snprintf(buf, sizeof(buf), "aim_stats: acc=%.0f%% win=%.0f%%\n",
                 accuracy() * 100.0, windowAccuracy() * 100.0);
        return std::string(buf);
    }

private:
    int shots_ = 0;
    int hits_ = 0;
    int windowShots_ = 0;
    int windowHits_ = 0;
    int64_t windowMs_ = 0;
};

// ---------------------------------------------------------------------------
// Full aim suite diag
// ---------------------------------------------------------------------------

std::string aimSuiteDiag() {
    std::string out = autoAimDiag();
    out += AimTrace::instance().diag();
    out += AimStats::instance().diag();
    return out;
}

// ---------------------------------------------------------------------------
// Focus persistence
// ---------------------------------------------------------------------------

// The focus target persists until it dies; only then the aim returns to
// the automatic modes.

class FocusPersistence {
public:
    static FocusPersistence& instance() {
        static FocusPersistence f;
        return f;
    }

    void setFocus(uint32_t heroId) {
        focus_ = heroId;
        dead_ = false;
    }

    void noteTargetState(uint32_t heroId, bool alive) {
        if (heroId != focus_) return;
        if (!alive) dead_ = true;
    }

    bool active() const { return focus_ != 0 && !dead_; }

    uint32_t focus() const { return focus_; }

    void clear() {
        focus_ = 0;
        dead_ = false;
    }

    std::string diag() const {
        char buf[96];
        snprintf(buf, sizeof(buf), "focus: %u active=%d\n", focus_,
                 active() ? 1 : 0);
        return std::string(buf);
    }

private:
    uint32_t focus_ = 0;
    bool dead_ = false;
};

// ---------------------------------------------------------------------------
// Aim priority reset
// ---------------------------------------------------------------------------

// Resets the aim mode back to automatic after the focus target dies.

void resetAfterFocusDeath() {
    FocusPersistence::instance().clear();
    AutoAim::instance().setMode(0);
}

// ---------------------------------------------------------------------------
// Adaptive lead
// ---------------------------------------------------------------------------

// Lead time adapts to the target's movement: stationary targets get
// zero lead, running targets get full lead.

float adaptiveLead(const TargetLock& target, float travelSpeed) {
    if (travelSpeed <= 0.0f) return 0.0f;
    float speed = std::sqrt(target.velocity.x * target.velocity.x +
                            target.velocity.z * target.velocity.z);
    if (speed < 50.0f) return 0.0f;
    return target.distance / travelSpeed;
}

// ---------------------------------------------------------------------------
// Aim warmth
// ---------------------------------------------------------------------------

// Aims "warm up" over a few seconds after a target swap; the warmth
// lowers confidence so the first corrections are small.

class AimWarmth {
public:
    static AimWarmth& instance() {
        static AimWarmth w;
        return w;
    }

    void noteSwap(int64_t nowMs) { lastSwapMs_ = nowMs; }

    float factor(int64_t nowMs) const {
        if (lastSwapMs_ == 0) return 1.0f;
        float age = static_cast<float>(nowMs - lastSwapMs_) / 3000.0f;
        return std::min(1.0f, age);
    }

    std::string diag() const {
        char buf[96];
        snprintf(buf, sizeof(buf), "warmth: %.2f\n", factor(utils::nowMs()));
        return std::string(buf);
    }

private:
    int64_t lastSwapMs_ = 0;
};

// ---------------------------------------------------------------------------
// Target acquisition range
// ---------------------------------------------------------------------------

// Acquisition range is wider than the aim range; the module only locks
// targets it can actually reach.

bool targetInAcquisitionRange(const TargetLock& t, float maxRange) {
    return t.locked && t.distance <= maxRange * 1.25f;
}

// ---------------------------------------------------------------------------
// Aim line length
// ---------------------------------------------------------------------------

float aimLineLength(const AimSolution& sol, float maxRange) {
    if (!sol.valid) return 0.0f;
    return std::min(maxRange, sol.leadSeconds * 1000.0f + 200.0f);
}

// ---------------------------------------------------------------------------
// Immediate vs predictive aim
// ---------------------------------------------------------------------------

// Returns true when the aim should fire immediately (target stationary
// or very close).

bool immediateAim(const TargetLock& t) {
    if (!t.locked) return false;
    float speed = std::sqrt(t.velocity.x * t.velocity.x +
                            t.velocity.z * t.velocity.z);
    return speed < 50.0f || t.distance < 200.0f;
}

// ---------------------------------------------------------------------------
// Aim engine summary
// ---------------------------------------------------------------------------

std::string aimEngineSummary() {
    std::string out = aimSuiteDiag();
    out += FocusPersistence::instance().diag();
    out += AimWarmth::instance().diag();
    return out;
}

// ---------------------------------------------------------------------------
// Skill priority order
// ---------------------------------------------------------------------------

// When several skills are ready, the picker uses this priority: ultimate
// first, then damage, then utility.

int skillPriority(int skillIndex) {
    switch (skillIndex) {
        case 0: return 1;   // ultimate
        case 1: return 2;   // first damage skill
        case 2: return 3;
        case 3: return 4;   // utility / escape
        default: return 5;
    }
}

// ---------------------------------------------------------------------------
// Cooldown awareness
// ---------------------------------------------------------------------------

// Skills on cooldown are excluded from the picker.

class SkillCooldowns {
public:
    static SkillCooldowns& instance() {
        static SkillCooldowns c;
        return c;
    }

    void markUsed(int skillIndex, int64_t nowMs, int cooldownMs) {
        readyAtMs_[skillIndex] = nowMs + cooldownMs;
    }

    bool ready(int skillIndex, int64_t nowMs) const {
        auto it = readyAtMs_.find(skillIndex);
        if (it == readyAtMs_.end()) return true;
        return nowMs >= it->second;
    }

    void reset() { readyAtMs_.clear(); }

    std::string diag() const {
        char buf[96];
        snprintf(buf, sizeof(buf), "skills: cooling=%d\n",
                 static_cast<int>(readyAtMs_.size()));
        return std::string(buf);
    }

private:
    std::map<int, int64_t> readyAtMs_;
};

// ---------------------------------------------------------------------------
// Best ready skill
// ---------------------------------------------------------------------------

int bestReadySkill(const SkillShotParams params[], int count,
                   const TargetLock& target, int64_t nowMs) {
    int best = -1;
    int bestPrio = 99;
    for (int i = 0; i < count; ++i) {
        if (!SkillCooldowns::instance().ready(i, nowMs)) continue;
        if (params[i].maxRange <= 0.0f) continue;
        if (target.distance > params[i].maxRange) continue;
        int prio = skillPriority(i);
        if (prio < bestPrio) {
            bestPrio = prio;
            best = i;
        }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Aim status line
// ---------------------------------------------------------------------------

std::string aimStatusLine() {
    AutoAim& m = AutoAim::instance();
    TargetLock lock = m.currentLock();
    AimSolution sol = m.currentSolution();
    char buf[192];
    snprintf(buf, sizeof(buf), "aim_status: lock=%d sol=%d conf=%.2f\n",
             lock.locked ? 1 : 0, sol.valid ? 1 : 0, sol.confidence);
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Full diag
// ---------------------------------------------------------------------------

std::string fullDiag() {
    std::string out = aimEngineSummary();
    out += aimStatusLine();
    out += SkillCooldowns::instance().diag();
    return out;
}

}  // namespace autoaim
}  // namespace arift