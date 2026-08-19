#include "physicaldamage.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "arift_log.h"
#include "arift_utils.h"
#include "feature_switch.h"

namespace arift {
namespace physicaldamage {

namespace {

float distXz(const Vec3& a, const Vec3& b) {
    float dx = a.x - b.x;
    float dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}

bool featureActive() {
    return FeatureSwitch::instance().isEnabled(kFeaturePhysicalDamage);
}

// Physical damage sensitivity: armor reduces physical damage taken.
float physicalSensitivity(const PlayerSnapshot& p) {
    return 100.0f / (100.0f + std::max(0.0f, p.armor));
}

}  // namespace

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------

void enable(bool on) {
    PhysicalDamage::instance().setEnabled(on);
    FeatureSwitch::instance().set(kFeaturePhysicalDamage, on);
}

bool enabled() {
    return PhysicalDamage::instance().running() && featureActive();
}

void setStyle(int style) {
    PhysicalDamage::instance().setStyle(style);
}

void setAutoCast(bool on) {
    PhysicalDamage::instance().setAutoCast(on);
}

// ---------------------------------------------------------------------------
// Target scoring
// ---------------------------------------------------------------------------

// Scores enemies by how much physical damage they will take: squishy
// targets (low armor) score highest.

DamageTarget scoreTarget(const PlayerSnapshot& t,
                         const PhysicalDamageConfig& cfg,
                         uint32_t preferId) {
    DamageTarget d;
    d.heroId = t.id;
    d.heroName = t.heroName;
    d.distance = distXz(t.position, t.position);
    (void)d.distance;
    d.hpRatio = t.healthRatio();
    d.armor = t.armor;
    d.physicalDamageTaken = physicalSensitivity(t);
    d.alive = t.alive;
    d.visible = !t.inFog;
    if (!t.alive || t.inFog) {
        d.score = -1.0f;
        return d;
    }
    float base = 100.0f * d.physicalDamageTaken;
    if (cfg.squishyPriority) {
        base += (1.0f - t.healthRatio()) * 30.0f;
    }
    if (preferId != 0 && t.id == preferId) base += 200.0f;
    d.score = base;
    return d;
}

DamageTarget bestDamageTarget(const std::vector<PlayerSnapshot>& players,
                              const PhysicalDamageConfig& cfg,
                              uint32_t preferId) {
    DamageTarget best;
    best.score = -1e9f;
    for (const auto& p : players) {
        if (p.team != EntityTeam::kEnemy) continue;
        DamageTarget s = scoreTarget(p, cfg, preferId);
        if (s.score > best.score) best = s;
    }
    return best;
}

// ---------------------------------------------------------------------------
// Skill readiness
// ---------------------------------------------------------------------------

bool skillReady(const DamageSkill& s, int64_t nowMs) {
    if (!s.ready) return false;
    return nowMs >= s.readyAtMs;
}

void markUsed(DamageSkill& s, int64_t nowMs) {
    s.readyAtMs = nowMs + static_cast<int64_t>(s.cooldownMs);
    s.ready = true;
}

// ---------------------------------------------------------------------------
// Rotation style logic
// ---------------------------------------------------------------------------

// Whether a skill should be cast for the current style + target state.

bool styleAllowsCast(RotationStyle style, const DamageSkill& s,
                     float targetHp, float targetDistance,
                     const PhysicalDamageConfig& cfg) {
    if (!s.ready) return false;
    if (targetDistance > s.range) return false;
    switch (style) {
        case RotationStyle::kBalanced:
            return !s.isUltimate || targetHp < 0.6f;
        case RotationStyle::kBurst:
            return targetHp < 0.75f || s.isUltimate;
        case RotationStyle::kPoke:
            return targetDistance <= cfg.pokeRange && !s.isUltimate;
        case RotationStyle::kAllIn:
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Burst window management
// ---------------------------------------------------------------------------

void updateBurst(BurstState& burst, int64_t nowMs) {
    if (burst.burstUntilMs != 0 && nowMs > burst.burstUntilMs) {
        burst.inBurst = false;
        burst.castsInBurst = 0;
        burst.damageInBurst = 0.0f;
    }
}

void openBurst(BurstState& burst, int64_t nowMs, int cooldownMs) {
    if (burst.inBurst) return;
    burst.inBurst = true;
    burst.burstUntilMs = nowMs + 8000;
    burst.castsInBurst = 0;
    burst.damageInBurst = 0.0f;
    (void)cooldownMs;
}

bool burstOpen(const BurstState& burst) {
    return burst.inBurst && burst.castsInBurst < burst.maxCasts;
}

// ---------------------------------------------------------------------------
// Rotation decision
// ---------------------------------------------------------------------------

RotationDecision decide(const std::vector<PlayerSnapshot>& players,
                        const PlayerSnapshot& self,
                        const std::vector<DamageSkill>& skills,
                        int64_t nowMs) {
    RotationDecision dec;
    if (!enabled()) return dec;
    PhysicalDamageConfig cfg = PhysicalDamage::instance().cfg();
    if (!cfg.autoCastSkills) return dec;
    DamageTarget target = bestDamageTarget(players, cfg, 0);
    if (target.heroId == 0 || target.score < 0.0f) return dec;
    for (const auto& t : players) {
        if (t.id == target.heroId) {
            target.distance = distXz(t.position, self.position);
            break;
        }
    }
    const DamageSkill* bestSkill = nullptr;
    float bestDamage = 0.0f;
    for (const auto& s : skills) {
        if (!skillReady(s, nowMs)) continue;
        if (!styleAllowsCast(cfg.style, s, target.hpRatio,
                             target.distance, cfg)) {
            continue;
        }
        float effective = s.damage * s.physicalRatio * target.physicalDamageTaken;
        if (s.isUltimate) effective *= 1.3f;
        if (effective > bestDamage) {
            bestDamage = effective;
            bestSkill = &s;
        }
    }
    if (!bestSkill) return dec;
    dec.cast = true;
    dec.skillIndex = bestSkill->index;
    dec.targetId = target.heroId;
    dec.targetName = target.heroName;
    dec.predictedDamage = bestDamage;
    dec.reason = bestSkill->isUltimate ? "ultimate window" : "rotation";
    dec.usesCrit = cfg.critAssist && target.hpRatio < 0.5f;
    return dec;
}

// ---------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------

void executeRotation(const RotationDecision& d, int64_t nowMs) {
    if (!d.cast) return;
    char buf[192];
    snprintf(buf, sizeof(buf), "damage: skill=%d -> %s (%.0f dmg) %s",
             d.skillIndex, d.targetName.c_str(), d.predictedDamage,
             d.usesCrit ? "+crit" : "");
    ARIFT_DEBUG(kTagPhysicalDamage, "%s", buf);
}

// ---------------------------------------------------------------------------
// Diag
// ---------------------------------------------------------------------------

std::string physicalDamageDiag() {
    PhysicalDamage& m = PhysicalDamage::instance();
    RotationDecision d = m.lastDecision();
    BurstState b = m.burstState();
    char buf[256];
    snprintf(buf, sizeof(buf),
             "physdamage: %s style=%d burst=%d casts=%d/%d last=%s\n",
             m.running() ? "ON" : "OFF", static_cast<int>(m.cfg().style),
             b.inBurst ? 1 : 0, b.castsInBurst, b.maxCasts,
             d.cast ? d.reason : "idle");
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// PhysicalDamage implementation
// ---------------------------------------------------------------------------

PhysicalDamage& PhysicalDamage::instance() {
    static PhysicalDamage p;
    return p;
}

int PhysicalDamage::start() {
    if (running_.load()) return 0;
    running_.store(true);
    if (!thread_.start([this] { loop(); })) {
        running_.store(false);
        return -1;
    }
    ARIFT_INFO(kTagPhysicalDamage, "Physical Damage started");
    return 0;
}

int PhysicalDamage::stop() {
    if (!running_.load()) return 0;
    running_.store(false);
    thread_.join();
    ARIFT_INFO(kTagPhysicalDamage, "Physical Damage stopped");
    return 0;
}

void PhysicalDamage::setEnabled(bool v) {
    cfg_.enabled = v;
    if (v) {
        start();
    } else {
        stop();
    }
}

void PhysicalDamage::setStyle(int s) {
    cfg_.style = static_cast<RotationStyle>(std::max(0, std::min(3, s)));
}

void PhysicalDamage::setAutoCast(bool v) {
    cfg_.autoCastSkills = v;
}

void PhysicalDamage::feed(const std::vector<PlayerSnapshot>& players,
                          const PlayerSnapshot& self,
                          const std::vector<DamageSkill>& skills,
                          int64_t nowMs) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    players_ = players;
    self_ = self;
    skills_ = skills;
    nowMs_ = nowMs;
}

RotationDecision PhysicalDamage::lastDecision() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return lastDecision_;
}

BurstState PhysicalDamage::burstState() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return burst_;
}

BurstState& PhysicalDamage::burstStateRef() {
    return burst_;
}

void PhysicalDamage::loop() {
    int64_t last = 0;
    while (running_.load()) {
        int64_t now = utils::monotonicMs();
        if (now - last >= 100) {
            last = now;
            std::vector<PlayerSnapshot> players;
            PlayerSnapshot self;
            std::vector<DamageSkill> skills;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                players = players_;
                self = self_;
                skills = skills_;
            }
            updateBurst(burst_, now);
            RotationDecision d = decide(players, self, skills, now);
            if (d.cast) {
                executeRotation(d, now);
                if (burst_.inBurst) {
                    burst_.castsInBurst++;
                    burst_.damageInBurst += d.predictedDamage;
                }
                for (auto& s : skills) {
                    if (s.index == d.skillIndex) markUsed(s, now);
                }
            }
            std::lock_guard<std::mutex> lock2(state_mutex_);
            lastDecision_ = d;
        }
        Thread::sleepMs(20);
    }
}

const PhysicalDamageConfig& PhysicalDamage::cfg() const {
    return cfg_;
}

std::string PhysicalDamage::diag() const {
    return physicalDamageDiag();
}

// ---------------------------------------------------------------------------
// Attack speed pacing
// ---------------------------------------------------------------------------

// Basic attacks pace around the attack speed; the pacer spaces auto
// attacks at the correct interval.

class AttackPacer {
public:
    static AttackPacer& instance() {
        static AttackPacer p;
        return p;
    }

    // Interval between basic attacks for a given attack speed (per s).
    int intervalMs(float attackSpeed) {
        if (attackSpeed <= 0.0f) return 1000;
        return static_cast<int>(1000.0f / std::min(5.0f, attackSpeed));
    }

    bool attackDue(int64_t nowMs) const { return nowMs >= nextAtMs_; }

    void schedule(int64_t nowMs, float attackSpeed) {
        nextAtMs_ = nowMs + intervalMs(attackSpeed);
    }

    std::string diag() const {
        char buf[96];
        snprintf(buf, sizeof(buf), "pacer: next=%lld\n",
                 static_cast<long long>(nextAtMs_));
        return std::string(buf);
    }

private:
    int64_t nextAtMs_ = 0;
};

// ---------------------------------------------------------------------------
// Crit assist
// ---------------------------------------------------------------------------

// When crit chance is high and the target is low, the assist commits to
// the target (no switching) to guarantee the crit lands.

class CritAssist {
public:
    static CritAssist& instance() {
        static CritAssist c;
        return c;
    }

    void commit(uint32_t heroId, int64_t nowMs, int commitMs) {
        committedId_ = heroId;
        untilMs_ = nowMs + commitMs;
    }

    uint32_t committed(int64_t nowMs) const {
        if (untilMs_ == 0) return 0;
        if (nowMs > untilMs_) return 0;
        return committedId_;
    }

    void clear() {
        committedId_ = 0;
        untilMs_ = 0;
    }

    std::string diag() const {
        char buf[96];
        snprintf(buf, sizeof(buf), "crit: committed=%u\n", committedId_);
        return std::string(buf);
    }

private:
    uint32_t committedId_ = 0;
    int64_t untilMs_ = 0;
};

// ---------------------------------------------------------------------------
// Damage overkill guard
// ---------------------------------------------------------------------------

// Casting a huge skill on a nearly-dead target wastes it; the guard
// switches to the cheapest finishing move.

bool overkill(const DamageSkill& s, float targetHp,
              float maxTargetHp) {
    if (maxTargetHp <= 0.0f) return false;
    return s.damage > targetHp * 1.5f && s.damage > maxTargetHp * 0.35f;
}

// ---------------------------------------------------------------------------
// Kill-secure logic
// ---------------------------------------------------------------------------

// When a target is low, prefer the skill that deals enough damage to
// finish without waste.

const DamageSkill* killSecureSkill(const std::vector<DamageSkill>& skills,
                                   float targetHp, int64_t nowMs) {
    const DamageSkill* best = nullptr;
    float bestWaste = 1e9f;
    for (const auto& s : skills) {
        if (!skillReady(s, nowMs)) continue;
        if (s.damage < targetHp) continue;
        float waste = s.damage - targetHp;
        if (waste < bestWaste) {
            bestWaste = waste;
            best = &s;
        }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Damage stat tracker
// ---------------------------------------------------------------------------

class DamageStats {
public:
    static DamageStats& instance() {
        static DamageStats s;
        return s;
    }

    void note(float damage) {
        total_ += damage;
        casts_++;
        if (damage > best_ ) best_ = damage;
    }

    float total() const { return total_; }
    int casts() const { return casts_; }
    float best() const { return best_; }

    void reset() {
        total_ = 0.0f;
        casts_ = 0;
        best_ = 0.0f;
    }

    std::string diag() const {
        char buf[160];
        snprintf(buf, sizeof(buf), "dmg_stats: total=%.0f casts=%d best=%.0f\n",
                 total(), casts(), best());
        return std::string(buf);
    }

private:
    float total_ = 0.0f;
    int casts_ = 0;
    float best_ = 0.0f;
};

// ---------------------------------------------------------------------------
// Engagement check
// ---------------------------------------------------------------------------

// Skills only cast when the fight is engaged (enemy in engage range or
// burst open).

bool engaged(const PlayerSnapshot& self,
             const std::vector<PlayerSnapshot>& players,
             const PhysicalDamageConfig& cfg) {
    for (const auto& p : players) {
        if (p.team != EntityTeam::kEnemy) continue;
        if (!p.alive || p.inFog) continue;
        float d = distXz(p.position, self.position);
        if (d <= cfg.engageRange) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Burst opener
// ---------------------------------------------------------------------------

// Opens a burst window when the conditions line up: target low and
// ultimate ready.

void maybeOpenBurst(BurstState& burst, const DamageTarget& target,
                    const std::vector<DamageSkill>& skills, int64_t nowMs) {
    if (burst.inBurst) return;
    if (target.hpRatio > 0.6f) return;
    for (const auto& s : skills) {
        if (s.isUltimate && skillReady(s, nowMs)) {
            openBurst(burst, nowMs, 0);
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Focused damage driver
// ---------------------------------------------------------------------------

// Full driver: engages, opens bursts, then picks the best skill with
// kill-secure overrides.

RotationDecision focusedDecision(const std::vector<PlayerSnapshot>& players,
                                 const PlayerSnapshot& self,
                                 const std::vector<DamageSkill>& skills,
                                 int64_t nowMs) {
    RotationDecision dec;
    PhysicalDamageConfig cfg = PhysicalDamage::instance().cfg();
    if (!engaged(self, players, cfg)) return dec;
    DamageTarget target = bestDamageTarget(players, cfg, 0);
    if (target.heroId == 0 || target.score < 0.0f) return dec;
    for (const auto& t : players) {
        if (t.id == target.heroId) {
            target.distance = distXz(t.position, self.position);
            break;
        }
    }
    maybeOpenBurst(PhysicalDamage::instance().burstStateRef(), target, skills,
                   nowMs);
    if (target.hpRatio < 0.3f) {
        const DamageSkill* finisher = killSecureSkill(skills, target.hpRatio,
                                                      nowMs);
        if (finisher) {
            dec.cast = true;
            dec.skillIndex = finisher->index;
            dec.targetId = target.heroId;
            dec.targetName = target.heroName;
            dec.predictedDamage = finisher->damage;
            dec.reason = "kill secure";
            return dec;
        }
    }
    return decide(players, self, skills, nowMs);
}

// ---------------------------------------------------------------------------
// Damage report line
// ---------------------------------------------------------------------------

std::string damageReportLine() {
    char buf[128];
    snprintf(buf, sizeof(buf), "damage_report: style=%d mult=%.2f\n",
             static_cast<int>(PhysicalDamage::instance().cfg().style),
             PhysicalDamage::instance().cfg().damageMultiplier);
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Full damage suite diag
// ---------------------------------------------------------------------------

std::string damageSuiteDiag() {
    std::string out = physicalDamageDiag();
    out += DamageStats::instance().diag();
    out += AttackPacer::instance().diag();
    out += CritAssist::instance().diag();
    return out;
}

// ---------------------------------------------------------------------------
// Damage rotation plan
// ---------------------------------------------------------------------------

// Pre-computes the ideal rotation for a target: the sequence of skills
// that maximizes damage inside the range and cooldown constraints.

struct RotationPlan {
    std::vector<int> skillOrder;
    float totalDamage = 0.0f;
    float totalTimeMs = 0.0f;
    uint32_t targetId = 0;
};

RotationPlan planRotation(const std::vector<DamageSkill>& skills,
                          const DamageTarget& target) {
    RotationPlan plan;
    plan.targetId = target.heroId;
    std::vector<DamageSkill> pool = skills;
    std::sort(pool.begin(), pool.end(),
              [](const DamageSkill& a, const DamageSkill& b) {
                  float da = a.damage / std::max(1.0f, a.cooldownMs);
                  float db = b.damage / std::max(1.0f, b.cooldownMs);
                  return da > db;
              });
    float eff = 1.0f;
    for (const auto& s : pool) {
        if (s.range < target.distance) continue;
        plan.skillOrder.push_back(s.index);
        plan.totalDamage += s.damage * eff;
        plan.totalTimeMs += s.cooldownMs * 0.3f;
        eff *= 0.85f;
    }
    return plan;
}

// ---------------------------------------------------------------------------
// Execute planned rotation
// ---------------------------------------------------------------------------

// Casts the next skill in the plan (first not on cooldown).

int nextPlannedSkill(const RotationPlan& plan,
                     const std::vector<DamageSkill>& skills, int64_t nowMs) {
    for (int idx : plan.skillOrder) {
        for (const auto& s : skills) {
            if (s.index == idx && skillReady(s, nowMs)) return idx;
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Target switching guard
// ---------------------------------------------------------------------------

// Switching targets wastes burst; the guard keeps the current target
// while it remains valuable.

class TargetCommit {
public:
    static TargetCommit& instance() {
        static TargetCommit c;
        return c;
    }

    void commit(uint32_t heroId, int64_t nowMs) {
        heroId_ = heroId;
        sinceMs_ = nowMs;
    }

    bool holding(uint32_t candidate, int64_t nowMs) const {
        if (heroId_ == 0) return false;
        if (nowMs - sinceMs_ > 5000) return false;
        return candidate == heroId_;
    }

    void clear() { heroId_ = 0; }

private:
    uint32_t heroId_ = 0;
    int64_t sinceMs_ = 0;
};

// ---------------------------------------------------------------------------
// Damage multiplier model
// ---------------------------------------------------------------------------

// The damage model applies a local multiplier curve: low HP targets take
// bonus damage, high-armor targets take less.

float effectiveDamage(const DamageSkill& s, const DamageTarget& target) {
    float mult = target.physicalDamageTaken;
    if (target.hpRatio < 0.3f) mult *= 1.2f;
    if (target.hpRatio > 0.85f) mult *= 0.9f;
    return s.damage * s.physicalRatio * mult;
}

// ---------------------------------------------------------------------------
// Best skill picker (effective)
// ---------------------------------------------------------------------------

const DamageSkill* bestEffectiveSkill(const std::vector<DamageSkill>& skills,
                                      const DamageTarget& target,
                                      int64_t nowMs) {
    const DamageSkill* best = nullptr;
    float bestDmg = 0.0f;
    for (const auto& s : skills) {
        if (!skillReady(s, nowMs)) continue;
        if (s.range < target.distance) continue;
        float dmg = effectiveDamage(s, target);
        if (dmg > bestDmg) {
            bestDmg = dmg;
            best = &s;
        }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Commit-aware decision
// ---------------------------------------------------------------------------

RotationDecision commitAwareDecision(const std::vector<PlayerSnapshot>& players,
                                     const PlayerSnapshot& self,
                                     const std::vector<DamageSkill>& skills,
                                     int64_t nowMs) {
    RotationDecision dec;
    if (!enabled()) return dec;
    PhysicalDamageConfig cfg = PhysicalDamage::instance().cfg();
    DamageTarget target = bestDamageTarget(players, cfg, 0);
    if (target.heroId == 0) return dec;
    uint32_t held = TargetCommit::instance().holding(target.heroId, nowMs)
                        ? target.heroId
                        : 0;
    (void)held;
    TargetCommit::instance().commit(target.heroId, nowMs);
    for (const auto& t : players) {
        if (t.id == target.heroId) {
            target.distance = distXz(t.position, self.position);
            break;
        }
    }
    const DamageSkill* skill = bestEffectiveSkill(skills, target, nowMs);
    if (!skill) return dec;
    dec.cast = true;
    dec.skillIndex = skill->index;
    dec.targetId = target.heroId;
    dec.targetName = target.heroName;
    dec.predictedDamage = effectiveDamage(*skill, target);
    dec.reason = "effective rotation";
    return dec;
}

// ---------------------------------------------------------------------------
// Damage window tracker
// ---------------------------------------------------------------------------

// Tracks how much damage was dealt in the current minute (window).

class DamageWindow {
public:
    static DamageWindow& instance() {
        static DamageWindow w;
        return w;
    }

    void note(float damage, int64_t nowMs) {
        if (nowMs - windowMs_ >= 60000) {
            windowMs_ = nowMs;
            windowDamage_ = 0.0f;
        }
        windowDamage_ += damage;
    }

    float windowDamage() const { return windowDamage_; }

    std::string diag() const {
        char buf[96];
        snprintf(buf, sizeof(buf), "dmg_window: %.0f/min\n", windowDamage());
        return std::string(buf);
    }

private:
    int64_t windowMs_ = 0;
    float windowDamage_ = 0.0f;
};

// ---------------------------------------------------------------------------
// Physical damage summary line
// ---------------------------------------------------------------------------

std::string physicalSummaryLine() {
    PhysicalDamage& m = PhysicalDamage::instance();
    char buf[192];
    snprintf(buf, sizeof(buf), "phys: style=%d burst=%d casts=%d dmg=%.0f\n",
             static_cast<int>(m.cfg().style),
             m.burstState().inBurst ? 1 : 0,
             DamageStats::instance().casts(),
             DamageStats::instance().total());
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Complete damage diag
// ---------------------------------------------------------------------------

std::string completeDamageDiag() {
    std::string out = damageSuiteDiag();
    out += DamageWindow::instance().diag();
    out += physicalSummaryLine();
    return out;
}

// ---------------------------------------------------------------------------
// Attack sequence generator
// ---------------------------------------------------------------------------

// Generates the attack sequence for a target: interleaves basic attacks
// with skills at the right pace.

struct AttackSequence {
    std::vector<std::string> steps;
    float totalTimeMs = 0.0f;
};

AttackSequence generateSequence(const DamageTarget& target,
                                const std::vector<DamageSkill>& skills,
                                float attackSpeed) {
    AttackSequence seq;
    RotationPlan plan = planRotation(skills, target);
    int basicEvery = std::max(1, 700 / std::max(1, AttackPacer::instance().intervalMs(attackSpeed)));
    int step = 0;
    for (int idx : plan.skillOrder) {
        if (step % basicEvery == 0) {
            seq.steps.push_back("basic");
        }
        seq.steps.push_back("skill" + std::to_string(idx));
        seq.totalTimeMs += std::max(100.0f, skills[idx].cooldownMs * 0.3f);
        step++;
    }
    return seq;
}

// ---------------------------------------------------------------------------
// Rotation pacing check
// ---------------------------------------------------------------------------

// Keeps the rotation inside human-scale pacing.

bool rotationPaced(const AttackSequence& seq) {
    return seq.totalTimeMs >= 300.0f;
}

// ---------------------------------------------------------------------------
// Passive proc tracker
// ---------------------------------------------------------------------------

// Passive damage procs (e.g. crit passives) are tracked so the rotation
// can time around them.

class PassiveTracker {
public:
    static PassiveTracker& instance() {
        static PassiveTracker p;
        return p;
    }

    void noteProc(float damage, int64_t nowMs) {
        procs_++;
        lastProcMs_ = nowMs;
        lastDamage_ = damage;
    }

    int procs() const { return procs_; }
    float lastDamage() const { return lastDamage_; }

    // Passive ready again after its internal cooldown.
    bool procReady(int64_t nowMs, int cooldownMs) const {
        return lastProcMs_ == 0 || nowMs - lastProcMs_ >= cooldownMs;
    }

    std::string diag() const {
        char buf[96];
        snprintf(buf, sizeof(buf), "passive: procs=%d\n", procs());
        return std::string(buf);
    }

private:
    int procs_ = 0;
    int64_t lastProcMs_ = 0;
    float lastDamage_ = 0.0f;
};

// ---------------------------------------------------------------------------
// Burst phase detector
// ---------------------------------------------------------------------------

// Detects the burst phase: enemy team low on HP across the board.

bool burstPhaseDetected(const std::vector<PlayerSnapshot>& players) {
    int enemies = 0;
    float totalHp = 0.0f;
    for (const auto& p : players) {
        if (p.team != EntityTeam::kEnemy) continue;
        if (!p.alive || p.inFog) continue;
        enemies++;
        totalHp += p.healthRatio();
    }
    if (enemies == 0) return false;
    return totalHp / enemies < 0.45f;
}

// ---------------------------------------------------------------------------
// Style shift on burst phase
// ---------------------------------------------------------------------------

RotationStyle styleForPhase(const PhysicalDamageConfig& cfg,
                            const std::vector<PlayerSnapshot>& players) {
    if (burstPhaseDetected(players)) return RotationStyle::kAllIn;
    return cfg.style;
}

// ---------------------------------------------------------------------------
// Cast counter
// ---------------------------------------------------------------------------

// Caps casts per second to avoid machine-like cadence.

class CastCounter {
public:
    static CastCounter& instance() {
        static CastCounter c;
        return c;
    }

    bool allowed(int64_t nowMs, int capPerSec) {
        if (nowMs - windowMs_ >= 1000) {
            windowMs_ = nowMs;
            casts_ = 0;
        }
        if (casts_ >= capPerSec) return false;
        casts_++;
        return true;
    }

    std::string diag() const {
        char buf[96];
        snprintf(buf, sizeof(buf), "casts: %d/s\n", casts_);
        return std::string(buf);
    }

private:
    int64_t windowMs_ = 0;
    int casts_ = 0;
};

// ---------------------------------------------------------------------------
// Phase-aware driver
// ---------------------------------------------------------------------------

RotationDecision phaseAwareDecision(const std::vector<PlayerSnapshot>& players,
                                    const PlayerSnapshot& self,
                                    const std::vector<DamageSkill>& skills,
                                    int64_t nowMs) {
    if (!enabled()) return RotationDecision();
    if (!CastCounter::instance().allowed(nowMs, 3)) return RotationDecision();
    PhysicalDamageConfig cfg = PhysicalDamage::instance().cfg();
    RotationStyle activeStyle = styleForPhase(cfg, players);
    DamageTarget target = bestDamageTarget(players, cfg, 0);
    if (target.heroId == 0) return RotationDecision();
    for (const auto& t : players) {
        if (t.id == target.heroId) {
            target.distance = distXz(t.position, self.position);
            break;
        }
    }
    const DamageSkill* skill = bestEffectiveSkill(skills, target, nowMs);
    if (!skill) return RotationDecision();
    RotationDecision dec;
    dec.cast = true;
    dec.skillIndex = skill->index;
    dec.targetId = target.heroId;
    dec.targetName = target.heroName;
    dec.predictedDamage = effectiveDamage(*skill, target);
    dec.reason = activeStyle == RotationStyle::kAllIn ? "all-in phase"
                                                      : "rotation";
    return dec;
}

// ---------------------------------------------------------------------------
// Final damage suite seal
// ---------------------------------------------------------------------------

std::string damageSuiteSeal() {
    std::string out = completeDamageDiag();
    out += PassiveTracker::instance().diag();
    out += CastCounter::instance().diag();
    return out;
}

// ---------------------------------------------------------------------------
// Damage floor guard
// ---------------------------------------------------------------------------

// Trivial damage casts waste mana; the guard refuses casts below a
// minimum effectiveness.

bool damageFloor(const RotationDecision& d, float minDamage) {
    if (!d.cast) return false;
    return d.predictedDamage >= minDamage;
}

// ---------------------------------------------------------------------------
// Rotation order stability
// ---------------------------------------------------------------------------

// The rotation order should not flip between frames; the stabilizer
// remembers the last order and only changes it when the target changes.

class OrderStabilizer {
public:
    static OrderStabilizer& instance() {
        static OrderStabilizer o;
        return o;
    }

    void note(uint32_t targetId, int skillIndex) {
        if (targetId != lastTarget_) {
            lastTarget_ = targetId;
            order_ = skillIndex;
        }
    }

    bool stable(uint32_t targetId, int skillIndex) const {
        if (targetId != lastTarget_) return true;
        return order_ == skillIndex || order_ == 0;
    }

private:
    uint32_t lastTarget_ = 0;
    int order_ = 0;
};

// ---------------------------------------------------------------------------
// Damage phase line
// ---------------------------------------------------------------------------

std::string damagePhaseLine(const std::vector<PlayerSnapshot>& players) {
    char buf[128];
    snprintf(buf, sizeof(buf), "phase: burst=%d\n",
             burstPhaseDetected(players) ? 1 : 0);
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Module seal
// ---------------------------------------------------------------------------

std::string moduleSeal(const std::vector<PlayerSnapshot>& players) {
    std::string out = damageSuiteSeal();
    out += damagePhaseLine(players);
    return out;
}

}  // namespace physicaldamage
}  // namespace arift