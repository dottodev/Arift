#include "tankdefense.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

#include "arift_log.h"
#include "arift_utils.h"
#include "feature_switch.h"

namespace arift {
namespace tankdefense {

namespace {

float distXz(const Vec3& a, const Vec3& b) {
    float dx = a.x - b.x;
    float dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}

bool featureActive() {
    return FeatureSwitch::instance().isEnabled(kFeatureTankDefense);
}

// CC escape skills: skills that remove crowd control.
bool isEscapeSkill(int skillIndex) {
    return skillIndex == 3 || skillIndex == 4;
}

// Shield skills: skills that grant a shield or heal.
bool isShieldSkill(int skillIndex) {
    return skillIndex == 1 || skillIndex == 2;
}

}  // namespace

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------

void enable(bool on) {
    TankDefense::instance().setEnabled(on);
    FeatureSwitch::instance().set(kFeatureTankDefense, on);
}

bool enabled() {
    return TankDefense::instance().running() && featureActive();
}

void setThreshold(float hp) {
    TankDefense::instance().setThreshold(hp);
}

void setAutoItems(bool on) {
    TankDefense::instance().setAutoItems(on);
}

// ---------------------------------------------------------------------------
// Fight context builder
// ---------------------------------------------------------------------------

FightContext buildFightContext(const PlayerSnapshot& self,
                               const std::vector<PlayerSnapshot>& players) {
    FightContext ctx;
    for (const auto& p : players) {
        if (!p.alive || p.inFog) continue;
        float d = distXz(p.position, self.position);
        if (p.team == EntityTeam::kEnemy) {
            if (d < 900.0f) {
                ctx.enemiesNear++;
                ctx.nearestEnemyDist = std::min(ctx.nearestEnemyDist, d);
            }
        } else if (p.team != EntityTeam::kUnknown) {
            if (d < 900.0f) {
                ctx.alliesNear++;
                ctx.avgAllyHp += p.healthRatio();
            }
        }
    }
    if (ctx.alliesNear > 0) {
        ctx.avgAllyHp /= (ctx.alliesNear + 1);
    }
    ctx.inFight = ctx.enemiesNear > 0;
    return ctx;
}

// ---------------------------------------------------------------------------
// Item readiness
// ---------------------------------------------------------------------------

bool itemReady(const DefensiveItem& item, int64_t nowMs) {
    if (!item.active) return false;
    if (item.slot < 0) return false;
    return nowMs - item.usedAtMs >= item.cooldownMs;
}

const DefensiveItem* bestItem(const std::vector<DefensiveItem>& items,
                              int64_t nowMs) {
    const DefensiveItem* best = nullptr;
    float bestValue = 0.0f;
    for (const auto& it : items) {
        if (!itemReady(it, nowMs)) continue;
        if (it.value > bestValue) {
            bestValue = it.value;
            best = &it;
        }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Trigger evaluator
// ---------------------------------------------------------------------------

// Low-HP trigger with aggression bias.

TriggerEval lowHpTrigger(const TankDefenseConfig& cfg,
                         const PlayerSnapshot& self,
                         const std::vector<DefensiveItem>& items,
                         int64_t nowMs) {
    TriggerEval ev;
    float hp = self.healthRatio();
    float effectiveThreshold =
        cfg.lowHpThreshold * (0.8f + cfg.aggressionBias / 250.0f);
    if (hp > effectiveThreshold) return ev;
    ev.triggered = true;
    ev.kind = TriggerKind::kLowHp;
    ev.score = (effectiveThreshold - hp) * 100.0f;
    const DefensiveItem* it = bestItem(items, nowMs);
    if (it && cfg.autoItems) {
        ev.action = DefenseAction::kActivateItem;
        ev.itemSlot = it->slot;
        ev.reason = "low HP item";
        return ev;
    }
    if (cfg.autoShields) {
        ev.action = DefenseAction::kCastShield;
        ev.skillIndex = 1;
        ev.reason = "low HP shield";
        return ev;
    }
    if (hp <= cfg.criticalHp && cfg.autoEscape) {
        ev.action = DefenseAction::kCastEscape;
        ev.skillIndex = 4;
        ev.reason = "critical HP escape";
        return ev;
    }
    ev.action = DefenseAction::kGuard;
    ev.reason = "low HP guard";
    return ev;
}

// CC-escape trigger (enemy burst detected via nearby enemies + own HP dip).

TriggerEval ccEscapeTrigger(const TankDefenseConfig& cfg,
                            const PlayerSnapshot& self,
                            const FightContext& ctx, int64_t nowMs) {
    TriggerEval ev;
    if (!cfg.autoEscape) return ev;
    if (self.healthRatio() > 0.5f) return ev;
    if (ctx.enemiesNear < 2) return ev;
    if (ctx.nearestEnemyDist > 400.0f) return ev;
    ev.triggered = true;
    ev.kind = TriggerKind::kCcEscaped;
    ev.action = DefenseAction::kCastEscape;
    ev.skillIndex = 4;
    ev.score = 80.0f + (0.5f - self.healthRatio()) * 60.0f;
    ev.reason = "cc escape";
    return ev;
}

// Team-fight trigger: shield allies when a fight starts.

TriggerEval teamFightTrigger(const TankDefenseConfig& cfg,
                             const PlayerSnapshot& self,
                             const FightContext& ctx, int64_t nowMs) {
    TriggerEval ev;
    if (!cfg.autoShields) return ev;
    if (!ctx.inFight) return ev;
    if (ctx.avgAllyHp > 0.7f) return ev;
    if (self.healthRatio() < 0.3f) return ev;
    ev.triggered = true;
    ev.kind = TriggerKind::kTeamFight;
    ev.action = DefenseAction::kCastShield;
    ev.skillIndex = 2;
    ev.score = 60.0f + (0.7f - ctx.avgAllyHp) * 50.0f;
    ev.reason = "team fight shield";
    return ev;
}

// Tower-guard trigger: stay defensive near a tower.

TriggerEval towerTrigger(const TankDefenseConfig& cfg,
                         const PlayerSnapshot& self,
                         const FightContext& ctx, int64_t nowMs) {
    TriggerEval ev;
    if (!cfg.towerGuard) return ev;
    if (!ctx.underTower) return ev;
    if (self.healthRatio() > 0.45f) return ev;
    ev.triggered = true;
    ev.kind = TriggerKind::kUnderTower;
    ev.action = DefenseAction::kGuard;
    ev.score = 50.0f;
    ev.reason = "tower guard";
    return ev;
}

// Timer trigger: periodic guard refresh.

TriggerEval timerTrigger(const TankDefenseConfig& cfg,
                         const PlayerSnapshot& self, int64_t nowMs) {
    TriggerEval ev;
    if (nowMs % cfg.guardCooldownMs > 500) return ev;
    if (self.healthRatio() > 0.8f) return ev;
    ev.triggered = true;
    ev.kind = TriggerKind::kTimer;
    ev.action = DefenseAction::kGuard;
    ev.score = 20.0f;
    ev.reason = "periodic guard";
    return ev;
}

// ---------------------------------------------------------------------------
// Full evaluation
// ---------------------------------------------------------------------------

TriggerEval evaluate(const PlayerSnapshot& self,
                     const std::vector<PlayerSnapshot>& players,
                     const std::vector<DefensiveItem>& items,
                     int64_t nowMs) {
    TriggerEval best;
    if (!enabled()) return best;
    TankDefenseConfig cfg = TankDefense::instance().cfg();
    FightContext ctx = buildFightContext(self, players);
    std::vector<TriggerEval> evals;
    evals.push_back(lowHpTrigger(cfg, self, items, nowMs));
    evals.push_back(ccEscapeTrigger(cfg, self, ctx, nowMs));
    evals.push_back(teamFightTrigger(cfg, self, ctx, nowMs));
    evals.push_back(towerTrigger(cfg, self, ctx, nowMs));
    evals.push_back(timerTrigger(cfg, self, nowMs));
    for (const auto& e : evals) {
        if (e.triggered && e.score > best.score) best = e;
    }
    return best;
}

// ---------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------

void execute(const TriggerEval& eval, int64_t nowMs) {
    if (!eval.triggered) return;
    char buf[192];
    snprintf(buf, sizeof(buf), "defense: %s (item=%d skill=%d)",
             eval.reason.c_str(), eval.itemSlot, eval.skillIndex);
    ARIFT_DEBUG(kTagTankDefense, "%s", buf);
}

// ---------------------------------------------------------------------------
// Diag
// ---------------------------------------------------------------------------

std::string tankDefenseDiag() {
    TankDefense& m = TankDefense::instance();
    TriggerEval ev = m.lastEval();
    char buf[256];
    snprintf(buf, sizeof(buf),
             "tankdef: %s hp=%.0f%% thr=%.0f%% trigger=%s(%d)\n",
             m.running() ? "ON" : "OFF",
             m.cfg().lowHpThreshold * 100.0f, ev.score,
             ev.triggered ? ev.reason.c_str() : "none",
             static_cast<int>(ev.kind));
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// TankDefense implementation
// ---------------------------------------------------------------------------

TankDefense& TankDefense::instance() {
    static TankDefense t;
    return t;
}

int TankDefense::start() {
    if (running_.load()) return 0;
    running_.store(true);
    if (!thread_.start([this] { loop(); })) {
        running_.store(false);
        return -1;
    }
    ARIFT_INFO(kTagTankDefense, "Tank Defense started");
    return 0;
}

int TankDefense::stop() {
    if (!running_.load()) return 0;
    running_.store(false);
    thread_.join();
    ARIFT_INFO(kTagTankDefense, "Tank Defense stopped");
    return 0;
}

void TankDefense::setEnabled(bool v) {
    cfg_.enabled = v;
    if (v) {
        start();
    } else {
        stop();
    }
}

void TankDefense::setThreshold(float hp) {
    cfg_.lowHpThreshold = std::max(0.10f, std::min(0.80f, hp));
}

void TankDefense::setAutoItems(bool v) {
    cfg_.autoItems = v;
}

void TankDefense::feed(const PlayerSnapshot& self,
                       const std::vector<PlayerSnapshot>& players,
                       const std::vector<DefensiveItem>& items,
                       int64_t nowMs) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    self_ = self;
    players_ = players;
    items_ = items;
    nowMs_ = nowMs;
}

TriggerEval TankDefense::lastEval() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return lastEval_;
}

void TankDefense::loop() {
    int64_t last = 0;
    while (running_.load()) {
        int64_t now = utils::monotonicMs();
        if (now - last >= 100) {
            last = now;
            PlayerSnapshot self;
            std::vector<PlayerSnapshot> players;
            std::vector<DefensiveItem> items;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                self = self_;
                players = players_;
                items = items_;
            }
            TriggerEval ev = evaluate(self, players, items, now);
            if (ev.triggered) {
                execute(ev, now);
            }
            std::lock_guard<std::mutex> lock2(state_mutex_);
            lastEval_ = ev;
        }
        Thread::sleepMs(20);
    }
}

const TankDefenseConfig& TankDefense::cfg() const {
    return cfg_;
}

const PlayerSnapshot& TankDefense::selfRef() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return self_;
}

std::string TankDefense::diag() const {
    return tankDefenseDiag();
}

// ---------------------------------------------------------------------------
// Defense item database
// ---------------------------------------------------------------------------

// Static knowledge of the defensive actives: what each item does, its
// cooldown and its value under different conditions.

struct DefenseItemInfo {
    const char* name;
    int cooldownMs;
    float value;            // generic value score
    float lowHpBonus;       // extra value when HP is low
    bool ccCleanse;         // removes crowd control
};

static const DefenseItemInfo kDefenseItems[] = {
    {"Aegis", 12000, 6.0f, 4.0f, false},
    {"Winter Truncheon", 18000, 7.0f, 5.0f, true},
    {"Vengeance", 10000, 5.0f, 6.0f, false},
    {"Immortality", 30000, 9.0f, 9.0f, false},
    {"Antique Cuirass", 15000, 4.0f, 2.0f, false},
    {"Dominance Ice", 12000, 4.5f, 1.5f, false},
};

const DefenseItemInfo* defenseItemInfo(const std::string& name) {
    for (const auto& it : kDefenseItems) {
        if (name == it.name) return &it;
    }
    return nullptr;
}

// Value of an item given the current situation.
float situationalItemValue(const DefensiveItem& item,
                           const PlayerSnapshot& self,
                           const FightContext& ctx) {
    const DefenseItemInfo* info = defenseItemInfo(item.name);
    float v = info ? info->value : item.value;
    if (info && self.healthRatio() < 0.35f) v += info->lowHpBonus;
    if (info && info->ccCleanse && ctx.enemiesNear >= 2) v += 3.0f;
    return v;
}

// ---------------------------------------------------------------------------
// Item slot picker
// ---------------------------------------------------------------------------

// Picks the best item slot considering situational value.

int bestItemSlot(const std::vector<DefensiveItem>& items,
                 const PlayerSnapshot& self, const FightContext& ctx,
                 int64_t nowMs) {
    int bestSlot = -1;
    float bestValue = 0.0f;
    for (const auto& it : items) {
        if (!itemReady(it, nowMs)) continue;
        float v = situationalItemValue(it, self, ctx);
        if (v > bestValue) {
            bestValue = v;
            bestSlot = it.slot;
        }
    }
    return bestSlot;
}

// ---------------------------------------------------------------------------
// Shield skill picker
// ---------------------------------------------------------------------------

// Picks between shield skills: larger cooldown skill for fights, small
// one for poke.

int pickShieldSkill(const PlayerSnapshot& self, const FightContext& ctx) {
    if (ctx.inFight && self.healthRatio() < 0.6f) return 2;
    return 1;
}

// ---------------------------------------------------------------------------
// Defense pressure meter
// ---------------------------------------------------------------------------

// A 0..1 pressure score: how hard the defense must respond right now.

float defensePressure(const PlayerSnapshot& self, const FightContext& ctx) {
    float hpTerm = (1.0f - self.healthRatio()) * 0.6f;
    float enemyTerm = std::min(1.0f, ctx.enemiesNear / 4.0f) * 0.3f;
    float towerTerm = ctx.underTower ? 0.1f : 0.0f;
    return std::min(1.0f, hpTerm + enemyTerm + towerTerm);
}

// ---------------------------------------------------------------------------
// Action cooldown table
// ---------------------------------------------------------------------------

// Per-action cooldowns tracked globally so triggers can't fire twice in
// a row.

class ActionCooldowns {
public:
    static ActionCooldowns& instance() {
        static ActionCooldowns c;
        return c;
    }

    void mark(DefenseAction a, int64_t nowMs, int cooldownMs) {
        lastMs_[a] = nowMs + cooldownMs;
    }

    bool ready(DefenseAction a, int64_t nowMs) const {
        auto it = lastMs_.find(a);
        if (it == lastMs_.end()) return true;
        return nowMs >= it->second;
    }

    std::string diag() const {
        char buf[96];
        snprintf(buf, sizeof(buf), "cooldowns: active=%d\n",
                 static_cast<int>(lastMs_.size()));
        return std::string(buf);
    }

private:
    std::map<DefenseAction, int64_t> lastMs_;
};

// ---------------------------------------------------------------------------
// Cooldown-aware evaluation
// ---------------------------------------------------------------------------

// Wraps the raw evaluator: suppresses triggers whose action is cooling
// down.

TriggerEval evaluateWithCooldowns(const PlayerSnapshot& self,
                                  const std::vector<PlayerSnapshot>& players,
                                  const std::vector<DefensiveItem>& items,
                                  int64_t nowMs) {
    TriggerEval ev = evaluate(self, players, items, nowMs);
    if (!ev.triggered) return ev;
    if (!ActionCooldowns::instance().ready(ev.action, nowMs)) {
        ev.triggered = false;
    }
    return ev;
}

// ---------------------------------------------------------------------------
// Post-execution mark
// ---------------------------------------------------------------------------

void executeAndMark(const TriggerEval& eval, int64_t nowMs,
                    const TankDefenseConfig& cfg) {
    if (!eval.triggered) return;
    execute(eval, nowMs);
    int cooldown = cfg.guardCooldownMs;
    switch (eval.action) {
        case DefenseAction::kActivateItem:
            cooldown = cfg.itemCooldownMs;
            break;
        case DefenseAction::kCastShield:
            cooldown = cfg.shieldCooldownMs;
            break;
        case DefenseAction::kCastEscape:
            cooldown = cfg.escapeCooldownMs;
            break;
        case DefenseAction::kGuard:
            cooldown = cfg.guardCooldownMs;
            break;
        case DefenseAction::kNone:
            return;
    }
    ActionCooldowns::instance().mark(eval.action, nowMs, cooldown);
}

// ---------------------------------------------------------------------------
// Tank posture
// ---------------------------------------------------------------------------

// Posture changes how aggressive the defense is: peek posture trades
// safety for lane presence.

enum class TankPosture {
    kAggressive,
    kBalanced,
    kPeel,       // protect allies
    kRetreat,
};

TankPosture postureFor(const PlayerSnapshot& self, const FightContext& ctx) {
    if (self.healthRatio() < 0.25f) return TankPosture::kRetreat;
    if (ctx.alliesNear >= 2 && ctx.avgAllyHp < 0.5f) return TankPosture::kPeel;
    if (ctx.enemiesNear == 0) return TankPosture::kAggressive;
    return TankPosture::kBalanced;
}

const char* postureName(TankPosture p) {
    switch (p) {
        case TankPosture::kAggressive: return "aggressive";
        case TankPosture::kBalanced: return "balanced";
        case TankPosture::kPeel: return "peel";
        case TankPosture::kRetreat: return "retreat";
    }
    return "balanced";
}

// ---------------------------------------------------------------------------
// Posture-adapted threshold
// ---------------------------------------------------------------------------

float thresholdForPosture(TankPosture p, float base) {
    switch (p) {
        case TankPosture::kRetreat: return base * 1.4f;
        case TankPosture::kPeel: return base * 1.2f;
        case TankPosture::kBalanced: return base;
        case TankPosture::kAggressive: return base * 0.8f;
    }
    return base;
}

// ---------------------------------------------------------------------------
// Defense report
// ---------------------------------------------------------------------------

std::string defenseReportLine(const PlayerSnapshot& self,
                              const FightContext& ctx, int64_t nowMs) {
    TankPosture p = postureFor(self, ctx);
    float pressure = defensePressure(self, ctx);
    char buf[192];
    snprintf(buf, sizeof(buf), "defense: posture=%s pressure=%.2f enemies=%d\n",
             postureName(p), pressure, ctx.enemiesNear);
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Full posture-aware evaluation
// ---------------------------------------------------------------------------

TriggerEval postureAwareEvaluate(const PlayerSnapshot& self,
                                 const std::vector<PlayerSnapshot>& players,
                                 const std::vector<DefensiveItem>& items,
                                 int64_t nowMs) {
    TankDefenseConfig cfg = TankDefense::instance().cfg();
    FightContext ctx = buildFightContext(self, players);
    TankPosture p = postureFor(self, ctx);
    TriggerEval ev = evaluate(self, players, items, nowMs);
    if (!ev.triggered) return ev;
    if (p == TankPosture::kRetreat && ev.action == DefenseAction::kGuard) {
        ev.action = DefenseAction::kCastEscape;
        ev.skillIndex = 4;
        ev.reason = "retreat escape";
    }
    return ev;
}

// ---------------------------------------------------------------------------
// Defense stats
// ---------------------------------------------------------------------------

class DefenseStats {
public:
    static DefenseStats& instance() {
        static DefenseStats s;
        return s;
    }

    void note(DefenseAction a) { actions_[a]++; }

    int count(DefenseAction a) const {
        auto it = actions_.find(a);
        return it == actions_.end() ? 0 : it->second;
    }

    void reset() { actions_.clear(); }

    std::string diag() const {
        char buf[160];
        snprintf(buf, sizeof(buf), "defense_stats: items=%d shields=%d escapes=%d\n",
                 count(DefenseAction::kActivateItem),
                 count(DefenseAction::kCastShield),
                 count(DefenseAction::kCastEscape));
        return std::string(buf);
    }

private:
    std::map<DefenseAction, int> actions_;
};

// ---------------------------------------------------------------------------
// Full defense diag
// ---------------------------------------------------------------------------

std::string fullDefenseDiag() {
    std::string out = tankDefenseDiag();
    out += ActionCooldowns::instance().diag();
    out += DefenseStats::instance().diag();
    return out;
}

// ---------------------------------------------------------------------------
// Burst detector
// ---------------------------------------------------------------------------

// Detects incoming burst: many enemies near + own HP falling fast. A
// burst alarm overrides every other trigger.

class BurstDetector {
public:
    static BurstDetector& instance() {
        static BurstDetector b;
        return b;
    }

    void sample(float hpRatio, int enemiesNear, int64_t nowMs) {
        if (nowMs - lastMs_ < 100) return;
        float delta = lastHp_ - hpRatio;
        if (delta > 0.08f && enemiesNear >= 2) {
            alarmMs_ = nowMs;
            alarmHp_ = hpRatio;
        }
        lastHp_ = hpRatio;
        lastMs_ = nowMs;
    }

    bool alarmActive(int64_t nowMs) const {
        return alarmMs_ != 0 && nowMs - alarmMs_ < 2000;
    }

    float alarmHp() const { return alarmHp_; }

    void reset() {
        alarmMs_ = 0;
        alarmHp_ = 0.0f;
        lastHp_ = 1.0f;
    }

private:
    float lastHp_ = 1.0f;
    int64_t lastMs_ = 0;
    int64_t alarmMs_ = 0;
    float alarmHp_ = 1.0f;
};

// ---------------------------------------------------------------------------
// Burst response
// ---------------------------------------------------------------------------

// On a burst alarm the defense immediately pops the best item and the
// shield skill together.

TriggerEval burstResponse(const PlayerSnapshot& self,
                          const std::vector<DefensiveItem>& items,
                          const FightContext& ctx, int64_t nowMs) {
    TriggerEval ev;
    if (!BurstDetector::instance().alarmActive(nowMs)) return ev;
    ev.triggered = true;
    ev.kind = TriggerKind::kCcEscaped;
    ev.action = DefenseAction::kActivateItem;
    ev.score = 999.0f;
    ev.reason = "burst alarm";
    const DefensiveItem* it = bestItem(items, nowMs);
    if (it) ev.itemSlot = it->slot;
    return ev;
}

// ---------------------------------------------------------------------------
// Trigger priority ladder
// ---------------------------------------------------------------------------

// The ladder decides which trigger wins when several fire at once.

int triggerLadderRank(TriggerKind k) {
    switch (k) {
        case TriggerKind::kCcEscaped: return 5;
        case TriggerKind::kLowHp: return 4;
        case TriggerKind::kTeamFight: return 3;
        case TriggerKind::kUnderTower: return 2;
        case TriggerKind::kTimer: return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Ladder evaluation
// ---------------------------------------------------------------------------

TriggerEval ladderEvaluate(const PlayerSnapshot& self,
                           const std::vector<PlayerSnapshot>& players,
                           const std::vector<DefensiveItem>& items,
                           int64_t nowMs) {
    FightContext ctx = buildFightContext(self, players);
    TriggerEval best;
    TriggerEval burst = burstResponse(self, items, ctx, nowMs);
    if (burst.triggered) return burst;
    std::vector<TriggerEval> evals;
    evals.push_back(lowHpTrigger(TankDefense::instance().cfg(), self, items,
                                 nowMs));
    evals.push_back(ccEscapeTrigger(TankDefense::instance().cfg(), self, ctx,
                                    nowMs));
    evals.push_back(teamFightTrigger(TankDefense::instance().cfg(), self, ctx,
                                     nowMs));
    evals.push_back(towerTrigger(TankDefense::instance().cfg(), self, ctx,
                                 nowMs));
    evals.push_back(timerTrigger(TankDefense::instance().cfg(), self, nowMs));
    for (const auto& e : evals) {
        if (!e.triggered) continue;
        if (triggerLadderRank(e.kind) > triggerLadderRank(best.kind)) {
            best = e;
        }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Peeling logic
// ---------------------------------------------------------------------------

// Peel: shield the ally with the lowest HP near the fight.

uint32_t peelTarget(const std::vector<PlayerSnapshot>& players,
                    const PlayerSnapshot& self, float radius) {
    uint32_t best = 0;
    float bestHp = 1.0f;
    for (const auto& p : players) {
        if (p.team == EntityTeam::kEnemy || p.team == EntityTeam::kUnknown) {
            continue;
        }
        if (!p.alive || p.inFog) continue;
        if (p.id == self.id) continue;
        float d = distXz(p.position, self.position);
        if (d > radius) continue;
        if (p.healthRatio() < bestHp) {
            bestHp = p.healthRatio();
            best = p.id;
        }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Peel shield execution
// ---------------------------------------------------------------------------

TriggerEval peelShield(const std::vector<PlayerSnapshot>& players,
                       const PlayerSnapshot& self, int64_t nowMs) {
    TriggerEval ev;
    TankDefenseConfig cfg = TankDefense::instance().cfg();
    if (!cfg.autoShields) return ev;
    uint32_t target = peelTarget(players, self, 800.0f);
    if (target == 0) return ev;
    if (self.healthRatio() < 0.4f) return ev;
    ev.triggered = true;
    ev.kind = TriggerKind::kTeamFight;
    ev.action = DefenseAction::kCastShield;
    ev.skillIndex = 2;
    ev.score = 70.0f;
    ev.reason = "peel ally";
    return ev;
}

// ---------------------------------------------------------------------------
// Tank line
// ---------------------------------------------------------------------------

std::string tankLine(const PlayerSnapshot& self, int64_t nowMs) {
    char buf[128];
    snprintf(buf, sizeof(buf), "tank: hp=%.0f%% burst=%d\n",
             self.healthRatio() * 100.0f,
             BurstDetector::instance().alarmActive(nowMs) ? 1 : 0);
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Full tank suite diag
// ---------------------------------------------------------------------------

std::string tankSuiteDiag() {
    std::string out = fullDefenseDiag();
    out += "burst: ";
    out += BurstDetector::instance().alarmActive(utils::nowMs()) ? "alarm\n"
                                                          : "clear\n";
    return out;
}

// ---------------------------------------------------------------------------
// Regen window
// ---------------------------------------------------------------------------

// Some items (e.g. Vengeance) heal or reduce damage during a window; the
// window tracker remembers when the effect is active so triggers wait.

class RegenWindow {
public:
    static RegenWindow& instance() {
        static RegenWindow r;
        return r;
    }

    void open(int64_t nowMs, int64_t durationMs) {
        untilMs_ = nowMs + durationMs;
    }

    bool active(int64_t nowMs) const { return nowMs < untilMs_; }

    int64_t remainingMs(int64_t nowMs) const {
        return std::max<int64_t>(0, untilMs_ - nowMs);
    }

    std::string diag() const {
        char buf[96];
        snprintf(buf, sizeof(buf), "regen: active=%d\n",
                 active(utils::nowMs()) ? 1 : 0);
        return std::string(buf);
    }

private:
    int64_t untilMs_ = 0;
};

// ---------------------------------------------------------------------------
// Item stagger
// ---------------------------------------------------------------------------

// Never pop two items in the same instant; the stagger spreads item
// activations across a short window.

class ItemStagger {
public:
    static ItemStagger& instance() {
        static ItemStagger s;
        return s;
    }

    bool allowed(int64_t nowMs) const {
        return nowMs - lastSlotMs_ >= 1500;
    }

    void mark(int64_t nowMs) { lastSlotMs_ = nowMs; }

private:
    int64_t lastSlotMs_ = 0;
};

// ---------------------------------------------------------------------------
// Shield overlap guard
// ---------------------------------------------------------------------------

// Shields don't stack; the guard skips a shield cast while a shield
// effect is still up.

bool shieldOverlap(int64_t lastShieldMs, int64_t nowMs) {
    return nowMs - lastShieldMs < 4000;
}

// ---------------------------------------------------------------------------
// Defensive posture summary
// ---------------------------------------------------------------------------

std::string postureSummaryLine(const PlayerSnapshot& self,
                               const FightContext& ctx) {
    TankPosture p = postureFor(self, ctx);
    char buf[160];
    snprintf(buf, sizeof(buf), "posture: %s pressure=%.2f\n",
             postureName(p), defensePressure(self, ctx));
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Match lifecycle hooks
// ---------------------------------------------------------------------------

void onMatchStart() {
    ActionCooldowns::instance();
    BurstDetector::instance().reset();
    RegenWindow::instance();
    DefenseStats::instance().reset();
    ARIFT_INFO(kTagTankDefense, "Tank Defense match start");
}

void onMatchEnd() {
    DefenseStats::instance().reset();
    BurstDetector::instance().reset();
    ARIFT_INFO(kTagTankDefense, "Tank Defense match end");
}

// ---------------------------------------------------------------------------
// Defense readiness check
// ---------------------------------------------------------------------------

bool defenseReady(const TankDefenseConfig& cfg, const TriggerEval& ev,
                  int64_t nowMs) {
    if (!ev.triggered) return false;
    if (ev.action == DefenseAction::kActivateItem &&
        !ItemStagger::instance().allowed(nowMs)) {
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Final defense driver
// ---------------------------------------------------------------------------

TriggerEval runDefenseDriver(const PlayerSnapshot& self,
                             const std::vector<PlayerSnapshot>& players,
                             const std::vector<DefensiveItem>& items,
                             int64_t nowMs) {
    if (!enabled()) return TriggerEval();
    FightContext ctx = buildFightContext(self, players);
    BurstDetector::instance().sample(self.healthRatio(), ctx.enemiesNear,
                                     nowMs);
    TriggerEval ev = ladderEvaluate(self, players, items, nowMs);
    if (!defenseReady(TankDefense::instance().cfg(), ev, nowMs)) {
        ev.triggered = false;
    }
    if (ev.triggered) {
        DefenseStats::instance().note(ev.action);
        executeAndMark(ev, nowMs, TankDefense::instance().cfg());
        if (ev.action == DefenseAction::kActivateItem) {
            ItemStagger::instance().mark(nowMs);
        }
    }
    return ev;
}

// ---------------------------------------------------------------------------
// Defense status line
// ---------------------------------------------------------------------------

std::string defenseStatusLine() {
    TankDefense& m = TankDefense::instance();
    TriggerEval ev = m.lastEval();
    char buf[192];
    snprintf(buf, sizeof(buf), "defense_status: trigger=%d last=%s\n",
             ev.triggered ? 1 : 0,
             ev.triggered ? ev.reason.c_str() : "idle");
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Complete defense diag
// ---------------------------------------------------------------------------

std::string completeDefenseDiag() {
    std::string out = tankSuiteDiag();
    out += RegenWindow::instance().diag();
    out += defenseStatusLine();
    return out;
}

// ---------------------------------------------------------------------------
// Tank guard line (HUD)
// ---------------------------------------------------------------------------

std::string tankGuardLine(const PlayerSnapshot& self) {
    char buf[128];
    snprintf(buf, sizeof(buf), "tank_guard: hp=%.0f%% thr=%.0f%%\n",
             self.healthRatio() * 100.0f,
             TankDefense::instance().cfg().lowHpThreshold * 100.0f);
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Module summary
// ---------------------------------------------------------------------------

std::string tankSummary() {
    std::string out = completeDefenseDiag();
    out += tankGuardLine(TankDefense::instance().selfRef());
    return out;
}

}  // namespace tankdefense
}  // namespace arift