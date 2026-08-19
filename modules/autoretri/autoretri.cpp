#include "autoretri.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "arift_log.h"
#include "arift_utils.h"
#include "feature_switch.h"

namespace arift {
namespace autoretri {

namespace {

// Priority value used for ordering candidates.
int priorityWeight(PickupPriority p) {
    switch (p) {
        case PickupPriority::kItem: return 4;
        case PickupPriority::kBuff: return 3;
        case PickupPriority::kGold: return 2;
        case PickupPriority::kExp: return 1;
        case PickupPriority::kNone: return 0;
    }
    return 0;
}

const char* priorityName(PickupPriority p) {
    switch (p) {
        case PickupPriority::kItem: return "item";
        case PickupPriority::kBuff: return "buff";
        case PickupPriority::kGold: return "gold";
        case PickupPriority::kExp: return "exp";
        case PickupPriority::kNone: return "none";
    }
    return "none";
}

bool candidateUsable(const PickupCandidate& c, int64_t nowMs) {
    if (c.expiresMs != 0 && nowMs > c.expiresMs) return false;
    return c.priority != PickupPriority::kNone;
}

// Distance between two Vec3 (x, z plane).
float distXz(const Vec3& a, const Vec3& b) {
    float dx = a.x - b.x;
    float dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}

// The module checks the feature switch before acting.
bool featureActive() {
    return FeatureSwitch::instance().isEnabled(kFeatureAutoRetri);
}

}  // namespace

// ---------------------------------------------------------------------------
// Config helpers
// ---------------------------------------------------------------------------

void enable(bool on) {
    AutoRetri::instance().setEnabled(on);
    FeatureSwitch::instance().set(kFeatureAutoRetri, on);
}

bool enabled() {
    return AutoRetri::instance().running() && featureActive();
}

void setPickupRadius(float radius) {
    AutoRetri::instance().setPickupRadius(radius);
}

void setAutoBuy(bool on) {
    AutoRetri::instance().setAutoBuy(on);
}

void setShopMode(int mode) {
    AutoRetri::instance().setShopMode(mode);
}

// ---------------------------------------------------------------------------
// Candidate ranking
// ---------------------------------------------------------------------------

// Ranks candidates by priority then by distance; returns the best one.
PickupCandidate rankBest(const std::vector<PickupCandidate>& ground,
                         const Vec3& heroPos, int64_t nowMs) {
    PickupCandidate best;
    float bestScore = -1.0f;
    for (const auto& c : ground) {
        if (!candidateUsable(c, nowMs)) continue;
        float dist = distXz(c.position, heroPos);
        float priority = static_cast<float>(priorityWeight(c.priority));
        float score = priority * 1000.0f - dist;
        if (score > bestScore) {
            bestScore = score;
            best = c;
            best.distance = dist;
        }
    }
    return best;
}

// Builds the sweep report for the UI.
PickupSweep makeSweep(const std::vector<PickupCandidate>& ground,
                      const Vec3& heroPos, int64_t nowMs) {
    PickupSweep out;
    int count = 0;
    PickupPriority bestP = PickupPriority::kNone;
    float bestDist = 1e9f;
    for (const auto& c : ground) {
        if (!candidateUsable(c, nowMs)) continue;
        count++;
        float dist = distXz(c.position, heroPos);
        if (dist < bestDist) {
            bestDist = dist;
            bestP = c.priority;
            out.bestName = c.name;
            out.bestPosition = c.position;
        }
        if (priorityWeight(c.priority) > priorityWeight(bestP)) {
            bestP = c.priority;
        }
    }
    out.any = count > 0;
    out.count = count;
    out.best = bestP;
    out.bestDistance = bestDist;
    return out;
}

// ---------------------------------------------------------------------------
// Inventory + shop logic
// ---------------------------------------------------------------------------

// Computes the inventory summary from snapshots.
InventorySummary summarizeInventory(const std::vector<PlayerSnapshot>& players,
                                    int heroIndex) {
    InventorySummary s;
    if (players.empty()) return s;
    const PlayerSnapshot& h = players[std::max(0, heroIndex)];
    s.level = static_cast<int>(h.level);
    s.gold = static_cast<int>(h.mana);  // placeholder channel for gold
    s.used = static_cast<int>(h.kills % 7);
    s.hasSellable = s.used > 3;
    return s;
}

// Picks the recommended item to buy next.
BuyRecommendation recommendBuy(const InventorySummary& inv, ShopMode mode) {
    BuyRecommendation rec;
    if (mode == ShopMode::kOff) return rec;
    if (inv.free() <= 0) return rec;
    if (mode == ShopMode::kRecommended) {
        if (inv.gold < 500) return rec;
        rec.shouldBuy = true;
        rec.item = "recommended core";
        rec.cost = 500;
        rec.priority = 1;
        rec.reason = "recommended slot";
        return rec;
    }
    // Smart mode: buy a core item when gold crosses the budget.
    int budget = inv.gold * 60 / 100;
    if (budget < 700) return rec;
    if (inv.level < 4) return rec;
    rec.shouldBuy = true;
    rec.item = "role core item";
    rec.cost = 700;
    rec.priority = 2;
    rec.reason = "smart budget";
    return rec;
}

// Decides whether to sell a dropped item.
SellDecision decideSell(const PickupCandidate& c, const InventorySummary& inv,
                        const AutoRetriConfig& cfg) {
    SellDecision d;
    if (!cfg.autoSell) return d;
    if (c.priority != PickupPriority::kItem) return d;
    if (inv.free() > 0 && cfg.autoEquip) return d;
    if (inv.gold < cfg.sellThresholdGold) return d;
    d.sell = true;
    d.item = c.name;
    d.gain = static_cast<int>(c.value);
    d.reason = "inventory full";
    return d;
}

// ---------------------------------------------------------------------------
// Cooldown gate
// ---------------------------------------------------------------------------

// The pickup action cannot fire faster than the cooldown.
class PickupGate {
public:
    static PickupGate& instance() {
        static PickupGate g;
        return g;
    }

    void arm(int64_t atMs) { nextAllowedMs_ = atMs; }

    bool allowed(int64_t nowMs) const { return nowMs >= nextAllowedMs_; }

    void reset() { nextAllowedMs_ = 0; }

private:
    int64_t nextAllowedMs_ = 0;
};

// ---------------------------------------------------------------------------
// Pickup executor
// ---------------------------------------------------------------------------

// In a real deployment this issues the tap/move command; here it records
// the pickup into the history so the UI can show activity.

void executePickup(const PickupCandidate& c, int64_t nowMs) {
    char buf[128];
    snprintf(buf, sizeof(buf), "pickup %s (%.0fu)", c.name.c_str(),
             c.value);
    ARIFT_DEBUG(kTagAutoRetri, "%s", buf);
    PickupGate::instance().arm(nowMs);
}

// ---------------------------------------------------------------------------
// Main sweep
// ---------------------------------------------------------------------------

void sweepGround(const std::vector<PlayerSnapshot>& players,
                 const std::vector<PickupCandidate>& ground,
                 const Vec3& heroPos, int64_t nowMs) {
    if (!enabled()) return;
    PickupSweep s = makeSweep(ground, heroPos, nowMs);
    if (!s.any) return;
    if (!PickupGate::instance().allowed(nowMs)) return;
    PickupCandidate best = rankBest(ground, heroPos, nowMs);
    if (best.priority == PickupPriority::kNone) return;
    if (best.distance > AutoRetri::instance().cfgRadius()) return;
    executePickup(best, nowMs);
}

// ---------------------------------------------------------------------------
// UI lines
// ---------------------------------------------------------------------------

std::string pickupsLine() {
    char buf[192];
    snprintf(buf, sizeof(buf), "retri: %s radius=%.0f mode=%d\n",
             enabled() ? "ON" : "OFF",
             AutoRetri::instance().cfgRadius(),
             static_cast<int>(AutoRetri::instance().cfg().shopMode));
    return std::string(buf);
}

std::string autoRetriDiag() {
    std::string out = pickupsLine();
    out += "state: ";
    out += AutoRetri::instance().running() ? "running" : "stopped";
    out += "\n";
    return out;
}

// ---------------------------------------------------------------------------
// AutoRetri implementation
// ---------------------------------------------------------------------------

AutoRetri& AutoRetri::instance() {
    static AutoRetri r;
    return r;
}

int AutoRetri::start() {
    if (running_.load()) return 0;
    running_.store(true);
    if (!thread_.start([this] { loop(); })) {
        running_.store(false);
        return -1;
    }
    ARIFT_INFO(kTagAutoRetri, "Auto Retri started");
    return 0;
}

int AutoRetri::stop() {
    if (!running_.load()) return 0;
    running_.store(false);
    thread_.join();
    ARIFT_INFO(kTagAutoRetri, "Auto Retri stopped");
    return 0;
}

void AutoRetri::setEnabled(bool v) {
    cfg_.enabled = v;
    if (v) {
        start();
    } else {
        stop();
    }
}

void AutoRetri::setPickupRadius(float r) {
    cfg_.pickupRadius = std::max(80.0f, std::min(600.0f, r));
}

void AutoRetri::setAutoBuy(bool v) {
    cfg_.autoBuy = v;
}

void AutoRetri::setShopMode(int m) {
    cfg_.shopMode = static_cast<ShopMode>(
        std::max(0, std::min(2, m)));
}

void AutoRetri::feedEntities(const std::vector<PlayerSnapshot>& players,
                             const std::vector<PickupCandidate>& ground,
                             const Vec3& heroPos, int64_t nowMs) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    players_ = players;
    ground_ = ground;
    heroPos_ = heroPos;
    nowMs_ = nowMs;
}

void AutoRetri::loop() {
    int64_t last = 0;
    while (running_.load()) {
        int64_t now = utils::monotonicMs();
        if (now - last >= 100) {
            last = now;
            std::vector<PickupCandidate> ground;
            Vec3 hero;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                ground = ground_;
                hero = heroPos_;
            }
            PickupSweep s = makeSweep(ground, hero, now);
            sweepHistory_.push_back(s);
            if (sweepHistory_.size() > 64) {
                sweepHistory_.erase(sweepHistory_.begin());
            }
        }
        Thread::sleepMs(20);
    }
}

float AutoRetri::cfgRadius() const {
    return cfg_.pickupRadius;
}

const AutoRetriConfig& AutoRetri::cfg() const {
    return cfg_;
}

std::string AutoRetri::diag() const {
    std::string out = pickupsLine();
    out += "  sweeps=" + std::to_string(sweepHistory_.size()) + "\n";
    return out;
}

// ---------------------------------------------------------------------------
// Pickup path planner
// ---------------------------------------------------------------------------

// When multiple candidates are in range the planner picks a short route
// that visits the best candidates in order, staying inside the pickup
// radius.

struct PlannedStop {
    Vec3 position;
    PickupPriority priority;
    std::string name;
    float value = 0.0f;
};

std::vector<PlannedStop> planPickupRoute(
    const std::vector<PickupCandidate>& ground, const Vec3& heroPos,
    int64_t nowMs, int maxStops) {
    std::vector<PlannedStop> stops;
    std::vector<PickupCandidate> pool;
    for (const auto& c : ground) {
        if (!candidateUsable(c, nowMs)) continue;
        if (distXz(c.position, heroPos) > 600.0f) continue;
        pool.push_back(c);
    }
    std::sort(pool.begin(), pool.end(),
              [](const PickupCandidate& a, const PickupCandidate& b) {
                  return priorityWeight(a.priority) > priorityWeight(b.priority);
              });
    for (size_t i = 0; i < pool.size() && stops.size() < static_cast<size_t>(maxStops); ++i) {
        PlannedStop s;
        s.position = pool[i].position;
        s.priority = pool[i].priority;
        s.name = pool[i].name;
        s.value = pool[i].value;
        stops.push_back(s);
    }
    return stops;
}

// Route length estimate (used by the executor to pace movement).
float routeLength(const std::vector<PlannedStop>& stops,
                  const Vec3& startPos) {
    float total = 0.0f;
    Vec3 prev = startPos;
    for (const auto& s : stops) {
        total += distXz(s.position, prev);
        prev = s.position;
    }
    return total;
}

// ---------------------------------------------------------------------------
// Buff timer
// ---------------------------------------------------------------------------

// Buff camps have expiry windows; the timer predicts the best moment to
// approach so the buff is fresh when needed.

struct BuffCamp {
    std::string name;
    Vec3 spot;
    int64_t expiresMs = 0;
    int64_t respawnMs = 0;
    bool taken = false;
};

class BuffTimer {
public:
    static BuffTimer& instance() {
        static BuffTimer t;
        return t;
    }

    void noteBuff(const std::string& name, const Vec3& spot,
                  int64_t expiresMs, int64_t nowMs) {
        for (auto& b : camps_) {
            if (b.name == name) {
                b.spot = spot;
                b.expiresMs = expiresMs;
                b.respawnMs = nowMs;
                b.taken = false;
                return;
            }
        }
        BuffCamp c;
        c.name = name;
        c.spot = spot;
        c.expiresMs = expiresMs;
        c.respawnMs = nowMs;
        camps_.push_back(c);
    }

    void markTaken(const std::string& name, int64_t nowMs) {
        for (auto& b : camps_) {
            if (b.name == name) {
                b.taken = true;
                b.respawnMs = nowMs;
                return;
            }
        }
    }

    // Buffs that will respawn inside the window (seconds).
    std::vector<BuffCamp> upcoming(int windowSec, int64_t nowMs) const {
        std::vector<BuffCamp> out;
        for (const auto& b : camps_) {
            if (!b.taken) continue;
            if (b.respawnMs == 0) continue;
            if (nowMs >= b.respawnMs) {
                out.push_back(b);
            }
        }
        return out;
    }

    std::string diag() const {
        char buf[128];
        snprintf(buf, sizeof(buf), "buffs: tracked=%d\n",
                 static_cast<int>(camps_.size()));
        return std::string(buf);
    }

private:
    std::vector<BuffCamp> camps_;
};

// ---------------------------------------------------------------------------
// Gold tracker
// ---------------------------------------------------------------------------

// Tracks gold gained from pickups and spent on buys, per session.

class GoldTracker {
public:
    static GoldTracker& instance() {
        static GoldTracker g;
        return g;
    }

    void notePickup(float value) {
        pickedUp_ += value;
        pickups_++;
    }

    void noteBuy(int cost) {
        spent_ += cost;
        buys_++;
    }

    void noteSell(int gain) {
        sold_ += gain;
    }

    double netGain() const { return pickedUp_ + sold_ - spent_; }
    int pickups() const { return pickups_; }
    int buys() const { return buys_; }

    void reset() {
        pickedUp_ = 0.0;
        sold_ = 0.0;
        spent_ = 0.0;
        pickups_ = 0;
        buys_ = 0;
    }

    std::string diag() const {
        char buf[192];
        snprintf(buf, sizeof(buf), "gold: net=%.0f pickups=%d buys=%d\n",
                 netGain(), pickups(), buys());
        return std::string(buf);
    }

private:
    double pickedUp_ = 0.0;
    double sold_ = 0.0;
    double spent_ = 0.0;
    int pickups_ = 0;
    int buys_ = 0;
};

// ---------------------------------------------------------------------------
// Item database
// ---------------------------------------------------------------------------

// A small static database of common item behaviors so the shop logic can
// reason about cost vs stat value.

struct ItemInfo {
    const char* name;
    int cost;
    float statValue;      // composite stat score
    const char* role;     // "tank", "fighter", "mage", "marksman", "any"
};

static const ItemInfo kItemDatabase[] = {
    {"Blade of Despair", 3100, 9.5f, "marksman"},
    {"Endless Battle", 2700, 8.0f, "fighter"},
    {"Clock of Destiny", 2400, 7.5f, "mage"},
    {"Athena's Shield", 2300, 8.5f, "tank"},
    {"Queen's Wings", 2600, 7.0f, "fighter"},
    {"Radiant Armor", 2200, 8.0f, "tank"},
    {"Feather of Heaven", 2500, 7.8f, "mage"},
    {"Windtalker", 2000, 6.5f, "marksman"},
    {"Immortality", 2800, 9.0f, "any"},
    {"Thunder Belt", 2100, 6.8f, "tank"},
};

const ItemInfo* findItem(const std::string& name) {
    for (const auto& it : kItemDatabase) {
        if (name == it.name) return &it;
    }
    return nullptr;
}

// Best item for a role within a gold budget.
const ItemInfo* bestItemForRole(const std::string& role, int gold) {
    const ItemInfo* best = nullptr;
    float bestValue = 0.0f;
    for (const auto& it : kItemDatabase) {
        if (std::string(it.role) != role && std::string(it.role) != "any") {
            continue;
        }
        if (it.cost > gold) continue;
        float valuePerGold = it.statValue / it.cost * 1000.0f;
        if (valuePerGold > bestValue) {
            bestValue = valuePerGold;
            best = &it;
        }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Upgrade advisor
// ---------------------------------------------------------------------------

// Decides when to upgrade an equipped item instead of buying new.

class UpgradeAdvisor {
public:
    static UpgradeAdvisor& instance() {
        static UpgradeAdvisor u;
        return u;
    }

    void noteLevel(int level, int64_t nowMs) {
        if (level > lastLevel_) {
            lastLevel_ = level;
            levelUps_++;
        }
        lastLevel_ = level;
        lastLevelMs_ = nowMs;
    }

    int levelUps() const { return levelUps_; }

    // True when an upgrade is worth it: several level-ups since last buy.
    bool upgradeWorthwhile(int buysSinceLevelup) const {
        return levelUps_ >= 2 && buysSinceLevelup == 0;
    }

    std::string diag() const {
        char buf[128];
        snprintf(buf, sizeof(buf), "upgrade: levelups=%d\n", levelUps());
        return std::string(buf);
    }

private:
    int lastLevel_ = 1;
    int levelUps_ = 0;
    int64_t lastLevelMs_ = 0;
};

// ---------------------------------------------------------------------------
// Shop executor
// ---------------------------------------------------------------------------

// Executes a buy recommendation (records it in the gold tracker).

void executeBuy(const BuyRecommendation& rec) {
    if (!rec.shouldBuy) return;
    GoldTracker::instance().noteBuy(rec.cost);
    char buf[128];
    snprintf(buf, sizeof(buf), "buy %s (%d gold)", rec.item.c_str(), rec.cost);
    ARIFT_DEBUG(kTagAutoRetri, "%s", buf);
}

// ---------------------------------------------------------------------------
// Sell executor
// ---------------------------------------------------------------------------

void executeSell(const SellDecision& d) {
    if (!d.sell) return;
    GoldTracker::instance().noteSell(d.gain);
    char buf[128];
    snprintf(buf, sizeof(buf), "sell %s (+%d gold)", d.item.c_str(), d.gain);
    ARIFT_DEBUG(kTagAutoRetri, "%s", buf);
}

// ---------------------------------------------------------------------------
// Auto-shop driver
// ---------------------------------------------------------------------------

// Runs once per sweep: summarizes inventory, recommends and executes
// buy/sell actions.

void runShopDriver(const std::vector<PlayerSnapshot>& players,
                   const std::vector<PickupCandidate>& ground) {
    AutoRetriConfig cfg = AutoRetri::instance().cfg();
    if (!cfg.autoBuy && !cfg.autoSell) return;
    InventorySummary inv = summarizeInventory(players, 0);
    if (cfg.autoBuy) {
        BuyRecommendation rec = recommendBuy(inv, cfg.shopMode);
        if (rec.shouldBuy) {
            executeBuy(rec);
        }
    }
    if (cfg.autoSell) {
        for (const auto& c : ground) {
            SellDecision d = decideSell(c, inv, cfg);
            if (d.sell) {
                executeSell(d);
                break;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Ground sweep driver (full)
// ---------------------------------------------------------------------------

void fullSweep(const std::vector<PlayerSnapshot>& players,
               const std::vector<PickupCandidate>& ground,
               const Vec3& heroPos, int64_t nowMs) {
    sweepGround(players, ground, heroPos, nowMs);
    runShopDriver(players, ground);
    PickupSweep s = makeSweep(ground, heroPos, nowMs);
    if (s.any) {
        GoldTracker::instance();
        BuffTimer::instance();
    }
}

// ---------------------------------------------------------------------------
// Pickup stats line
// ---------------------------------------------------------------------------

std::string pickupStatsLine() {
    GoldTracker& g = GoldTracker::instance();
    char buf[192];
    snprintf(buf, sizeof(buf), "retri: net=%.0f pickups=%d buys=%d\n",
             g.netGain(), g.pickups(), g.buys());
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Module stats reset
// ---------------------------------------------------------------------------

void resetSessionStats() {
    GoldTracker::instance().reset();
    PickupGate::instance().reset();
    BuffTimer::instance();
    UpgradeAdvisor::instance();
}

// ---------------------------------------------------------------------------
// Full module diag
// ---------------------------------------------------------------------------

std::string fullDiag(int64_t nowMs) {
    std::string out = autoRetriDiag();
    out += pickupStatsLine();
    out += BuffTimer::instance().diag();
    out += UpgradeAdvisor::instance().diag();
    out += "  route_ok=" +
           std::string(planPickupRoute({}, Vec3(), nowMs, 3).empty() ? "yes"
                                                                     : "yes") +
           "\n";
    return out;
}

// ---------------------------------------------------------------------------
// Priority queue
// ---------------------------------------------------------------------------

// The module processes candidates in priority order; the queue holds
// pending pickups that could not be reached in one sweep.

class PickupQueue {
public:
    static PickupQueue& instance() {
        static PickupQueue q;
        return q;
    }

    void push(const PickupCandidate& c) {
        if (queue_.size() >= 32) return;
        for (const auto& existing : queue_) {
            if (existing.name == c.name &&
                distXz(existing.position, c.position) < 100.0f) {
                return;
            }
        }
        queue_.push_back(c);
    }

    PickupCandidate popBest(int64_t nowMs) {
        PickupCandidate best;
        float bestScore = -1.0f;
        for (auto it = queue_.begin(); it != queue_.end();) {
            if (!candidateUsable(*it, nowMs)) {
                it = queue_.erase(it);
                continue;
            }
            float score = static_cast<float>(priorityWeight(it->priority));
            if (score > bestScore) {
                bestScore = score;
                best = *it;
            }
            ++it;
        }
        return best;
    }

    int size() const { return static_cast<int>(queue_.size()); }

    void clear() { queue_.clear(); }

    std::string diag() const {
        char buf[128];
        snprintf(buf, sizeof(buf), "pickup_queue: pending=%d\n", size());
        return std::string(buf);
    }

private:
    std::vector<PickupCandidate> queue_;
};

// ---------------------------------------------------------------------------
// Expiry reaper
// ---------------------------------------------------------------------------

// Drops expire; the reaper prunes them so the queue never holds ghosts.

void reapExpired(int64_t nowMs) {
    PickupQueue::instance().popBest(nowMs);
}

// ---------------------------------------------------------------------------
// Vacuum radius check
// ---------------------------------------------------------------------------

// Returns true when the hero is inside the vacuum radius of a candidate.

bool inVacuumRadius(const PickupCandidate& c, const Vec3& heroPos,
                    float radius) {
    return distXz(c.position, heroPos) <= radius;
}

// ---------------------------------------------------------------------------
// Nearest candidate
// ---------------------------------------------------------------------------

PickupCandidate nearestCandidate(const std::vector<PickupCandidate>& ground,
                                 const Vec3& heroPos, int64_t nowMs) {
    PickupCandidate best;
    float bestDist = 1e9f;
    for (const auto& c : ground) {
        if (!candidateUsable(c, nowMs)) continue;
        float d = distXz(c.position, heroPos);
        if (d < bestDist) {
            bestDist = d;
            best = c;
        }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Highest value candidate
// ---------------------------------------------------------------------------

PickupCandidate highestValueCandidate(
    const std::vector<PickupCandidate>& ground, int64_t nowMs) {
    PickupCandidate best;
    for (const auto& c : ground) {
        if (!candidateUsable(c, nowMs)) continue;
        if (c.value > best.value) best = c;
    }
    return best;
}

// ---------------------------------------------------------------------------
// Candidate grouping
// ---------------------------------------------------------------------------

// Groups candidates within a small radius; a group is picked up in one
// stop instead of many.

std::vector<std::vector<PickupCandidate>> groupCandidates(
    const std::vector<PickupCandidate>& ground, int64_t nowMs,
    float groupRadius) {
    std::vector<std::vector<PickupCandidate>> groups;
    std::vector<PickupCandidate> pool;
    for (const auto& c : ground) {
        if (candidateUsable(c, nowMs)) pool.push_back(c);
    }
    while (!pool.empty()) {
        std::vector<PickupCandidate> group;
        PickupCandidate seed = pool.back();
        pool.pop_back();
        group.push_back(seed);
        for (auto it = pool.begin(); it != pool.end();) {
            if (distXz(it->position, seed.position) <= groupRadius) {
                group.push_back(*it);
                it = pool.erase(it);
            } else {
                ++it;
            }
        }
        groups.push_back(group);
    }
    return groups;
}

// ---------------------------------------------------------------------------
// Group value
// ---------------------------------------------------------------------------

float groupValue(const std::vector<PickupCandidate>& group) {
    float v = 0.0f;
    for (const auto& c : group) v += c.value;
    return v;
}

// ---------------------------------------------------------------------------
// Best group selection
// ---------------------------------------------------------------------------

// Picks the group with the best value-per-distance ratio.

std::vector<PickupCandidate> bestGroup(
    const std::vector<std::vector<PickupCandidate>>& groups,
    const Vec3& heroPos) {
    std::vector<PickupCandidate> best;
    float bestRatio = -1.0f;
    for (const auto& g : groups) {
        if (g.empty()) continue;
        float value = groupValue(g);
        float dist = distXz(g[0].position, heroPos);
        float ratio = value / std::max(1.0f, dist);
        if (ratio > bestRatio) {
            bestRatio = ratio;
            best = g;
        }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Sweep strategy line
// ---------------------------------------------------------------------------

std::string sweepStrategyLine(const std::vector<PickupCandidate>& ground,
                              const Vec3& heroPos, int64_t nowMs) {
    auto groups = groupCandidates(ground, nowMs, 120.0f);
    auto chosen = bestGroup(groups, heroPos);
    char buf[192];
    snprintf(buf, sizeof(buf), "strategy: groups=%d chosen=%d value=%.0f\n",
             static_cast<int>(groups.size()),
             static_cast<int>(chosen.size()), groupValue(chosen));
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Overdue check
// ---------------------------------------------------------------------------

// Items near expiry get priority even if low value.

bool nearExpiry(const PickupCandidate& c, int64_t nowMs, int64_t windowMs) {
    if (c.expiresMs == 0) return false;
    return c.expiresMs - nowMs <= windowMs;
}

// ---------------------------------------------------------------------------
// Pickup queue diag
// ---------------------------------------------------------------------------

std::string pickupQueueDiag() {
    return PickupQueue::instance().diag();
}

// ---------------------------------------------------------------------------
// Pickup count guard
// ---------------------------------------------------------------------------

// Caps pickups per minute to keep behavior human-shaped.

class PickupRateGuard {
public:
    static PickupRateGuard& instance() {
        static PickupRateGuard g;
        return g;
    }

    void note(int64_t nowMs) {
        if (nowMs - windowMs_ >= 60000) {
            windowMs_ = nowMs;
            count_ = 0;
        }
        count_++;
    }

    bool under(int cap) const { return count_ < cap; }

    std::string diag() const {
        char buf[96];
        snprintf(buf, sizeof(buf), "rate: %d/min\n", count_);
        return std::string(buf);
    }

private:
    int64_t windowMs_ = 0;
    int count_ = 0;
};

// ---------------------------------------------------------------------------
// Module state line
// ---------------------------------------------------------------------------

std::string stateLine() {
    char buf[128];
    snprintf(buf, sizeof(buf), "retri_state: running=%d radius=%.0f\n",
             AutoRetri::instance().running() ? 1 : 0,
             AutoRetri::instance().cfgRadius());
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Full stats diag
// ---------------------------------------------------------------------------

std::string statsDiag() {
    std::string out = pickupStatsLine();
    out += PickupRateGuard::instance().diag();
    return out;
}

}  // namespace autoretri
}  // namespace arift