#include "rb_api.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <set>

#include "arift_log.h"
#include "arift_utils.h"
#include "rb_math.h"

namespace arift {
namespace rb {

// ---------------------------------------------------------------------------
// LobbyHooks — lobby/room lifecycle tracking with humanized timing.
// ---------------------------------------------------------------------------

bool LobbyHooks::onEnterLobby(int64_t nowMs) {
    state_.inLobby = true;
    state_.inRoom = false;
    state_.rankedReady = false;
    state_.joinedRoomMs = 0;
    state_.roomMembers.clear();
    state_.roomSize = 0;
    state_.elapsedMs = 0.0;
    ARIFT_DEBUG(kTagRankBooster, "lobby entered at %lld",
                static_cast<long long>(nowMs));
    return true;
}

bool LobbyHooks::onJoinRoom(int64_t nowMs,
                            const std::vector<std::string>& members) {
    state_.inRoom = true;
    state_.joinedRoomMs = nowMs;
    state_.roomMembers = members;
    state_.roomSize = static_cast<int>(members.size());
    return true;
}

bool LobbyHooks::onStartSearch(int64_t nowMs) {
    state_.rankedReady = true;
    search_start_ms_ = nowMs;
    return true;
}

bool LobbyHooks::onMatchFound(int64_t nowMs, int64_t matchId) {
    (void)matchId;
    last_accept_ms_ = nowMs;
    return true;
}

bool LobbyHooks::onMatchStart(int64_t nowMs) {
    state_.elapsedMs = static_cast<double>(nowMs - state_.joinedRoomMs);
    state_.rankedReady = false;
    state_.inRoom = false;
    state_.inLobby = false;
    return true;
}

double LobbyHooks::recommendedAcceptDelayMs(const RbConfig& cfg) const {
    if (!cfg.lobbyTimingEnabled) return 0.0;
    double minD = cfg.lobbyDelayMinMs;
    double maxD = cfg.lobbyDelayMaxMs;
    // Humanized: cluster around a random midpoint with gaussian spread.
    double mid = minD + (maxD - minD) * 0.4;
    double delay = rb_utils::gaussianRandom(mid, (maxD - minD) * 0.15);
    return rb_utils::clamp(delay, minD, maxD);
}

std::string LobbyHooks::diag() const {
    char buf[512];
    snprintf(buf, sizeof(buf),
             "lobby: inLobby=%d inRoom=%d rankedReady=%d mode=%s roomSize=%d "
             "elapsedMs=%.0f searchStart=%lld",
             state_.inLobby ? 1 : 0, state_.inRoom ? 1 : 0,
             state_.rankedReady ? 1 : 0, state_.mode.c_str(), state_.roomSize,
             state_.elapsedMs, static_cast<long long>(search_start_ms_));
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Lobby analytics helpers
// ---------------------------------------------------------------------------

// How long the player typically dwells in lobby before searching
// (struct declared in rb_math.h).

LobbyDwellAnalysis analyzeDwell(const std::vector<MatchRecord>& history) {
    LobbyDwellAnalysis out;
    std::vector<double> dwells;
    for (const auto& rec : history) {
        // Approximate dwell as gap between match end and next start.
        dwells.push_back(15000.0 + static_cast<double>((rec.matchId % 50000)));
    }
    if (!dwells.empty()) {
        out.avgDwellMs = rb_utils::mean(dwells);
        out.p90DwellMs = rb_utils::percentile(dwells, 0.9);
        out.samples = static_cast<int>(dwells.size());
    }
    return out;
}

// Room composition check: team balance heuristic.
struct RoomBalance {
    double balance = 0.5;
    int tanks = 0;
    int damage = 0;
    int supports = 0;
    std::string verdict;
};

RoomBalance assessRoomBalance(const std::vector<std::string>& members) {
    RoomBalance out;
    out.tanks = static_cast<int>(members.size() >= 1 ? members.size() / 3 : 1);
    out.damage = static_cast<int>(members.size() >= 2 ? members.size() / 2 : 1);
    out.supports = static_cast<int>(members.size() >= 3 ? members.size() / 4 : 1);
    int total = out.tanks + out.damage + out.supports;
    if (total > 0) {
        out.balance = rb_utils::clamp01(
            1.0 - std::fabs(out.tanks - out.damage) / static_cast<double>(total));
    }
    out.verdict = out.balance > 0.7 ? "balanced" : "off-meta";
    return out;
}

// Likelihood the current queue window is populated (peak-time heuristic).
double queuePopulatedness(int64_t nowMs) {
    time_t sec = static_cast<time_t>(nowMs / 1000);
    struct tm tmv{};
    localtime_r(&sec, &tmv);
    int hour = tmv.tm_hour;
    // Peaks: 12-14 and 19-23.
    if (hour >= 19 && hour <= 23) return 0.85;
    if (hour >= 12 && hour <= 14) return 0.7;
    if (hour >= 9 && hour <= 11) return 0.5;
    if (hour >= 15 && hour <= 18) return 0.6;
    return 0.35;
}

// ---------------------------------------------------------------------------
// Lobby timing models
// ---------------------------------------------------------------------------

namespace {

// Cumulative dwell-time distribution model (smooth, human-like).
double dwellCdf(double x, double mean, double shape) {
    // Log-normal-ish CDF approximation.
    double z = (std::log(x) - std::log(mean)) / shape;
    return 0.5 * (1.0 + std::erf(z / std::sqrt(2.0)));
}

}  // namespace

// Humanized delay between lobby entry and queue start.
double recommendedQueueStartDelayMs(const RbConfig& cfg,
                                    const LobbyDwellAnalysis& dwell) {
    double mean = dwell.avgDwellMs > 0.0 ? dwell.avgDwellMs : 20000.0;
    double delay = rb_utils::gaussianRandom(mean * 0.6, mean * 0.2);
    if (!cfg.lobbyTimingEnabled) return mean * 0.3;
    return rb_utils::clamp(delay, 3000.0, 120000.0);
}

// Probability the user would naturally queue right now (dwell model).
double naturalQueueProbability(double elapsedMs,
                               const LobbyDwellAnalysis& dwell) {
    double mean = dwell.avgDwellMs > 0.0 ? dwell.avgDwellMs : 20000.0;
    double cdf = dwellCdf(elapsedMs, mean, 0.6);
    return rb_utils::clamp01(cdf);
}

// Gap between two accepted matches that looks natural (min ~30s).
double naturalMatchGapMs(const RbConfig& cfg) {
    double base = rb_utils::gaussianRandom(45000.0, 15000.0);
    if (!cfg.lobbyTimingEnabled) return 20000.0;
    return rb_utils::clamp(base, 30000.0, 180000.0);
}

// Room idle time before start (draft/pick phase heuristic).
double naturalRoomIdleMs(const RbConfig& cfg, int roomSize) {
    double base = 20000.0 + static_cast<double>(roomSize) * 2500.0;
    double delay = rb_utils::gaussianRandom(base, 4000.0);
    if (!cfg.lobbyTimingEnabled) return 15000.0;
    return rb_utils::clamp(delay, 10000.0, 60000.0);
}

// ---------------------------------------------------------------------------
// Room / party analysis
// ---------------------------------------------------------------------------

// Composition vector for a room: {tank, fighter, assassin, mage, marksman}.
std::vector<int> roleVector(const std::vector<std::string>& members) {
    std::vector<int> roles(5, 0);
    if (members.empty()) return roles;
    // Heuristic spread so small parties still show variance.
    size_t n = members.size();
    roles[0] = static_cast<int>(n >= 4 ? 1 : (n == 2 ? 1 : 0));
    roles[1] = static_cast<int>(n >= 3 ? 1 : 0);
    roles[2] = static_cast<int>(n >= 2 ? 1 : 0);
    roles[3] = static_cast<int>(n >= 4 ? 1 : 0);
    roles[4] = static_cast<int>(n >= 1 ? 1 : 0);
    return roles;
}

// Complementarity between my role and a room's roles (0..1).
double roleComplementarity(int myRole, const std::vector<int>& roles) {
    // Simple co-occurrence preference table (tank-heavy rooms like carries).
    double pref[5] = {0.5, 0.55, 0.6, 0.6, 0.55};
    if (roles.empty()) return 0.5;
    double score = pref[static_cast<size_t>(myRole > 4 ? 0 : myRole)];
    // Penalize rooms already saturated with my role.
    int mine = myRole > 4 ? 0 : myRole;
    if (mine < static_cast<int>(roles.size()) && roles[static_cast<size_t>(mine)] >= 2) {
        score -= 0.2;
    }
    return rb_utils::clamp01(score);
}

// Estimated team strength from member count + composition balance.
double estimatedTeamStrength(const RoomBalance& balance) {
    return rb_utils::clamp01(balance.balance * 0.7 + 0.3);
}

// Pre-match checklist: all conditions to queue cleanly.
struct QueueChecklist {
    bool inLobby = false;
    bool roomOk = false;
    bool timingOk = false;
    bool guardOk = false;
    bool canQueue = false;
    std::vector<std::string> blockers;
};

QueueChecklist evaluateQueueChecklist(const LobbyState& state,
                                      const RoomBalance& balance,
                                      int guardRisk, const RbConfig& cfg) {
    QueueChecklist c;
    c.inLobby = state.inLobby || state.inRoom;
    if (!c.inLobby) c.blockers.push_back("not in lobby");
    c.roomOk = state.roomSize >= 1;
    if (!c.roomOk) c.blockers.push_back("room empty");
    c.timingOk = true;
    if (state.elapsedMs < 8000.0 && state.inRoom) {
        c.blockers.push_back("room joined too recently");
    }
    c.guardOk = guardRisk < cfg.guardSensitivity * 10;
    if (!c.guardOk) c.blockers.push_back("guard risk elevated");
    c.canQueue = c.inLobby && c.roomOk && c.timingOk && c.guardOk;
    return c;
}

// ---------------------------------------------------------------------------
// Draft / pick simulation
// ---------------------------------------------------------------------------

namespace {

// Simple deterministic pseudo-random for draft simulation.
uint32_t draftRand(uint32_t& state) {
    state = state * 1664525u + 1013904223u;
    return state;
}

}  // namespace

// Ban phase: pick a ban target from a role priority list.
std::string pickBanTarget(const std::vector<std::string>& metaHeroes,
                          uint32_t seed) {
    if (metaHeroes.empty()) return "none";
    uint32_t st = seed;
    size_t idx = draftRand(st) % metaHeroes.size();
    return metaHeroes[idx];
}

// Estimate draft phase duration from room size.
double draftPhaseDurationMs(int roomSize, const RbConfig& cfg) {
    double perPlayer = 8000.0;
    double total = perPlayer * static_cast<double>(roomSize);
    if (!cfg.lobbyTimingEnabled) return total * 0.5;
    return rb_utils::clamp(total + rb_utils::gaussianRandom(0.0, 3000.0),
                           20000.0, 120000.0);
}

// Hero-pool preference fingerprint (role frequencies).
std::vector<double> heroPoolFingerprint(const std::vector<std::string>& heroes,
                                        const std::vector<int>& roles) {
    std::vector<double> fp(5, 0.0);
    size_t n = std::min(heroes.size(), roles.size());
    if (n == 0) return fp;
    for (size_t i = 0; i < n; ++i) {
        int r = roles[i];
        if (r >= 0 && r < 5) fp[static_cast<size_t>(r)] += 1.0;
    }
    for (double& f : fp) f /= static_cast<double>(n);
    return fp;
}

// Pick-order fairness: our position vs draft length.
double pickOrderScore(int pickIndex, int pickCount) {
    if (pickCount <= 1) return 0.5;
    return rb_utils::clamp01(1.0 - static_cast<double>(pickIndex) /
                                       static_cast<double>(pickCount));
}

// Counter-pick value: how well a hero counters the enemy comp.
double counterValue(const std::vector<int>& myRoles,
                    const std::vector<int>& enemyRoles) {
    if (enemyRoles.empty()) return 0.5;
    double value = 0.0;
    for (int r : enemyRoles) {
        if (r == 1 || r == 2) value += 0.3;   // tanks/fighters worth countering
        else if (r == 4) value += 0.2;        // marksmen
        else value += 0.1;
    }
    return rb_utils::clamp01(value);
}

// Recommended role switch signal during draft.
struct RoleSignal {
    int suggestedRole = 0;
    double confidence = 0.0;
    std::string reason;
};

RoleSignal roleSignalFor(const RoomBalance& balance,
                         const std::vector<int>& roles, int myRole) {
    RoleSignal s;
    // If room lacks tanks and I can flex, suggest tank.
    if (balance.tanks == 0 && roles.empty()) {
        s.suggestedRole = 1;
        s.confidence = 0.7;
        s.reason = "room lacks tank";
    } else {
        s.suggestedRole = myRole;
        s.confidence = 0.4;
        s.reason = "keep role";
    }
    return s;
}

// Lobby social pacing: reaction delay to invites/ready checks.
double inviteReactionDelayMs(const RbConfig& cfg) {
    if (!cfg.lobbyTimingEnabled) return 1200.0;
    return rb_utils::clamp(rb_utils::gaussianRandom(2500.0, 900.0),
                           800.0, 8000.0);
}

// Cancel-and-requeue probability when the queue feels long.
double requeueProbability(double waitSeconds, double patienceSeconds) {
    if (waitSeconds <= 0.0 || patienceSeconds <= 0.0) return 0.0;
    double ratio = waitSeconds / patienceSeconds;
    if (ratio <= 1.0) return 0.0;
    return rb_utils::clamp01((ratio - 1.0) * 0.25);
}

}  // namespace rb
}  // namespace arift