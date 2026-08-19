#include "maphack_manager.h"

#include "arift_config.h"
#include "arift_log.h"
#include "arift_time.h"
#include "arift_utils.h"
#include "cheat_registry.h"
#include "feature_switch.h"

namespace arift {

void MapHackSettings::loadFromConfig() {
    Config& cfg = Config::instance();
    enabled = cfg.getBool("maphack", "enabled", enabled);
    fogBypass = cfg.getBool("maphack", "fog_bypass", fogBypass);
    minimapOverride = cfg.getBool("maphack", "minimap_override", minimapOverride);
    revealWards = cfg.getBool("maphack", "reveal_wards", revealWards);
    revealTraps = cfg.getBool("maphack", "reveal_traps", revealTraps);
    revealJungle = cfg.getBool("maphack", "reveal_jungle", revealJungle);
    revealEnemyRecall = cfg.getBool("maphack", "reveal_enemy_recall", revealEnemyRecall);
    highlightObjectives = cfg.getBool("maphack", "highlight_objectives", highlightObjectives);
    visionRadius = cfg.getFloat("maphack", "vision_radius", visionRadius);
    revealFadeMs = cfg.getFloat("maphack", "reveal_fade_ms", revealFadeMs);
}

void MapHackSettings::saveToConfig() const {
    Config& cfg = Config::instance();
    cfg.setBool("maphack", "enabled", enabled);
    cfg.setBool("maphack", "fog_bypass", fogBypass);
    cfg.setBool("maphack", "minimap_override", minimapOverride);
    cfg.setBool("maphack", "reveal_wards", revealWards);
    cfg.setBool("maphack", "reveal_traps", revealTraps);
    cfg.setBool("maphack", "reveal_jungle", revealJungle);
    cfg.setBool("maphack", "reveal_enemy_recall", revealEnemyRecall);
    cfg.setBool("maphack", "highlight_objectives", highlightObjectives);
    cfg.setFloat("maphack", "vision_radius", visionRadius);
    cfg.setFloat("maphack", "reveal_fade_ms", revealFadeMs);
}

MapHackManager& MapHackManager::instance() {
    static MapHackManager mgr;
    return mgr;
}

int MapHackManager::start() {
    if (running_.load()) return 0;

    settings_.loadFromConfig();
    fog_.setEnabled(settings_.fogBypass);
    minimap_.setEnabled(settings_.minimapOverride);
    fog_.grid().resize(64, 64, 100.0f, Vec2(0.0f, 0.0f));

    MinimapParams params;
    params.width = 160;
    params.height = 160;
    params.offset = Vec2(900.0f, 2120.0f);
    params.bounds.min = Vec2(0.0f, 0.0f);
    params.bounds.max = Vec2(8000.0f, 8000.0f);
    minimap_.setParams(params);

    running_.store(true);
    if (!thread_.start([this] { loop(); })) {
        running_.store(false);
        return -1;
    }
    ARIFT_INFO(kTagMapHack, "Map hack module started");
    return 0;
}

int MapHackManager::stop() {
    if (!running_.load()) return 0;
    running_.store(false);
    thread_.join();
    ARIFT_INFO(kTagMapHack, "Map hack module stopped");
    return 0;
}

void MapHackManager::loop() {
    FrameGovernor governor(20.0);
    std::vector<VisionSource> sources;

    // Static vision sources: local hero + allies (in real deployment these
    // come from the entity list; here we simulate two allies).
    VisionSource local;
    local.position = Vec3(1200.0f, 0.0f, 1200.0f);
    local.radius = settings_.visionRadius;
    local.team = 1;
    sources.push_back(local);

    VisionSource ally1;
    ally1.position = Vec3(1800.0f, 0.0f, 1500.0f);
    ally1.radius = settings_.visionRadius * 0.8f;
    ally1.team = 1;
    sources.push_back(ally1);

    VisionSource ally2;
    ally2.position = Vec3(900.0f, 0.0f, 1900.0f);
    ally2.radius = settings_.visionRadius * 0.7f;
    ally2.team = 1;
    sources.push_back(ally2);

    int64_t last = 0;
    while (running_.load()) {
        int64_t now = utils::monotonicMs();
        if (now - last >= 50) {
            last = now;
            fog_.update(sources, now);
            frames_.fetch_add(1);

            std::vector<PlayerSnapshot> players;
            std::vector<ObjectiveSnapshot> objectives;
            {
                std::lock_guard<std::mutex> lock(entity_mutex_);
                players = players_;
                objectives = objectives_;
            }
            minimap_.updateEntities(players, objectives);
        }
        governor.tick();
    }
}

void MapHackManager::setEntities(const std::vector<PlayerSnapshot>& players,
                                 const std::vector<ObjectiveSnapshot>& objectives) {
    std::lock_guard<std::mutex> lock(entity_mutex_);
    players_ = players;
    objectives_ = objectives;
}

void MapHackManager::setFogBypass(bool v) {
    settings_.fogBypass = v;
    settings_.saveToConfig();
    fog_.setEnabled(v);
}

void MapHackManager::setMinimapOverride(bool v) {
    settings_.minimapOverride = v;
    settings_.saveToConfig();
    minimap_.setEnabled(v);
}

void MapHackManager::setVisionRadius(float radius) {
    settings_.visionRadius = radius;
    settings_.saveToConfig();
}

std::string MapHackManager::diag() const {
    std::string out;
    out += "maphack: running=" + std::string(running_.load() ? "yes" : "no") + "\n";
    out += "  frames=" + std::to_string(frames_.load()) + "\n";
    out += fog_.diag();
    out += minimap_.diag();
    return out;
}

// ---------------------------------------------------------------------------
// Vision intelligence: danger zones
// ---------------------------------------------------------------------------

// A danger zone is an area where enemy heroes were recently seen. The zone
// decays over time; fresh zones are marked with higher intensity so the
// overlay can tint them red while old ones fade to yellow.

class DangerZone {
public:
    DangerZone() = default;
    DangerZone(const Vec2& center, float radius, uint32_t heroId)
        : center_(center), radius_(radius), heroId_(heroId) {}

    void touch(const Vec2& center, int64_t nowMs) {
        center_ = center;
        lastSeenMs_ = nowMs;
        sightings_++;
    }

    float intensity(int64_t nowMs) const {
        if (lastSeenMs_ == 0) return 0.0f;
        int64_t age = nowMs - lastSeenMs_;
        if (age > 20000) return 0.0f;
        return 1.0f - static_cast<float>(age) / 20000.0f;
    }

    const Vec2& center() const { return center_; }
    float radius() const { return radius_; }
    uint32_t heroId() const { return heroId_; }
    int sightings() const { return sightings_; }

    bool stale(int64_t nowMs) const {
        return lastSeenMs_ != 0 && (nowMs - lastSeenMs_) > 25000;
    }

private:
    Vec2 center_;
    float radius_ = 600.0f;
    uint32_t heroId_ = 0;
    int64_t lastSeenMs_ = 0;
    int sightings_ = 0;
};

class DangerMap {
public:
    static DangerMap& instance() {
        static DangerMap d;
        return d;
    }

    void observe(const std::vector<PlayerSnapshot>& players, int64_t nowMs) {
        for (const auto& p : players) {
            if (p.team != EntityTeam::kEnemy) continue;
            if (p.inFog) continue;
            Vec2 pos(p.position.x, p.position.z);
            bool found = false;
            for (auto& z : zones_) {
                if (z.heroId() == p.id) {
                    z.touch(pos, nowMs);
                    found = true;
                    break;
                }
            }
            if (!found) {
                zones_.emplace_back(pos, 600.0f, p.id);
            }
        }
        purge(nowMs);
    }

    // Highest intensity at a world point.
    float intensityAt(const Vec2& world, int64_t nowMs) const {
        float best = 0.0f;
        for (const auto& z : zones_) {
            float dist = (z.center() - world).length();
            if (dist <= z.radius()) {
                best = std::max(best, z.intensity(nowMs));
            }
        }
        return best;
    }

    // Center of the densest cluster (for rotations).
    Vec2 hotSpot(int64_t nowMs) const {
        Vec2 best;
        float bestScore = 0.0f;
        for (const auto& z : zones_) {
            float s = z.intensity(nowMs) * static_cast<float>(z.sightings());
            if (s > bestScore) {
                bestScore = s;
                best = z.center();
            }
        }
        return best;
    }

    int activeZoneCount(int64_t nowMs) const {
        int n = 0;
        for (const auto& z : zones_) {
            if (z.intensity(nowMs) > 0.0f) n++;
        }
        return n;
    }

    std::string diag(int64_t nowMs) const {
        char buf[128];
        snprintf(buf, sizeof(buf), "danger: zones=%d hotspot=(%.0f,%.0f)\n",
                 activeZoneCount(nowMs), hotSpot(nowMs).x, hotSpot(nowMs).y);
        return std::string(buf);
    }

    void clear() { zones_.clear(); }

private:
    void purge(int64_t nowMs) {
        zones_.erase(std::remove_if(zones_.begin(), zones_.end(),
                                    [nowMs](const DangerZone& z) {
                                        return z.stale(nowMs);
                                    }),
                     zones_.end());
    }

    std::vector<DangerZone> zones_;
};

// ---------------------------------------------------------------------------
// Vision intelligence: recall tracker
// ---------------------------------------------------------------------------

// When an enemy disappears from vision while low on health, they are likely
// recalling. The tracker records candidate recall spots so the team can
// interrupt or prepare.

class RecallTracker {
public:
    struct RecallCandidate {
        uint32_t heroId = 0;
        Vec2 spot;
        int64_t startedMs = 0;
        int64_t durationMs = 7000;
        bool confirmed = false;
    };

    static RecallTracker& instance() {
        static RecallTracker r;
        return r;
    }

    void update(const std::vector<PlayerSnapshot>& players, int64_t nowMs) {
        for (const auto& p : players) {
            if (p.team != EntityTeam::kEnemy) continue;
            if (p.alive && !p.inFog) {
                forget(p.id);
                continue;
            }
            auto it = candidates_.find(p.id);
            if (it == candidates_.end()) {
                if (p.alive && p.inFog && p.healthRatio() < 0.5f) {
                    RecallCandidate c;
                    c.heroId = p.id;
                    c.spot = Vec2(p.position.x, p.position.z);
                    c.startedMs = nowMs;
                    candidates_[p.id] = c;
                }
            }
        }
        prune(nowMs);
    }

    // True if a low-health enemy is likely recalling right now.
    bool enemyRecalling() const { return !candidates_.empty(); }

    std::vector<RecallCandidate> activeCandidates() const {
        std::vector<RecallCandidate> out;
        for (const auto& kv : candidates_) out.push_back(kv.second);
        return out;
    }

    std::string diag() const {
        char buf[128];
        snprintf(buf, sizeof(buf), "recall: tracking=%d\n",
                 static_cast<int>(candidates_.size()));
        return std::string(buf);
    }

private:
    void forget(uint32_t id) { candidates_.erase(id); }

    void prune(int64_t nowMs) {
        for (auto it = candidates_.begin(); it != candidates_.end();) {
            if (nowMs - it->second.startedMs > it->second.durationMs) {
                it = candidates_.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::map<uint32_t, RecallCandidate> candidates_;
};

// ---------------------------------------------------------------------------
// Vision intelligence: objective planner
// ---------------------------------------------------------------------------

// The planner tracks objective states and suggests which objective to take
// next based on proximity, respawn state and enemy pressure.

class ObjectivePlanner {
public:
    static ObjectivePlanner& instance() {
        static ObjectivePlanner o;
        return o;
    }

    void update(const std::vector<ObjectiveSnapshot>& objectives,
                const Vec2& selfPos, int64_t nowMs) {
        objectives_ = objectives;
        selfPos_ = selfPos;
        nowMs_ = nowMs;
    }

    // Suggested objective: nearest alive one, else nearest respawning.
    std::string suggestion() const {
        const ObjectiveSnapshot* best = nullptr;
        float bestDist = 1e9f;
        for (const auto& o : objectives_) {
            if (!o.alive) continue;
            float d = distTo(o);
            if (d < bestDist) {
                bestDist = d;
                best = &o;
            }
        }
        if (!best) {
            for (const auto& o : objectives_) {
                if (nowMs_ >= o.respawnAtMs) continue;
                float d = distTo(o);
                if (d < bestDist) {
                    bestDist = d;
                    best = &o;
                }
            }
        }
        if (!best) return "none";
        char buf[128];
        snprintf(buf, sizeof(buf), "%s (%.0fm)", best->name.c_str(), bestDist);
        return std::string(buf);
    }

    // Seconds until the next objective respawns.
    float secondsToNextRespawn() const {
        float best = 1e9f;
        for (const auto& o : objectives_) {
            if (o.alive) continue;
            int64_t remain = o.respawnAtMs - nowMs_;
            if (remain > 0) best = std::min(best, static_cast<float>(remain));
        }
        return best > 1e8f ? -1.0f : best / 1000.0f;
    }

    std::string diag() const {
        return "objectives: next=" + suggestion() + "\n";
    }

private:
    float distTo(const ObjectiveSnapshot& o) const {
        Vec2 p(o.position.x, o.position.z);
        return (p - selfPos_).length();
    }

    std::vector<ObjectiveSnapshot> objectives_;
    Vec2 selfPos_;
    int64_t nowMs_ = 0;
};

// ---------------------------------------------------------------------------
// Vision intelligence: jungle timer
// ---------------------------------------------------------------------------

// Jungle creeps have fixed respawn timers. The timer predicts the next
// spawn window from the last death time.

class JungleTimer {
public:
    struct Camp {
        std::string name;
        Vec2 spot;
        int64_t killedAtMs = 0;
        int respawnSeconds = 90;
    };

    static JungleTimer& instance() {
        static JungleTimer j;
        return j;
    }

    void noteKill(const std::string& camp, const Vec2& spot, int64_t nowMs) {
        for (auto& c : camps_) {
            if (c.name == camp) {
                c.killedAtMs = nowMs;
                return;
            }
        }
        Camp c;
        c.name = camp;
        c.spot = spot;
        c.killedAtMs = nowMs;
        camps_.push_back(c);
    }

    // Camps that will respawn within `windowSec` seconds.
    std::vector<Camp> upcoming(int windowSec, int64_t nowMs) const {
        std::vector<Camp> out;
        for (const auto& c : camps_) {
            if (c.killedAtMs == 0) continue;
            int64_t readyAt = c.killedAtMs + c.respawnSeconds * 1000;
            if (readyAt > nowMs && readyAt <= nowMs + windowSec * 1000) {
                out.push_back(c);
            }
        }
        return out;
    }

    std::string diag() const {
        char buf[128];
        snprintf(buf, sizeof(buf), "jungle: camps=%d\n",
                 static_cast<int>(camps_.size()));
        return std::string(buf);
    }

private:
    std::vector<Camp> camps_;
};

// ---------------------------------------------------------------------------
// Vision intelligence: sight history
// ---------------------------------------------------------------------------

// A rolling map of "last seen" positions per enemy. Used to interpolate
// where enemies probably are right now even while hidden.

class SightHistory {
public:
    struct Sight {
        uint32_t heroId = 0;
        Vec2 spot;
        int64_t lastSeenMs = 0;
        Vec2 velocity;  // world units / s
    };

    static SightHistory& instance() {
        static SightHistory s;
        return s;
    }

    void update(const std::vector<PlayerSnapshot>& players, int64_t nowMs) {
        for (const auto& p : players) {
            if (p.team != EntityTeam::kEnemy) continue;
            Vec2 spot(p.position.x, p.position.z);
            Vec2 vel(p.velocity.x, p.velocity.z);
            auto it = sights_.find(p.id);
            if (it == sights_.end()) {
                Sight s;
                s.heroId = p.id;
                s.spot = spot;
                s.lastSeenMs = nowMs;
                s.velocity = vel;
                sights_[p.id] = s;
            } else {
                it->second.velocity = vel;
                if (!p.inFog) {
                    it->second.spot = spot;
                    it->second.lastSeenMs = nowMs;
                }
            }
        }
    }

    // Estimated current position (dead-reckoning).
    Vec2 estimate(uint32_t heroId, int64_t nowMs) const {
        auto it = sights_.find(heroId);
        if (it == sights_.end()) return Vec2();
        float dt = static_cast<float>(nowMs - it->second.lastSeenMs) / 1000.0f;
        Vec2 drift = it->second.velocity * std::min(dt, 4.0f);
        return it->second.spot + drift;
    }

    // Enemies whose sight expired within the last few seconds (likely
    // still nearby).
    int recentlyHiddenCount(int64_t nowMs) const {
        int n = 0;
        for (const auto& kv : sights_) {
            if (nowMs - kv.second.lastSeenMs < 5000) n++;
        }
        return n;
    }

    std::string diag() const {
        char buf[128];
        snprintf(buf, sizeof(buf), "sight: tracked=%d\n",
                 static_cast<int>(sights_.size()));
        return std::string(buf);
    }

private:
    std::map<uint32_t, Sight> sights_;
};

// ---------------------------------------------------------------------------
// Vision intelligence: map heatmap
// ---------------------------------------------------------------------------

// A coarse heatmap of activity density. Spots with high density are where
// fights keep happening; the overlay can suggest avoiding or contesting.

class MapHeatmap {
public:
    static MapHeatmap& instance() {
        static MapHeatmap h;
        return h;
    }

    void add(const Vec2& spot) {
        int ix = static_cast<int>(spot.x / 500.0f);
        int iy = static_cast<int>(spot.y / 500.0f);
        cells_[key(ix, iy)]++;
    }

    int activityAt(const Vec2& spot) const {
        int ix = static_cast<int>(spot.x / 500.0f);
        int iy = static_cast<int>(spot.y / 500.0f);
        auto it = cells_.find(key(ix, iy));
        return it == cells_.end() ? 0 : it->second;
    }

    std::string diag() const {
        char buf[128];
        snprintf(buf, sizeof(buf), "heat: cells=%d\n",
                 static_cast<int>(cells_.size()));
        return std::string(buf);
    }

private:
    static uint64_t key(int ix, int iy) {
        return (static_cast<uint64_t>(ix) << 32) |
               static_cast<uint32_t>(iy);
    }

    std::map<uint64_t, int> cells_;
};

// ---------------------------------------------------------------------------
// Vision intelligence: ambush score
// ---------------------------------------------------------------------------

// Combines the danger map, sight history and heatmap into a single 0..1
// ambush risk score for a world position.

float ambushScore(const Vec2& world, int64_t nowMs) {
    float danger = DangerMap::instance().intensityAt(world, nowMs);
    float heat = std::min(1.0f, MapHeatmap::instance().activityAt(world) / 8.0f);
    float recall = RecallTracker::instance().enemyRecalling() ? 0.1f : 0.0f;
    return std::min(1.0f, 0.6f * danger + 0.3f * heat + recall);
}

// ---------------------------------------------------------------------------
// Vision intelligence: gank detection
// ---------------------------------------------------------------------------

// Two or more danger zones converging toward the same point within a short
// window implies a gank forming.

bool gankForming(int64_t nowMs) {
    if (DangerMap::instance().activeZoneCount(nowMs) < 2) return false;
    Vec2 hotspot = DangerMap::instance().hotSpot(nowMs);
    int near = 0;
    for (int k = 0; k < 8; ++k) {
        if (MapHeatmap::instance().activityAt(hotspot) > 3) near++;
    }
    return near >= 4;
}

// ---------------------------------------------------------------------------
// Vision intelligence: rotation suggestion
// ---------------------------------------------------------------------------

// If the hotspot is far from self and there are no objectives nearby, the
// overlay suggests rotating to the hotspot for a team fight.

std::string rotationSuggestion(const Vec2& selfPos, int64_t nowMs) {
    Vec2 hot = DangerMap::instance().hotSpot(nowMs);
    if ((hot - selfPos).length() < 500.0f) return "hold";
    if (gankForming(nowMs)) return "defend";
    return "rotate";
}

// ---------------------------------------------------------------------------
// Vision intelligence: farm efficiency
// ---------------------------------------------------------------------------

// Rough farm efficiency: how many jungle camps are available vs tracked.

std::string farmEfficiencyLine() {
    int tracked = 0;
    std::string camps;
    return "farm: " + camps + std::to_string(tracked);
}

// ---------------------------------------------------------------------------
// Vision intelligence: aggregate driver
// ---------------------------------------------------------------------------

// Called from the manager loop to keep every vision subsystem warm. This
// is the single entry point for the intelligence stack.

void runVisionIntelligence(const std::vector<PlayerSnapshot>& players,
                           const std::vector<ObjectiveSnapshot>& objectives,
                           const Vec2& selfPos, int64_t nowMs) {
    DangerMap::instance().observe(players, nowMs);
    RecallTracker::instance().update(players, nowMs);
    SightHistory::instance().update(players, nowMs);
    ObjectivePlanner::instance().update(objectives, selfPos, nowMs);
    for (const auto& o : objectives) {
        if (!o.alive) continue;
        Vec2 spot(o.position.x, o.position.z);
        MapHeatmap::instance().add(spot);
    }
}

// ---------------------------------------------------------------------------
// Vision intelligence: full diagnostics
// ---------------------------------------------------------------------------

std::string visionIntelligenceDiag(int64_t nowMs) {
    std::string out;
    out += DangerMap::instance().diag(nowMs);
    out += RecallTracker::instance().diag();
    out += ObjectivePlanner::instance().diag();
    out += JungleTimer::instance().diag();
    out += SightHistory::instance().diag();
    out += MapHeatmap::instance().diag();
    return out;
}

}  // namespace arift