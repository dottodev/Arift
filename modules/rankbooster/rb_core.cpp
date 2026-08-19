#include "rb_api.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>

#include "arift_log.h"
#include "arift_thread.h"
#include "arift_utils.h"
#include "rb_math.h"

namespace arift {
namespace rb {

// ---------------------------------------------------------------------------
// RankBooster — core manager wiring every booster subsystem together.
// ---------------------------------------------------------------------------

RankBooster::RankBooster()
    : telemetry_(RbTelemetry::instance()),
      cache_(RbCache::instance()),
      engine_(RbEngine::instance()) {
    ARIFT_INFO(kTagRankBooster, "RankBooster constructed");
}

RankBooster::~RankBooster() {
    stop();
}

RankBooster& RankBooster::instance() {
    static RankBooster b;
    return b;
}

int RankBooster::start() {
    if (running_.load()) return 0;
    config_ = RbConfigStore::instance().config();

    running_.store(true);
    state_.store(BoosterState::kCollecting);
    thread_.start([this]() { runLoop(); });
    ARIFT_INFO(kTagRankBooster, "RankBooster started");
    return 0;
}

int RankBooster::stop() {
    if (!running_.load()) return 0;
    running_.store(false);
    state_.store(BoosterState::kIdle);
    thread_.join();
    ARIFT_INFO(kTagRankBooster, "RankBooster stopped");
    return 0;
}

void RankBooster::setEnabled(bool enabled) {
    config_.enabled = enabled;
    RbConfigStore::instance().save();
}

void RankBooster::setAggression(int level) {
    config_.aggression = static_cast<AggressionLevel>(
        rb_utils::clampInt(level, static_cast<int>(AggressionLevel::kConservative),
                           static_cast<int>(AggressionLevel::kExtreme)));
    RbConfigStore::instance().save();
}

void RankBooster::setTargetRank(int rankAbs) {
    config_.targetTier = static_cast<RankTier>(
        rb_utils::clampInt(rankAbs / 100, 0,
                           static_cast<int>(RankTier::kCount) - 1));
    config_.targetStars = rankAbs % 100;
    RbConfigStore::instance().save();
}

void RankBooster::pump() {
    int64_t now = utils::monotonicMs();
    if (now - last_pump_ms_ < 250) return;
    last_pump_ms_ = now;

    // Periodic tuning refresh.
    if (config_.tuneIntervalSec > 0.0 &&
        now - last_tune_ms_ >
            static_cast<int64_t>(config_.tuneIntervalSec * 1000.0)) {
        last_tune_ms_ = now;
        stats_.compute(profile_store_.local(), profile_store_.history());
        TunerResult tr =
            tuner_.tune(profile_store_.local(), stats_, config_,
                        config_.aggression);
        if (tr.changed) {
            config_ = tr.suggested;
            RbConfigStore::instance().save();
        }
        telemetry_.recordTunerRun(now);
        ARIFT_DEBUG(kTagRankBooster, "tune: %s", tr.rationale.c_str());
    }

    // Guard sweep.
    if (guard_.riskLevel(config_)) {
        state_.store(BoosterState::kPaused);
        telemetry_.recordGuardEvent(now);
    } else if (state_.load() == BoosterState::kPaused) {
        state_.store(BoosterState::kReady);
    }

    // Scheduler check.
    if (!scheduler_.inActiveWindow(now)) {
        state_.store(BoosterState::kIdle);
    }
}

void RankBooster::onMatchEnded(const MatchResult& result) {
    PlayerProfile& p = profile_store_.local();

    MatchRecord rec;
    rec.matchId = utils::monotonicMs();
    rec.startedAtMs = p.lastMatchMs > 0 ? p.lastMatchMs
                                        : utils::monotonicMs() - 900000;
    rec.endedAtMs = result.endedAtMs > 0 ? result.endedAtMs
                                         : utils::monotonicMs();
    rec.result = result;
    rec.rankBefore = p.rank;
    rec.mode = result.ranked ? "ranked" : "classic";

    // Elo + MMR update against an estimated opponent.
    double oppRating = p.mmr +
                       (result.won ? -rb_utils::gaussianRandom(60.0, 30.0)
                                   : rb_utils::gaussianRandom(60.0, 30.0));
    EloResult er = elo_.apply(p.mmr, oppRating, result.won,
                              static_cast<int>(p.streak), p.protectedLoss);
    MMRUpdate mu = mmr_.update(p, oppRating, 100.0, result.won, 0.5);

    rec.mmrDelta = mu.delta;
    rec.skillRating = mu.newRating;
    rec.confidence = mmr_.confidenceOf(mu.newSigma);

    p.mmr = mu.newRating;
    p.sigma = mu.newSigma;
    p.volatility = mu.newVolatility;
    p.rank = elo_.advanceRank(p.rank, er.starDelta);
    p.streak = result.won ? (p.streak > 0 ? p.streak + 1 : 1)
                          : (p.streak < 0 ? p.streak - 1 : -1);
    if (result.won) {
        p.matchesWon += 1;
        p.protectedLoss = true;
    } else if (p.protectedLoss) {
        p.protectedLoss = false;
    }
    p.matchesPlayed += 1;
    p.winRate = static_cast<double>(p.matchesWon) /
                static_cast<double>(p.matchesPlayed);
    p.performanceIndex = rb_utils::damp(
        p.performanceIndex,
        static_cast<double>(result.performancePercentile), 0.3, 1.0);

    rec.rankAfter = p.rank;
    profile_store_.recordMatch(rec);
    telemetry_.recordMatch(rec);
    stats_.compute(p, profile_store_.history());

    ARIFT_INFO(kTagRankBooster, "match recorded: won=%d delta=%.1f rank=%s",
               result.won ? 1 : 0, mu.delta, p.rank.toString().c_str());
}

std::string RankBooster::snapshot() const {
    const PlayerProfile& p = profile_store_.local();
    char buf[1024];
    snprintf(buf, sizeof(buf),
             "booster: state=%d enabled=%d rank=%s mmr=%.0f sigma=%.0f "
             "wr=%.1f%% streak=%lld matches=%lld\n"
             "stats: %s",
             static_cast<int>(state_.load()), config_.enabled ? 1 : 0,
             p.rank.toString().c_str(), p.mmr, p.sigma,
             p.winRate * 100.0, static_cast<long long>(p.streak),
             static_cast<long long>(p.matchesPlayed),
             stats_.toString().c_str());
    return std::string(buf);
}

std::string RankBooster::diag() const {
    std::string out;
    out += lobby_.diag();
    out += "\n";
    out += guard_.dump();
    out += telemetry_.dump();
    out += "\n";
    out += "cache entries=" + std::to_string(cache_.size());
    out += " bytes=" + std::to_string(cache_.bytesStored());
    return out;
}

void RankBooster::runLoop() {
    while (running_.load()) {
        pump();
        Thread::sleepMs(200);
    }
}

// ---------------------------------------------------------------------------
// Session / match lifecycle helpers
// ---------------------------------------------------------------------------

// Apply the active configuration to the lobby hooks + scheduler windows.
void RankBooster::applyConfigToComponents() {
    RbConfig& cfg = config_;

    // Scheduler budget from aggression.
    int budget = 6;
    switch (cfg.aggression) {
        case AggressionLevel::kConservative: budget = 4; break;
        case AggressionLevel::kBalanced: budget = 6; break;
        case AggressionLevel::kAggressive: budget = 9; break;
        case AggressionLevel::kExtreme: budget = 12; break;
    }
    scheduler_.setSessionBudget(budget);

    // Lobby timing guards.
    cfg.lobbyDelayMinMs =
        rb_utils::clamp(cfg.lobbyDelayMinMs, 300.0, 10000.0);
    cfg.lobbyDelayMaxMs =
        rb_utils::clamp(cfg.lobbyDelayMaxMs, cfg.lobbyDelayMinMs, 30000.0);

    // Cache the effective tuning snapshot.
    std::string key = cacheKey("tuning", "effective");
    std::string val;
    if (cache_.get(key, val)) {
        // Reuse cached tuning if fresh.
        ARIFT_DEBUG(kTagRankBooster, "reusing cached tuning: %s",
                    val.c_str());
    } else {
        cache_.put(key, cfg.aggression == AggressionLevel::kExtreme
                           ? "extreme" : "balanced", 300000);
    }
}

// Build a queue request honoring the active configuration.
MatchmakingRequest RankBooster::makeQueueRequest() const {
    MatchmakingRequest req;
    req.mode = "ranked";
    req.preferredRole = config_.preferredRoleEnabled
                            ? config_.preferredRole : 0;
    req.maxQueueSeconds = static_cast<int>(config_.maxQueueSeconds);
    req.partySize = config_.partyAssistEnabled ? 2 : 1;
    req.desiredDifficulty = tuner_.recommendedQueueDifficulty(
        profile_store_.local(), stats_, config_);
    req.regionBias = 0;
    req.useWideSearch = config_.adaptiveDifficulty;
    return req;
}

// Kick off a queue cycle through the engine.
int RankBooster::startQueueCycle() {
    if (!config_.enabled) return -1;
    if (state_.load() != BoosterState::kReady &&
        state_.load() != BoosterState::kActive) {
        return -2;
    }
    if (guard_.riskLevel(config_)) {
        ARIFT_WARN(kTagRankBooster, "queue blocked by guard risk=%d",
                   guard_.currentRisk());
        return -3;
    }

    MatchmakingRequest req = makeQueueRequest();
    queue_req_ = req;
    MatchmakingEngine& mm = matchmaking_;
    QueueSnapshot qs = mm.beginQueue(req);
    telemetry_.recordQueueJoin(utils::monotonicMs());
    state_.store(BoosterState::kActive);
    last_queue_start_ms_ = utils::monotonicMs();
    ARIFT_INFO(kTagRankBooster, "queue cycle started (wait %.0fs)",
               qs.estimatedWaitSec);
    return 0;
}

// Poll the matchmaking engine; returns 1 when a match resolved.
int RankBooster::pollQueueCycle() {
    if (!matchmaking_.isSearching()) return 0;
    QueueSnapshot qs = matchmaking_.pollQueue();
    if (qs.found) {
        int64_t now = utils::monotonicMs();
        // Humanized accept delay before we "confirm".
        double delay = lobby_.recommendedAcceptDelayMs(config_);
        engine_delay_ms_ = static_cast<int64_t>(delay);
        lobby_.onMatchFound(now + engine_delay_ms_, qs.matchId);
        ARIFT_INFO(kTagRankBooster, "match found id=%lld (accept in %.0f ms)",
                   static_cast<long long>(qs.matchId), delay);
        return 1;
    }
    return 0;
}

// Cancel an in-flight queue cycle.
void RankBooster::cancelQueueCycle() {
    if (!matchmaking_.isSearching()) return;
    matchmaking_.cancelQueue();
    if (state_.load() == BoosterState::kActive) {
        state_.store(BoosterState::kReady);
    }
    ARIFT_DEBUG(kTagRankBooster, "queue cycle cancelled");
}

// Guard sweep: evaluate current behavior signals; pause if risky.
void RankBooster::guardSweep() {
    const PlayerProfile& p = profile_store_.local();
    const auto& history = profile_store_.history();

    bool flag = false;
    RiskEvent ev;
    ev.atMs = utils::monotonicMs();

    if (guard_.checkMatchPattern(history, config_)) {
        ev.code = 0x51;
        ev.severity = 4;
        ev.message = "match pattern anomaly";
        flag = true;
    } else if (guard_.checkTimingPattern(history, config_)) {
        ev.code = 0x52;
        ev.severity = 3;
        ev.message = "timing pattern anomaly";
        flag = true;
    } else if (guard_.checkPerformanceOutlier(p, config_)) {
        ev.code = 0x53;
        ev.severity = 3;
        ev.message = "performance outlier";
        flag = true;
    } else if (extremeStreakPattern(history, 6)) {
        ev.code = 0x54;
        ev.severity = 2;
        ev.message = "extreme streak pattern";
        flag = true;
    }

    if (flag) {
        guard_.recordEvent(ev);
        telemetry_.recordGuardEvent(ev.atMs);
        if (config_.autoPauseOnRisk) {
            state_.store(BoosterState::kPaused);
            ARIFT_WARN(kTagRankBooster, "guard paused booster: %s",
                       ev.message.c_str());
        }
    }
}

// Record a finished match through the full pipeline.
void RankBooster::processMatchEnd(const MatchResult& result) {
    onMatchEnded(result);
    telemetry_.recordSessionEnd(utils::monotonicMs());
    engine_.onMatchFinished(result);

    // Cache the latest match digest.
    std::string digest =
        std::to_string(result.won ? 1 : 0) + "|" +
        std::to_string(result.performancePercentile) + "|" +
        std::to_string(result.durationMin);
    cache_.put(cacheKey("match", "last"), digest, 3600000);
}

// Session start: prime components + telemetry.
void RankBooster::beginSession() {
    applyConfigToComponents();
    engine_.beginSession();
    telemetry_.recordSessionStart(utils::monotonicMs());
    if (state_.load() == BoosterState::kIdle ||
        state_.load() == BoosterState::kPaused) {
        state_.store(BoosterState::kReady);
    }
    ARIFT_INFO(kTagRankBooster, "session begun");
}

// Session end: flush caches + persist profile.
void RankBooster::endSession() {
    engine_.endSession();
    telemetry_.recordSessionEnd(utils::monotonicMs());
    cache_.flush();
    profile_store_.save(base_dir_);
    state_.store(BoosterState::kIdle);
    ARIFT_INFO(kTagRankBooster, "session ended, profile saved");
}

// Load the persisted profile from disk.
bool RankBooster::loadProfile(const std::string& baseDir) {
    base_dir_ = baseDir;
    cache_.setBaseDir(baseDir);
    bool ok = profile_store_.load(baseDir);
    stats_.compute(profile_store_.local(), profile_store_.history());
    return ok;
}

// ---------------------------------------------------------------------------
// Integration helpers (used by JNI / FeatureSwitch wiring)
// ---------------------------------------------------------------------------

// Full status blob for the overlay: config + stats + engine + guard.
std::string RankBooster::statusBlob() const {
    std::string out;
    out += config_.toString();
    out += "\n";
    out += stats_.toString();
    out += "\n";
    out += snapshot();
    out += "\n";
    out += telemetry_.dump();
    return out;
}

// Quick win-rate snapshot (for the INGAME tab badge).
double RankBooster::currentWinRate() const {
    return stats_.winRate;
}

// Set the booster data directory and load persisted state.
bool RankBooster::initData(const std::string& baseDir) {
    return loadProfile(baseDir);
}

// Flush all persistent state (call before process exit).
void RankBooster::flushAll() {
    profile_store_.save(base_dir_);
    cache_.flush();
    RbConfigStore::instance().save();
}

// Force a guard evaluation now (returns new risk level).
int RankBooster::evaluateGuardNow() {
    guardSweep();
    return guard_.currentRisk();
}

// Register a lobby event from the game-side hooks.
void RankBooster::onLobbyEvent(int code, const std::vector<std::string>& members) {
    int64_t now = utils::monotonicMs();
    switch (code) {
        case 1:  // entered lobby
            lobby_.onEnterLobby(now);
            break;
        case 2:  // joined room
            lobby_.onJoinRoom(now, members);
            break;
        case 3:  // started search
            lobby_.onStartSearch(now);
            break;
        case 4:  // match found
            lobby_.onMatchFound(now, 0);
            break;
        case 5:  // match started
            lobby_.onMatchStart(now);
            break;
        default:
            break;
    }
}

// Register a match result from game-side hooks (full pipeline).
void RankBooster::onGameMatchEnded(const MatchResult& result) {
    processMatchEnd(result);
}

// Predicted difficulty for the next queue cycle (0..1).
double RankBooster::predictedDifficulty() const {
    return tuner_.recommendedQueueDifficulty(
        profile_store_.local(), stats_, config_);
}

// Remaining session budget.
int RankBooster::sessionMatchesLeft() const {
    return scheduler_.matchesRemaining(utils::monotonicMs());
}

// Star projection to the configured target.
std::string RankBooster::projectionText() const {
    StarEconomy econ = economyForTier(config_.targetTier);
    StarProjection proj = projectStars(profile_store_.local(), stats_,
                                       RankPoint{config_.targetTier,
                                                 config_.targetStars, 0, 0},
                                       econ);
    return proj.text;
}

// Playstyle label for the current AI profile.
std::string RankBooster::playstyleLabel() const {
    AiProfile prof = ai_.buildProfile(profile_store_.local(), stats_);
    double idx = ai_.recommendedPlaystyleIndex(prof, config_);
    return ai_.playstyleLabel(idx);
}

// ---------------------------------------------------------------------------
// Diagnostics and monitoring helpers
// ---------------------------------------------------------------------------

// One-line status for the overlay HUD.
std::string RankBooster::hudLine() const {
    char buf[256];
    snprintf(buf, sizeof(buf),
             "RB %s | wr=%.0f%% s=%d m=%d | guard=%d | sess=%d",
             config_.enabled ? "ON" : "OFF", stats_.winRate * 100.0,
             static_cast<int>(stats_.currentStreak),
             static_cast<int>(stats_.totalMatches),
             guard_.currentRisk(), sessionMatchesLeft());
    return std::string(buf);
}

// Whether a queue attempt should happen this cycle.
bool RankBooster::shouldAttemptQueueNow() const {
    if (!config_.enabled) return false;
    if (guard_.currentRisk() >= config_.guardSensitivity * 10) return false;
    int64_t now = utils::monotonicMs();
    if (scheduler_.matchesRemaining(now) <= 0) return false;
    if (now - last_queue_start_ms_ < 30000) return false;
    return true;
}

// Star economy for the configured target tier.
StarEconomy RankBooster::currentEconomy() const {
    return economyForTier(config_.targetTier);
}

// Estimated sessions until the target rank (integer).
int RankBooster::sessionsToTargetNow() const {
    RankPoint current{profile_store_.local().rank.tier,
                      profile_store_.local().rank.stars, 0, 0};
    RankPoint target{config_.targetTier, config_.targetStars, 0, 0};
    int stars = std::max(0, target.absolute() - current.absolute());
    if (stars <= 0) return 0;
    double per = std::max(0.25, stats_.expectedStarsPerMatch * 8.0);
    return static_cast<int>(std::ceil(stars / per));
}

// Guard risk band as text.
const char* RankBooster::guardBand() const {
    int r = guard_.currentRisk();
    if (r >= 75) return "CRITICAL";
    if (r >= 50) return "HIGH";
    if (r >= 25) return "MODERATE";
    if (r > 0) return "LOW";
    return "CLEAR";
}

// Effective star gain this week (net).
double RankBooster::weeklyStarDelta() const {
    int64_t cutoff = utils::monotonicMs() - 7LL * 86400000;
    double delta = 0.0;
    for (const auto& rec : profile_store_.history()) {
        if (rec.endedAtMs >= cutoff) delta += rec.mmrDelta * 0.01;
    }
    return delta;
}

// ---------------------------------------------------------------------------
// Session profile recorder
// ---------------------------------------------------------------------------

// Records the shape of every play session: start time, duration, how many
// matches, win/loss cadence and the MMR path through the session.

struct SessionProfile {
    int64_t sessionId = 0;
    int64_t beganMs = 0;
    int64_t endedMs = 0;
    int matches = 0;
    int wins = 0;
    int losses = 0;
    double mmrStart = 0.0;
    double mmrEnd = 0.0;
    int bestStreak = 0;
    int worstStreak = 0;
    double avgPerformance = 50.0;
    double durationMin = 0.0;

    double mmrDelta() const { return mmrEnd - mmrStart; }
    double winRate() const {
        if (matches == 0) return 0.0;
        return static_cast<double>(wins) / static_cast<double>(matches);
    }
};

class SessionRecorder {
public:
    static SessionRecorder& instance() {
        static SessionRecorder r;
        return r;
    }

    void begin(int64_t nowMs, double mmr) {
        if (active_) return;
        active_ = true;
        cur_.sessionId = utils::random32();
        cur_.beganMs = nowMs;
        cur_.mmrStart = mmr;
        cur_.mmrEnd = mmr;
    }

    void noteMatch(const MatchRecord& rec) {
        if (!active_) return;
        cur_.matches++;
        if (rec.result.won) {
            cur_.wins++;
            cur_.bestStreak = std::max(cur_.bestStreak, 1);
        } else {
            cur_.losses++;
        }
        cur_.mmrEnd = rec.mmrDelta + cur_.mmrEnd;
        cur_.avgPerformance =
            (cur_.avgPerformance * (cur_.matches - 1) +
             rec.result.performancePercentile) /
            cur_.matches;
    }

    void end(int64_t nowMs) {
        if (!active_) return;
        cur_.endedMs = nowMs;
        cur_.durationMin =
            static_cast<double>(nowMs - cur_.beganMs) / 60000.0;
        history_.push_back(cur_);
        if (history_.size() > 50) {
            history_.erase(history_.begin());
        }
        active_ = false;
    }

    bool active() const { return active_; }
    const SessionProfile& current() const { return cur_; }

    // Last n sessions, newest first.
    std::vector<SessionProfile> recent(int n) const {
        std::vector<SessionProfile> out;
        int start = std::max(0, static_cast<int>(history_.size()) - n);
        for (size_t i = start; i < history_.size(); ++i) {
            out.push_back(history_[i]);
        }
        return out;
    }

    // Average session length over the last 10 sessions (minutes).
    double avgSessionLengthMin() const {
        auto s = recent(10);
        if (s.empty()) return 0.0;
        double sum = 0.0;
        for (const auto& x : s) sum += x.durationMin;
        return sum / s.size();
    }

    std::string diag() const {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "sessions: active=%d recent=%d avg_len=%.1fmin\n",
                 active_ ? 1 : 0, static_cast<int>(history_.size()),
                 avgSessionLengthMin());
        return std::string(buf);
    }

private:
    bool active_ = false;
    SessionProfile cur_;
    std::vector<SessionProfile> history_;
};

// ---------------------------------------------------------------------------
// Performance analytics
// ---------------------------------------------------------------------------

// Tracks a rolling performance index: how well the last matches went
// relative to the player's own average. Used by the advisor to decide
// whether to keep queueing.

struct PerformanceRolling {
    int samples = 0;
    double sum = 0.0;
    double sumSq = 0.0;
    int64_t firstSampleMs = 0;
    int64_t lastSampleMs = 0;

    double mean() const {
        if (samples == 0) return 50.0;
        return sum / samples;
    }
    double stddev() const {
        if (samples < 2) return 0.0;
        double m = mean();
        double v = (sumSq - 2.0 * m * sum + samples * m * m) / samples;
        return std::sqrt(std::max(0.0, v));
    }
};

class PerformanceTracker {
public:
    static PerformanceTracker& instance() {
        static PerformanceTracker t;
        return t;
    }

    void add(int percentile, int64_t nowMs) {
        samples_.samples++;
        samples_.sum += percentile;
        samples_.sumSq += static_cast<double>(percentile) * percentile;
        if (samples_.firstSampleMs == 0) samples_.firstSampleMs = nowMs;
        samples_.lastSampleMs = nowMs;
        if (samples_.samples > 60) {
            // Drop the oldest contribution (approximate: halve window).
            samples_.sum /= 2.0;
            samples_.sumSq /= 2.0;
            samples_.samples = 30;
        }
    }

    double current() const { return samples_.mean(); }
    double volatility() const { return samples_.stddev(); }

    // Trend over the last few samples: +improving, -declining.
    double trend(int64_t nowMs) const {
        if (samples_.samples < 4) return 0.0;
        return (samples_.mean() - 50.0) * 0.1;
    }

    std::string diag() const {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "perf: mean=%.1f sigma=%.1f samples=%d trend=%.1f\n",
                 current(), volatility(), samples_.samples,
                 trend(samples_.lastSampleMs));
        return std::string(buf);
    }

private:
    PerformanceRolling samples_;
};

// ---------------------------------------------------------------------------
// Streak intelligence
// ---------------------------------------------------------------------------

// Models the current win/loss streak and how it interacts with the
// matchmaker. Long streaks invite the algorithm to change difficulty, so
// the advisor knows when the streak is "primed" to break.

struct StreakModel {
    int64_t current = 0;        // +win / -loss
    int best = 0;
    int worst = 0;
    int64_t lastChangeMs = 0;
    double avgMmrDuringStreak = 0.0;
    int matchesInStreak = 0;
};

class StreakTracker {
public:
    static StreakTracker& instance() {
        static StreakTracker s;
        return s;
    }

    void note(bool won, double mmrDelta, int64_t nowMs) {
        int64_t sign = won ? 1 : -1;
        if (streak_.current == 0 || sign == (streak_.current > 0 ? 1 : -1)) {
            streak_.current += sign;
        } else {
            streak_.current = sign;
            streak_.matchesInStreak = 0;
        }
        streak_.matchesInStreak++;
        streak_.lastChangeMs = nowMs;
        streak_.avgMmrDuringStreak =
            (streak_.avgMmrDuringStreak * (streak_.matchesInStreak - 1) +
             mmrDelta) /
            streak_.matchesInStreak;
        streak_.best = std::max(streak_.best, static_cast<int>(streak_.current));
        streak_.worst = std::min(streak_.worst, static_cast<int>(streak_.current));
    }

    const StreakModel& current() const { return streak_; }

    // True when the streak is long enough that the matchmaker is likely
    // to adjust difficulty.
    bool primedToBreak() const {
        return std::abs(streak_.current) >= 4;
    }

    // Risk of continuing: a long win streak followed by a loss usually
    // opens a losing counter-streak.
    double continuationRisk() const {
        if (streak_.current <= 1) return 0.0;
        double len = static_cast<double>(streak_.current);
        return std::min(0.8, (len - 1.0) * 0.15);
    }

    std::string diag() const {
        char buf[192];
        snprintf(buf, sizeof(buf), "streak: cur=%lld best=%d worst=%d risk=%.2f\n",
                 static_cast<long long>(streak_.current), streak_.best,
                 streak_.worst, continuationRisk());
        return std::string(buf);
    }

private:
    StreakModel streak_;
};

// ---------------------------------------------------------------------------
// Queue timing advisor
// ---------------------------------------------------------------------------

// The matchmaker behaves differently by time of day and by queue length.
// The advisor learns the best queue windows from history and scores the
// current moment.

struct QueueWindowScore {
    double score = 0.0;         // 0..1
    const char* label = "neutral";
    int waitSeconds = 30;
    bool primeTime = false;
};

class QueueAdvisor {
public:
    static QueueAdvisor& instance() {
        static QueueAdvisor q;
        return q;
    }

    // Score the current moment (hour 0..23).
    QueueWindowScore score(int hour, int dayOfWeek) {
        QueueWindowScore s;
        s.waitSeconds = 30 + static_cast<int>(utils::random32() % 40);
        if (dayOfWeek >= 5 && hour >= 18 && hour <= 22) {
            s.score = 0.9;
            s.label = "prime";
            s.primeTime = true;
        } else if (hour >= 12 && hour <= 14) {
            s.score = 0.6;
            s.label = "good";
        } else if (hour >= 21 && hour <= 23) {
            s.score = 0.5;
            s.label = "fair";
        } else if (hour >= 1 && hour <= 6) {
            s.score = 0.2;
            s.label = "quiet";
        } else {
            s.score = 0.4;
            s.label = "ok";
        }
        return s;
    }

    std::string diag(int hour, int dayOfWeek) {
        QueueWindowScore s = score(hour, dayOfWeek);
        char buf[192];
        snprintf(buf, sizeof(buf), "queue: %s score=%.1f wait=%ds\n",
                 s.label, s.score, s.waitSeconds);
        return std::string(buf);
    }

private:
};

// ---------------------------------------------------------------------------
// Hero / role fit analyzer
// ---------------------------------------------------------------------------

// Some heroes fit a player's style better than others. The analyzer keeps
// a per-hero performance table and recommends heroes for the next match.

struct HeroRecord {
    std::string hero;
    std::string role;
    int games = 0;
    int wins = 0;
    double avgKda = 0.0;
    double avgPerformance = 50.0;
    int64_t lastPlayedMs = 0;

    double winRate() const {
        if (games == 0) return 0.0;
        return static_cast<double>(wins) / games;
    }
};

class HeroAdvisor {
public:
    static HeroAdvisor& instance() {
        static HeroAdvisor h;
        return h;
    }

    void noteMatch(const std::string& hero, bool won, double kda,
                   int performance, int64_t nowMs) {
        auto& rec = table_[hero];
        rec.hero = hero;
        rec.games++;
        if (won) rec.wins++;
        rec.avgKda = (rec.avgKda * (rec.games - 1) + kda) / rec.games;
        rec.avgPerformance =
            (rec.avgPerformance * (rec.games - 1) + performance) / rec.games;
        rec.lastPlayedMs = nowMs;
    }

    // Best hero by win rate with at least 3 games.
    std::string bestHero() const {
        std::string best;
        double bestRate = 0.0;
        for (const auto& kv : table_) {
            if (kv.second.games < 3) continue;
            if (kv.second.winRate() > bestRate) {
                bestRate = kv.second.winRate();
                best = kv.second.hero;
            }
        }
        return best.empty() ? "any" : best;
    }

    // Heroes that are overdue for a game (favorites decaying).
    std::vector<std::string> staleFavorites(int64_t nowMs) const {
        std::vector<std::string> out;
        for (const auto& kv : table_) {
            if (kv.second.games >= 5 &&
                nowMs - kv.second.lastPlayedMs > 3LL * 86400000) {
                out.push_back(kv.second.hero);
            }
        }
        return out;
    }

    std::string diag() const {
        char buf[192];
        snprintf(buf, sizeof(buf), "heroes: tracked=%d best=%s\n",
                 static_cast<int>(table_.size()), bestHero().c_str());
        return std::string(buf);
    }

private:
    std::map<std::string, HeroRecord> table_;
};

// ---------------------------------------------------------------------------
// Match review summarizer
// ---------------------------------------------------------------------------

// Builds the human-readable review card shown after each match, turning
// the raw record into actionable text.

std::string reviewLine(const MatchRecord& rec) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s %s | mmr %+.1f | perf %d%% | %.1fmin",
             rec.result.won ? "WIN" : "LOSS",
             rec.mode.c_str(), rec.mmrDelta,
             rec.result.performancePercentile, rec.result.durationMin);
    return std::string(buf);
}

std::string reviewKda(const MatchResult& r) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%d/%d/%d", r.kdaKills, r.kdaDeaths,
             r.kdaAssists);
    return std::string(buf);
}

// Full session review block.
std::string sessionReviewBlock(const SessionProfile& s) {
    std::string out;
    char buf[384];
    snprintf(buf, sizeof(buf),
             "session %lld: %d matches, %dW/%dL (%.0f%%)\n"
             "  mmr %+.1f | best streak %d | avg perf %.1f\n",
             static_cast<long long>(s.sessionId), s.matches, s.wins,
             s.losses, s.winRate() * 100.0, s.mmrDelta(), s.bestStreak,
             s.avgPerformance);
    out += buf;
    return out;
}

// ---------------------------------------------------------------------------
// Ladder progress tracker
// ---------------------------------------------------------------------------

// Models the ladder climb: how many stars to the next tier, the pace
// needed and whether the current pace is sustainable.

struct LadderProgress {
    RankTier tier = RankTier::kWarrior;
    int stars = 0;
    int starsToNext = 5;
    double starsPerSession = 0.0;
    double sessionsToNext = 0.0;
    double estimatedDays = 0.0;
    bool paceSustainable = true;
};

class LadderTracker {
public:
    static LadderTracker& instance() {
        static LadderTracker l;
        return l;
    }

    void update(const RankPoint& rank) {
        rank_ = rank;
        int tierStars = 5;
        int need = std::max(1, tierStars - rank_.stars);
        starsToNext_ = need;
    }

    // Recompute pace from session history.
    void recomputePace(const std::vector<SessionProfile>& sessions) {
        if (sessions.empty()) return;
        double totalStars = 0.0;
        for (const auto& s : sessions) {
            totalStars += s.mmrDelta() * 0.01;
        }
        double perSession = totalStars / sessions.size();
        starsPerSession_ = perSession;
        if (perSession <= 0.0) {
            sessionsToNext_ = 999.0;
        } else {
            sessionsToNext_ = starsToNext_ / perSession;
        }
        estimatedDays_ = sessionsToNext_ * 0.6;
        paceSustainable_ = sessionsToNext_ < 40.0;
    }

    LadderProgress progress() const {
        LadderProgress p;
        p.tier = rank_.tier;
        p.stars = rank_.stars;
        p.starsToNext = starsToNext_;
        p.starsPerSession = starsPerSession_;
        p.sessionsToNext = sessionsToNext_;
        p.estimatedDays = estimatedDays_;
        p.paceSustainable = paceSustainable_;
        return p;
    }

    std::string diag() const {
        LadderProgress p = progress();
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "ladder: %s %d/5 stars, %.1f sessions to next, "
                 "%.1f days (pace %s)\n",
                 tierCode(p.tier), p.stars, p.sessionsToNext,
                 p.estimatedDays, p.paceSustainable ? "ok" : "tight");
        return std::string(buf);
    }

private:
    RankPoint rank_;
    int starsToNext_ = 5;
    double starsPerSession_ = 0.0;
    double sessionsToNext_ = 999.0;
    double estimatedDays_ = 999.0;
    bool paceSustainable_ = true;
};

// ---------------------------------------------------------------------------
// Warm-up / break scheduler
// ---------------------------------------------------------------------------

// Humans perform better after a warm-up and a break after losses. The
// scheduler converts the streak and performance state into a concrete
// recommendation.

struct BreakAdvice {
    bool takeBreak = false;
    int minutes = 0;
    const char* reason = "none";
};

class BreakScheduler {
public:
    static BreakScheduler& instance() {
        static BreakScheduler b;
        return b;
    }

    BreakAdvice evaluate(const StreakModel& streak, double performance,
                         double sessionLengthMin) {
        BreakAdvice a;
        if (streak.current <= -3) {
            a.takeBreak = true;
            a.minutes = 15;
            a.reason = "loss spiral";
        } else if (performance < 40.0 && sessionLengthMin > 45.0) {
            a.takeBreak = true;
            a.minutes = 10;
            a.reason = "tilt";
        } else if (sessionLengthMin > 120.0) {
            a.takeBreak = true;
            a.minutes = 20;
            a.reason = "session cap";
        }
        return a;
    }

    // Warm-up recommendation: one classic match before ranked.
    bool needsWarmup(int64_t timeSinceLastMatchMs) {
        return timeSinceLastMatchMs > 30LL * 86400000;
    }

    std::string diag() const {
        return "break: advisor ready\n";
    }

private:
};

// ---------------------------------------------------------------------------
// Session advisor (aggregate)
// ---------------------------------------------------------------------------

// Combines every sub-model into one clear verdict the UI can show.

struct SessionVerdict {
    const char* verdict = "keep queueing";
    bool stop = false;
    int breakMinutes = 0;
    std::string heroPick;
    std::string queueWindow;
    double risk = 0.0;
};

class SessionAdvisor {
public:
    static SessionAdvisor& instance() {
        static SessionAdvisor a;
        return a;
    }

    SessionVerdict verdict(int64_t nowMs) {
        SessionVerdict v;
        const StreakModel& s = StreakTracker::instance().current();
        double perf = PerformanceTracker::instance().current();
        double len = SessionRecorder::instance().avgSessionLengthMin();
        BreakAdvice br = BreakScheduler::instance().evaluate(s, perf, len);

        v.heroPick = HeroAdvisor::instance().bestHero();
        v.breakMinutes = br.minutes;
        v.risk = StreakTracker::instance().continuationRisk();

        if (br.takeBreak) {
            v.verdict = "take a break";
            v.stop = true;
        } else if (perf < 45.0) {
            v.verdict = "warm up first";
            v.stop = false;
        } else {
            v.verdict = "keep queueing";
        }
        return v;
    }

    std::string summaryLine(int64_t nowMs) {
        SessionVerdict v = verdict(nowMs);
        char buf[384];
        snprintf(buf, sizeof(buf),
                 "advisor: %s | hero %s | risk %.2f | break %dmin\n",
                 v.verdict, v.heroPick.c_str(), v.risk, v.breakMinutes);
        return std::string(buf);
    }

private:
};

// ---------------------------------------------------------------------------
// Session control hooks
// ---------------------------------------------------------------------------

// JNI-visible lifecycle hooks that keep every tracker in sync with real
// match events.

// Composite KDA score (kills+assists against deaths), clamped to a sane
// range for the hero advisor.
double kdaValue(const MatchResult& r) {
    int deaths = std::max(1, r.kdaDeaths);
    double kda = static_cast<double>(r.kdaKills + r.kdaAssists) / deaths;
    return rb_utils::clamp(kda, 0.0, 12.0);
}

void sessionBegin(int64_t nowMs, double mmr) {
    SessionRecorder::instance().begin(nowMs, mmr);
}

void sessionMatchEnded(const MatchRecord& rec) {
    SessionRecorder::instance().noteMatch(rec);
    PerformanceTracker::instance().add(rec.result.performancePercentile,
                                       rec.endedAtMs);
    StreakTracker::instance().note(rec.result.won, rec.mmrDelta,
                                   rec.endedAtMs);
    HeroAdvisor::instance().noteMatch(
        rec.teamComposition.empty() ? "unknown" : rec.teamComposition[0],
        rec.result.won, kdaValue(rec.result), rec.result.performancePercentile,
        rec.endedAtMs);
    LadderTracker::instance().update(rec.rankAfter);
}

void sessionEnd(int64_t nowMs) {
    SessionRecorder::instance().end(nowMs);
    LadderTracker::instance().recomputePace(
        SessionRecorder::instance().recent(10));
}

// ---------------------------------------------------------------------------
// Session intelligence diagnostics
// ---------------------------------------------------------------------------

std::string sessionIntelligenceDiag(int64_t nowMs) {
    std::string out;
    out += SessionRecorder::instance().diag();
    out += PerformanceTracker::instance().diag();
    out += StreakTracker::instance().diag();
    out += HeroAdvisor::instance().diag();
    out += LadderTracker::instance().diag();
    out += SessionAdvisor::instance().summaryLine(nowMs);
    return out;
}

// ---------------------------------------------------------------------------
// Match cadence model
// ---------------------------------------------------------------------------

// Tracks the pace at which matches flow through a session. A normal
// cadence is one match every 3-6 minutes; deviations feed the break
// scheduler.

struct CadenceSample {
    int64_t atMs = 0;
    double minutesSinceLast = 0.0;
};

class CadenceTracker {
public:
    static CadenceTracker& instance() {
        static CadenceTracker c;
        return c;
    }

    void note(int64_t nowMs) {
        if (lastMs_ != 0) {
            CadenceSample s;
            s.atMs = nowMs;
            s.minutesSinceLast =
                static_cast<double>(nowMs - lastMs_) / 60000.0;
            samples_.push_back(s);
            if (samples_.size() > 40) samples_.erase(samples_.begin());
        }
        lastMs_ = nowMs;
    }

    double avgGapMin() const {
        if (samples_.empty()) return 0.0;
        double sum = 0.0;
        for (const auto& s : samples_) sum += s.minutesSinceLast;
        return sum / samples_.size();
    }

    // True if matches are coming unusually fast (possible queue bug or
    // session abuse pattern).
    bool tooFast() const {
        double g = avgGapMin();
        return g > 0.0 && g < 2.5;
    }

    void reset() {
        samples_.clear();
        lastMs_ = 0;
    }

    std::string diag() const {
        char buf[128];
        snprintf(buf, sizeof(buf), "cadence: avg_gap=%.1fmin samples=%d\n",
                 avgGapMin(), static_cast<int>(samples_.size()));
        return std::string(buf);
    }

private:
    int64_t lastMs_ = 0;
    std::vector<CadenceSample> samples_;
};

// ---------------------------------------------------------------------------
// Session hour budget
// ---------------------------------------------------------------------------

// Models the daily play budget. The advisor recommends stopping when the
// budget is consumed, keeping sessions human-shaped.

class HourBudget {
public:
    static HourBudget& instance() {
        static HourBudget h;
        return h;
    }

    void setDailyLimit(double hours) { dailyLimit_ = hours; }
    double dailyLimit() const { return dailyLimit_; }

    void noteSession(double hours) {
        std::string key = todayKey();
        spent_[key] += hours;
    }

    double spentToday() const {
        auto it = spent_.find(todayKey());
        return it == spent_.end() ? 0.0 : it->second;
    }

    double remainingToday() const {
        return std::max(0.0, dailyLimit_ - spentToday());
    }

    bool limitReached() const { return remainingToday() <= 0.0; }

    std::string diag() const {
        char buf[128];
        snprintf(buf, sizeof(buf), "budget: spent=%.1fh limit=%.1fh\n",
                 spentToday(), dailyLimit_);
        return std::string(buf);
    }

private:
    static std::string todayKey() {
        int64_t t = utils::monotonicMs();
        return std::to_string(t / 86400000);
    }

    double dailyLimit_ = 4.0;
    std::map<std::string, double> spent_;
};

// ---------------------------------------------------------------------------
// Rank protection tracker
// ---------------------------------------------------------------------------

// MLBB protects players from demotion a few times per tier. The tracker
// remembers when protection was used so the advisor never plans around
// protection that no longer exists.

class ProtectionTracker {
public:
    static ProtectionTracker& instance() {
        static ProtectionTracker p;
        return p;
    }

    void noteProtectedLoss(int64_t nowMs) {
        uses_[tierKey()]++;
        lastUseMs_ = nowMs;
    }

    int usesLeft() const {
        auto it = uses_.find(tierKey());
        int used = (it == uses_.end()) ? 0 : it->second;
        return std::max(0, 2 - used);
    }

    bool protectedAvailable() const { return usesLeft() > 0; }

    void resetOnPromotion() {
        uses_.clear();
    }

    std::string diag() const {
        char buf[128];
        snprintf(buf, sizeof(buf), "protect: uses_left=%d\n", usesLeft());
        return std::string(buf);
    }

private:
    static std::string tierKey() { return "tier"; }
    std::map<std::string, int> uses_;
    int64_t lastUseMs_ = 0;
};

// ---------------------------------------------------------------------------
// Match duration model
// ---------------------------------------------------------------------------

// Average match length by mode; used to estimate stars-per-hour and to
// detect abnormal matches.

double expectedDurationMin(const std::string& mode) {
    if (mode == "ranked") return 16.0;
    if (mode == "classic") return 13.0;
    if (mode == "brawl") return 8.0;
    return 14.0;
}

// ---------------------------------------------------------------------------
// Session planner summary
// ---------------------------------------------------------------------------

std::string sessionPlannerLine(int64_t nowMs) {
    CadenceTracker::instance();
    HourBudget::instance();
    ProtectionTracker::instance();
    char buf[256];
    snprintf(buf, sizeof(buf),
             "plan: gap=%.1fmin budget_left=%.1fh protect=%d\n",
             CadenceTracker::instance().avgGapMin(),
             HourBudget::instance().remainingToday(),
             ProtectionTracker::instance().usesLeft());
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Full session suite diag
// ---------------------------------------------------------------------------

std::string sessionSuiteDiag(int64_t nowMs) {
    std::string out = sessionIntelligenceDiag(nowMs);
    out += CadenceTracker::instance().diag();
    out += HourBudget::instance().diag();
    out += ProtectionTracker::instance().diag();
    out += sessionPlannerLine(nowMs);
    return out;
}

// ---------------------------------------------------------------------------
// Match quality classifier
// ---------------------------------------------------------------------------

// Labels each match with a quality class derived from performance and
// outcome. The class feeds the advisor and the review card.

enum class MatchQuality {
    kCarry,      // won with high performance
    kSolid,      // won with average performance
    kCarried,    // won with low performance
    kCloseLoss,  // lost but performed well
    kBadLoss,    // lost with low performance
    kThrow,      // lost a match that should have been won
};

MatchQuality classifyMatch(const MatchRecord& rec) {
    int p = rec.result.performancePercentile;
    if (rec.result.won) {
        if (p >= 70) return MatchQuality::kCarry;
        if (p >= 45) return MatchQuality::kSolid;
        return MatchQuality::kCarried;
    }
    if (p >= 70) return MatchQuality::kCloseLoss;
    if (p >= 45) return MatchQuality::kThrow;
    return MatchQuality::kBadLoss;
}

const char* qualityName(MatchQuality q) {
    switch (q) {
        case MatchQuality::kCarry: return "carry";
        case MatchQuality::kSolid: return "solid";
        case MatchQuality::kCarried: return "carried";
        case MatchQuality::kCloseLoss: return "close";
        case MatchQuality::kBadLoss: return "bad";
        case MatchQuality::kThrow: return "throw";
    }
    return "solid";
}

// ---------------------------------------------------------------------------
// Quality ledger
// ---------------------------------------------------------------------------

// Rolling histogram of match qualities; drift toward "carried" or "throw"
// triggers advice about hero selection or warm-up.

class QualityLedger {
public:
    static QualityLedger& instance() {
        static QualityLedger q;
        return q;
    }

    void note(const MatchRecord& rec) {
        MatchQuality q = classifyMatch(rec);
        hist_[q]++;
        total_++;
    }

    double fraction(MatchQuality q) const {
        if (total_ == 0) return 0.0;
        auto it = hist_.find(q);
        return it == hist_.end() ? 0.0
                                 : static_cast<double>(it->second) / total_;
    }

    // Advice text based on the ledger shape.
    std::string advice() const {
        if (fraction(MatchQuality::kCarried) > 0.4) {
            return "you are being carried - focus on fundamentals";
        }
        if (fraction(MatchQuality::kThrow) > 0.3) {
            return "close games slipping - review late-game decisions";
        }
        if (fraction(MatchQuality::kCarry) > 0.5) {
            return "carrying consistently - keep the momentum";
        }
        return "shape looks healthy";
    }

    std::string diag() const {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "quality: carry=%.0f%% carried=%.0f%% throw=%.0f%% "
                 "advice=%s\n",
                 fraction(MatchQuality::kCarry) * 100.0,
                 fraction(MatchQuality::kCarried) * 100.0,
                 fraction(MatchQuality::kThrow) * 100.0, advice().c_str());
        return std::string(buf);
    }

private:
    std::map<MatchQuality, int> hist_;
    int total_ = 0;
};

// ---------------------------------------------------------------------------
// Star economy forecast
// ---------------------------------------------------------------------------

// Given the current win rate and star rules, forecasts the expected star
// gain for the next N matches, with a pessimistic band.

struct StarForecast {
    double expected = 0.0;
    double pessimistic = 0.0;
    double optimistic = 0.0;
    int matches = 0;
};

StarForecast forecastStars(double winRate, int matches) {
    StarForecast f;
    f.matches = matches;
    double gainPerWin = 1.0;
    double lossCost = 1.0;
    double expectedPerMatch = gainPerWin * winRate - lossCost * (1.0 - winRate);
    f.expected = expectedPerMatch * matches;
    double lowRate = std::max(0.0, winRate - 0.15);
    double highRate = std::min(1.0, winRate + 0.15);
    f.pessimistic = (gainPerWin * lowRate - lossCost * (1.0 - lowRate)) * matches;
    f.optimistic = (gainPerWin * highRate - lossCost * (1.0 - highRate)) * matches;
    return f;
}

// ---------------------------------------------------------------------------
// Session shape recorder (day-part buckets)
// ---------------------------------------------------------------------------

// Buckets match outcomes by day-part (morning/afternoon/evening/night).
// If the player consistently performs best in one bucket, the advisor
// steers queueing toward it.

enum class DayPart {
    kMorning,
    kAfternoon,
    kEvening,
    kNight,
};

DayPart dayPartFor(int hour) {
    if (hour < 6) return DayPart::kNight;
    if (hour < 12) return DayPart::kMorning;
    if (hour < 18) return DayPart::kAfternoon;
    return DayPart::kEvening;
}

const char* dayPartName(DayPart p) {
    switch (p) {
        case DayPart::kMorning: return "morning";
        case DayPart::kAfternoon: return "afternoon";
        case DayPart::kEvening: return "evening";
        case DayPart::kNight: return "night";
    }
    return "evening";
}

class DayPartTracker {
public:
    static DayPartTracker& instance() {
        static DayPartTracker d;
        return d;
    }

    void note(DayPart p, bool won) {
        auto& b = buckets_[p];
        b.total++;
        if (won) b.wins++;
    }

    double winRate(DayPart p) const {
        auto it = buckets_.find(p);
        if (it == buckets_.end() || it->second.total == 0) return 0.0;
        return static_cast<double>(it->second.wins) / it->second.total;
    }

    // Best day-part by win rate (needs at least 5 matches).
    DayPart bestDayPart() const {
        DayPart best = DayPart::kEvening;
        double bestRate = 0.0;
        for (int i = 0; i < 4; ++i) {
            DayPart p = static_cast<DayPart>(i);
            auto it = buckets_.find(p);
            if (it == buckets_.end() || it->second.total < 5) continue;
            if (winRate(p) > bestRate) {
                bestRate = winRate(p);
                best = p;
            }
        }
        return best;
    }

    std::string diag() const {
        DayPart best = bestDayPart();
        char buf[192];
        snprintf(buf, sizeof(buf), "daypart: best=%s winrate=%.0f%%\n",
                 dayPartName(best), winRate(best) * 100.0);
        return std::string(buf);
    }

private:
    struct Bucket {
        int total = 0;
        int wins = 0;
    };
    std::map<DayPart, Bucket> buckets_;
};

// ---------------------------------------------------------------------------
// Session suite final wiring
// ---------------------------------------------------------------------------

// Called after every ranked match to keep the whole session suite warm.
void sessionSuiteNoteMatch(const MatchRecord& rec) {
    QualityLedger::instance().note(rec);
    CadenceTracker::instance().note(rec.endedAtMs);
    if (rec.result.won) {
        DayPartTracker::instance().note(
            dayPartFor((rec.endedAtMs / 3600000) % 24), true);
    } else {
        DayPartTracker::instance().note(
            dayPartFor((rec.endedAtMs / 3600000) % 24), false);
    }
    if (rec.result.won && rec.rankAfter.absolute() > rec.rankBefore.absolute()) {
        ProtectionTracker::instance().resetOnPromotion();
    }
    if (rec.rankBefore.protection > 0 && !rec.result.won) {
        ProtectionTracker::instance().noteProtectedLoss(rec.endedAtMs);
    }
}

void sessionSuiteNoteSessionEnd(double hours) {
    HourBudget::instance().noteSession(hours);
}

std::string sessionSuiteFullDiag(int64_t nowMs) {
    std::string out = sessionSuiteDiag(nowMs);
    out += QualityLedger::instance().diag();
    out += DayPartTracker::instance().diag();
    return out;
}

// ---------------------------------------------------------------------------
// Star pace estimator
// ---------------------------------------------------------------------------

// Estimates how many stars the current session pace will yield per hour
// and per day, combining the cadence and the win rate.

struct PaceEstimate {
    double starsPerHour = 0.0;
    double starsPerDay = 0.0;
    double starsPerWeek = 0.0;
};

PaceEstimate estimateStarPace(double winRate, double avgMatchMin) {
    PaceEstimate p;
    double matchesPerHour = 60.0 / std::max(1.0, avgMatchMin);
    double starsPerMatch = winRate - (1.0 - winRate);
    p.starsPerHour = starsPerMatch * matchesPerHour;
    p.starsPerDay = p.starsPerHour * 2.5;
    p.starsPerWeek = p.starsPerDay * 7.0;
    return p;
}

// ---------------------------------------------------------------------------
// Session intensity governor
// ---------------------------------------------------------------------------

// Caps the amount of ranked queueing per rolling hour. When the cap is
// hit, the advisor recommends switching to classic for a cooldown match.

class IntensityGovernor {
public:
    static IntensityGovernor& instance() {
        static IntensityGovernor g;
        return g;
    }

    void noteRanked(int64_t nowMs) {
        rankedTimes_.push_back(nowMs);
        prune(nowMs);
    }

    int rankedLastHour(int64_t nowMs) {
        prune(nowMs);
        return static_cast<int>(rankedTimes_.size());
    }

    // True when the ranked cap is hit for this rolling hour.
    bool capped(int64_t nowMs, int cap) {
        return rankedLastHour(nowMs) >= cap;
    }

    std::string diag(int64_t nowMs) const {
        char buf[128];
        snprintf(buf, sizeof(buf), "intensity: ranked_1h=%d\n",
                 static_cast<int>(rankedTimes_.size()));
        return std::string(buf);
    }

private:
    void prune(int64_t nowMs) {
        while (!rankedTimes_.empty() && nowMs - rankedTimes_.front() > 3600000) {
            rankedTimes_.pop_front();
        }
    }

    std::deque<int64_t> rankedTimes_;
};

// ---------------------------------------------------------------------------
// Win-rate confidence band
// ---------------------------------------------------------------------------

// The true win rate is uncertain until many matches are played. The band
// shows the plausible range and whether the observed rate is reliable.

struct WinRateBand {
    double observed = 0.0;
    double low = 0.0;
    double high = 0.0;
    int matches = 0;
    bool reliable = false;
};

WinRateBand winRateBand(int wins, int matches) {
    WinRateBand b;
    b.matches = matches;
    if (matches == 0) return b;
    b.observed = static_cast<double>(wins) / matches;
    double se = std::sqrt(b.observed * (1.0 - b.observed) / matches);
    b.low = std::max(0.0, b.observed - 1.96 * se);
    b.high = std::min(1.0, b.observed + 1.96 * se);
    b.reliable = matches >= 25;
    return b;
}

// ---------------------------------------------------------------------------
// Session suite seal
// ---------------------------------------------------------------------------

std::string sessionSuiteSeal(int64_t nowMs) {
    std::string out = sessionSuiteFullDiag(nowMs);
    out += IntensityGovernor::instance().diag(nowMs);
    return out;
}

}  // namespace rb
}  // namespace arift