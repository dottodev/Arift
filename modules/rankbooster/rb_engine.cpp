#include "rb_api.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

#include "arift_log.h"
#include "arift_utils.h"
#include "rb_math.h"

namespace arift {
namespace rb {

// ---------------------------------------------------------------------------
// RbEngine — orchestrates the booster lifecycle; driven by RankBooster::pump.
// ---------------------------------------------------------------------------

RbEngine& RbEngine::instance() {
    static RbEngine e;
    return e;
}

void RbEngine::start(const RbConfig& cfg) {
    if (running_.load()) return;
    started_ms_ = utils::monotonicMs();
    last_tick_ms_ = started_ms_;
    running_.store(true);
    state_.store(BoosterState::kCollecting);
    (void)cfg;
    ARIFT_INFO(kTagRankBooster, "engine started");
}

void RbEngine::stop() {
    running_.store(false);
    state_.store(BoosterState::kIdle);
    session_active_ = false;
}

void RbEngine::tick() {
    if (!running_.load()) return;
    int64_t now = utils::monotonicMs();
    if (now - last_tick_ms_ < 200) return;
    last_tick_ms_ = now;
    advanceState(now);
}

void RbEngine::beginSession() {
    if (!running_.load()) return;
    session_active_ = true;
    session_win_rate_ = 0.5;
    match_counter_ = 0;
    state_.store(BoosterState::kReady);
}

void RbEngine::endSession() {
    session_active_ = false;
    state_.store(BoosterState::kIdle);
}

void RbEngine::onMatchFinished(const MatchResult& result) {
    match_counter_ += 1;
    double played = static_cast<double>(match_counter_);
    double wins = result.won ? 1.0 : 0.0;
    session_win_rate_ = rb_utils::damp(session_win_rate_,
                                       wins / played, 0.35, 1.0);
}

bool RbEngine::shouldStartQueue(const MatchmakingRequest& req) const {
    if (!running_.load()) return false;
    if (!session_active_) return false;
    if (state_.load() != BoosterState::kReady &&
        state_.load() != BoosterState::kActive) {
        return false;
    }
    if (req.maxQueueSeconds <= 0) return false;
    return true;
}

double RbEngine::queueDifficulty(const MatchmakingRequest& req) const {
    // Blend request preference with session momentum.
    double base = req.desiredDifficulty;
    double momentum = (session_win_rate_ - 0.5) * 0.3;
    return rb_utils::clamp01(base + momentum);
}

int64_t RbEngine::recommendedDelayBeforeAcceptMs() const {
    // Humanized accept delay around ~1.5s.
    return static_cast<int64_t>(
        rb_utils::gaussianRandom(1500.0, 300.0));
}

EngineSnapshot RbEngine::snapshot() const {
    EngineSnapshot s;
    s.state = state_.load();
    s.searching = false;
    s.uptimeMs = utils::monotonicMs() - started_ms_;
    s.matchesTracked = match_counter_;
    s.effectiveWinRate = session_win_rate_;
    return s;
}

std::string RbEngine::summary() const {
    EngineSnapshot s = snapshot();
    char buf[256];
    snprintf(buf, sizeof(buf),
             "engine: state=%d running=%d uptime=%llds matches=%lld wr=%.2f",
             static_cast<int>(s.state), running_.load() ? 1 : 0,
             static_cast<long long>(s.uptimeMs / 1000),
             static_cast<long long>(s.matchesTracked),
             s.effectiveWinRate);
    return std::string(buf);
}

void RbEngine::advanceState(int64_t nowMs) {
    switch (state_.load()) {
        case BoosterState::kCollecting: {
            // Collect until at least one recorded match or a grace period.
            int64_t elapsed = nowMs - started_ms_;
            if (elapsed > 30000) state_.store(BoosterState::kAnalyzing);
            break;
        }
        case BoosterState::kAnalyzing: {
            state_.store(BoosterState::kTuning);
            break;
        }
        case BoosterState::kTuning: {
            state_.store(BoosterState::kReady);
            break;
        }
        case BoosterState::kReady: {
            if (session_active_) state_.store(BoosterState::kActive);
            break;
        }
        case BoosterState::kActive: {
            // Active sessions run until ended or paused by the guard.
            break;
        }
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// Engine helpers
// ---------------------------------------------------------------------------

// Session quality: composite score of recent session behavior.
double sessionQuality(const RbStatistics& stats, const RbConfig& cfg) {
    double q = 0.0;
    q += stats.winRate * 0.5;
    q += stats.avgPerformance / 100.0 * 0.25;
    q += (1.0 - stats.sigma / 350.0) * 0.15;
    q += (1.0 - stats.volatility / 0.12) * 0.1;
    if (!cfg.enabled) q *= 0.5;
    return rb_utils::clamp01(q);
}

// Star projection for a target rank with per-tier economy.
int starsToTarget(const RankPoint& from, const RankPoint& target) {
    int delta = target.absolute() - from.absolute();
    return std::max(0, delta);
}

// Expected number of queue cycles before a match resolves.
int expectedQueueCycles(double difficulty) {
    double base = 1.0 + (1.0 - difficulty) * 1.5;
    return std::max(1, static_cast<int>(std::round(base)));
}

// ---------------------------------------------------------------------------
// Difficulty engine
// ---------------------------------------------------------------------------

namespace {

// Wilson-style confidence bound on the current win-rate estimate.
double wrLowerBound(double wins, double matches) {
    if (matches < 1.0) return 0.0;
    double p = wins / matches;
    double z = 1.65;
    double denom = 1.0 + z * z / matches;
    double centre = (p + z * z / (2.0 * matches)) / denom;
    double margin = z * std::sqrt(p * (1.0 - p) / matches +
                                  z * z / (4.0 * matches * matches)) / denom;
    return std::max(0.0, centre - margin);
}

}  // namespace

// Difficulty target blended from confidence and session momentum.
double difficultyTarget(const RbStatistics& stats, const RbConfig& cfg,
                        double sessionWr) {
    double bound = wrLowerBound(static_cast<double>(stats.wins),
                                static_cast<double>(stats.totalMatches));
    double gap = cfg.desiredWinRate - bound;
    // Overperformers get harder matches; underperformers get easier ones.
    double adj = rb_utils::clamp(gap * 2.0, -0.45, 0.45);
    double momentum = rb_utils::clamp((sessionWr - 0.55) * 0.5, -0.2, 0.2);
    double base = 0.5 + adj + momentum;
    return rb_utils::clamp01(base);
}

// Match acceptance probability given current difficulty preference.
double acceptProbability(double difficulty, double myMmr, double oppMmr) {
    double diff = (oppMmr - myMmr) / 1000.0;
    double want = difficulty - 0.5;
    double score = rb_utils::clamp01(0.5 - (diff - want) * 2.0);
    return rb_utils::clamp01(score + rb_utils::gaussianRandom(0.0, 0.06));
}

// Queue delay model: geometric-ish growth with difficulty.
double queueDelayModel(double difficulty, double baseSeconds) {
    double factor = 1.0 + (1.0 - difficulty) * 3.0;
    return baseSeconds * factor;
}

// ---------------------------------------------------------------------------
// Session tracker
// ---------------------------------------------------------------------------

struct SessionTracker {
    int64_t startedMs = 0;
    int matches = 0;
    int wins = 0;
    int losses = 0;
    double mmrDeltaSum = 0.0;
    double perfSum = 0.0;
    int queueCycles = 0;
    int pauses = 0;
    std::vector<double> matchDurations;

    void record(const MatchRecord& rec) {
        matches += 1;
        if (rec.result.won) {
            wins += 1;
        } else {
            losses += 1;
        }
        mmrDeltaSum += rec.mmrDelta;
        perfSum += static_cast<double>(rec.result.performancePercentile);
        matchDurations.push_back(rec.result.durationMin);
    }

    double winRate() const {
        return matches > 0 ? static_cast<double>(wins) /
                                 static_cast<double>(matches) : 0.5;
    }

    double avgDelta() const {
        return matches > 0 ? mmrDeltaSum / static_cast<double>(matches) : 0.0;
    }

    double avgPerformance() const {
        return matches > 0 ? perfSum / static_cast<double>(matches) : 50.0;
    }

    double totalPlayedMin() const {
        double sum = 0.0;
        for (double d : matchDurations) sum += d;
        return sum;
    }

    std::string report() const {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "session: matches=%d W:%d L:%d wr=%.1f%% deltaAvg=%.1f "
                 "perfAvg=%.1f queues=%d pauses=%d playedMin=%.0f",
                 matches, wins, losses, winRate() * 100.0, avgDelta(),
                 avgPerformance(), queueCycles, pauses, totalPlayedMin());
        return std::string(buf);
    }
};

static SessionTracker& sessionTracker() {
    static SessionTracker st;
    return st;
}

// ---------------------------------------------------------------------------
// Engine diagnostics
// ---------------------------------------------------------------------------

// Human-readable pipeline status for the UI overlay.
std::string engineStatusText(const EngineSnapshot& snap,
                             const RbConfig& cfg) {
    const char* stateNames[] = {
        "IDLE", "COLLECTING", "ANALYZING", "TUNING", "READY",
        "ACTIVE", "PAUSED", "ERROR",
    };
    int idx = static_cast<int>(snap.state);
    if (idx < 0 || idx > 7) idx = 0;
    char buf[256];
    snprintf(buf, sizeof(buf),
             "STATE=%s ENABLED=%d WR=%.1f%% MATCHES=%lld QUEUE=%s",
             stateNames[idx], cfg.enabled ? 1 : 0,
             snap.effectiveWinRate * 100.0,
             static_cast<long long>(snap.matchesTracked),
             snap.searching ? "ON" : "OFF");
    return std::string(buf);
}

// Per-state tick behavior summary (used for debug logs).
std::string engineStateHint(BoosterState st) {
    switch (st) {
        case BoosterState::kCollecting:
            return "waiting for first matches to calibrate";
        case BoosterState::kAnalyzing:
            return "analyzing match history";
        case BoosterState::kTuning:
            return "tuning difficulty and timing";
        case BoosterState::kReady:
            return "ready to queue";
        case BoosterState::kActive:
            return "queue cycle active";
        case BoosterState::kPaused:
            return "paused by guard or schedule";
        default:
            return "idle";
    }
}

// ---------------------------------------------------------------------------
// Engine reports
// ---------------------------------------------------------------------------

// Progress toward target rank in percent.
double rankProgress(const PlayerProfile& p, const RankPoint& target) {
    int from = p.rank.absolute();
    int to = target.absolute();
    if (to <= from) return 1.0;
    // Use MMR as a smoother progress signal.
    double mmrProgress = rb_utils::clamp01((p.mmr - 1000.0) / 3000.0);
    return rb_utils::clamp01(mmrProgress * 0.7 + 0.3);
}

// Expected sessions remaining to reach target.
double sessionsRemaining(const RbStatistics& stats, const RankPoint& target,
                         const RankPoint& current) {
    int stars = starsToTarget(current, target);
    double ev = stats.expectedStarsPerMatch;
    if (ev <= 0.0) return 1e9;
    double matches = static_cast<double>(stars) / ev;
    return matches / 8.0;
}

// Star pace chart: stars needed vs. sessions.
std::vector<double> starPaceCurve(const RbStatistics& stats, int starsNeeded,
                                  int sessions) {
    std::vector<double> out;
    double remaining = static_cast<double>(starsNeeded);
    for (int i = 0; i < sessions; ++i) {
        out.push_back(remaining);
        remaining -= stats.expectedStarsPerMatch * 8.0;
        if (remaining < 0.0) remaining = 0.0;
    }
    return out;
}

// Engine event counters (for UI badge).
struct EngineCounters {
    int64_t ticks = 0;
    int64_t queues = 0;
    int64_t matches = 0;
    int64_t pauses = 0;
    int64_t tunings = 0;
};

EngineCounters engineCounters() {
    EngineCounters c;
    BoosterTelemetry t;
    RbTelemetry::instance().snapshot(t);
    c.matches = t.matchesTracked;
    c.queues = t.queueJoins;
    c.tunings = t.tunerRuns;
    c.pauses = t.guardEvents;
    return c;
}

// Format a counter line for the overlay.
std::string countersLine(const EngineCounters& c) {
    char buf[128];
    snprintf(buf, sizeof(buf), "queues=%lld matches=%lld tunings=%lld pauses=%lld",
             static_cast<long long>(c.queues),
             static_cast<long long>(c.matches),
             static_cast<long long>(c.tunings),
             static_cast<long long>(c.pauses));
    return std::string(buf);
}

// Effective difficulty the engine will seek next cycle.
double nextCycleDifficulty(const RbEngine& engine, const RbConfig& cfg) {
    EngineSnapshot s = engine.snapshot();
    double momentum = rb_utils::clamp((s.effectiveWinRate - 0.55) * 0.5, -0.2, 0.2);
    double d = rb_utils::clamp01(0.5 + momentum);
    (void)d;
    return rb_utils::clamp01(s.effectiveWinRate);
}

// ---------------------------------------------------------------------------
// Engine verdicts and scheduling helpers
// ---------------------------------------------------------------------------

// Whether the engine currently believes a queue is reasonable.
bool queueReasonableNow(const RbStatistics& stats, const RbConfig& cfg) {
    if (!cfg.enabled) return false;
    if (cfg.autoPauseOnRisk && stats.currentStreak <= -4) return false;
    if (stats.avgPerformance < 35.0 && stats.totalMatches >= 5) return false;
    return true;
}

// Recommended queue hour window from the engine's perspective.
std::pair<int, int> queueWindowHours(const RbStatistics& stats,
                                     const RbConfig& cfg) {
    int lo = 12;
    int hi = 23;
    if (cfg.preferEvenings) {
        lo = 18;
        hi = 23;
    }
    if (stats.matchesThisWeek >= 30) {
        lo = 10;
        hi = 22;
    }
    return std::make_pair(lo, hi);
}

// Expected time (minutes) to gain one star at the current pace.
double minutesPerStar(const RbStatistics& stats) {
    double ev = stats.expectedStarsPerMatch;
    if (ev <= 0.0) return 120.0;
    return 10.0 / ev;
}

// Total sessions needed to finish the climb at the current pace.
int sessionsToTarget(const RbStatistics& stats, const RankPoint& current,
                     const RankPoint& target) {
    int stars = std::max(0, target.absolute() - current.absolute());
    if (stars == 0) return 0;
    double perSession = std::max(0.25, stats.expectedStarsPerMatch * 8.0);
    return static_cast<int>(std::ceil(stars / perSession));
}

// Day of week label for reporting.
const char* dayOfWeekLabel(int64_t ms) {
    time_t raw = static_cast<time_t>(ms / 1000);
    struct tm tmv;
    localtime_r(&raw, &tmv);
    switch (tmv.tm_wday) {
        case 0: return "Sunday";
        case 1: return "Monday";
        case 2: return "Tuesday";
        case 3: return "Wednesday";
        case 4: return "Thursday";
        case 5: return "Friday";
        default: return "Saturday";
    }
}

// Busy-hour multiplier (evening peak adds wait time).
double busyHourMultiplier(int64_t ms) {
    time_t raw = static_cast<time_t>(ms / 1000);
    struct tm tmv;
    localtime_r(&raw, &tmv);
    int h = tmv.tm_hour;
    if (h >= 20 && h <= 23) return 1.4;
    if (h >= 12 && h <= 14) return 1.2;
    return 1.0;
}

// Engine verdict for the session: queue / pause / stop.
struct EngineVerdict {
    std::string state = "idle";
    std::string advice = "";
};

EngineVerdict engineVerdict(const RbStatistics& stats, const RbConfig& cfg,
                            int guardRisk) {
    EngineVerdict v;
    if (!cfg.enabled) {
        v.state = "disabled";
        v.advice = "rank booster disabled";
        return v;
    }
    if (guardRisk >= 60) {
        v.state = "paused";
        v.advice = "guard risk too high - pause";
        return v;
    }
    if (stats.currentStreak <= -4) {
        v.state = "paused";
        v.advice = "losing streak - pause";
        return v;
    }
    v.state = "active";
    v.advice = "queue normally";
    return v;
}

// Estimated queue acceptance odds for the next attempt.
double nextAcceptOdds(const RbEngine& engine, const RbStatistics& stats) {
    if (!engine.running()) return 0.0;
    EngineSnapshot snap = engine.snapshot();
    double base = 0.45 + stats.winRate * 0.3;
    if (snap.difficulty > 0.65) base *= 0.8;
    if (snap.queuesSinceMatch >= 3) base *= 1.2;
    return rb_utils::clamp01(base);
}

}  // namespace rb
}  // namespace arift