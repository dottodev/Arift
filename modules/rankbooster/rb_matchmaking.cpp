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
// MatchmakingEngine
// ---------------------------------------------------------------------------

QueueSnapshot MatchmakingEngine::beginQueue(const MatchmakingRequest& req) {
    searching_ = true;
    queue_.queueStartMs = utils::monotonicMs();
    queue_.queueEndMs = 0;
    queue_.mode = req.mode;
    queue_.searching = true;
    queue_.found = false;
    queue_.matchId = 0;
    queue_.estimatedWaitSec = estimateWait(req, 8);
    queue_.currentPosition = 0;
    queue_.totalInQueue = 0;
    queue_deadline_ms_ = queue_.queueStartMs +
                         static_cast<int64_t>(req.maxQueueSeconds * 1000.0);
    return queue_;
}

QueueSnapshot MatchmakingEngine::pollQueue() {
    int64_t now = utils::monotonicMs();
    queue_.totalInQueue = 8 + static_cast<int>((now / 1000) % 12);
    queue_.currentPosition =
        std::max(0, queue_.totalInQueue - static_cast<int>((now - queue_.queueStartMs) / 2500));
    if (now >= queue_deadline_ms_) {
        // Force match found at deadline (simulated matchmaking resolution).
        queue_.searching = false;
        queue_.found = true;
        queue_.queueEndMs = now;
        queue_.matchId = now % 1000000 + 1;
    }
    return queue_;
}

void MatchmakingEngine::cancelQueue() {
    searching_ = false;
    queue_.searching = false;
    queue_.found = false;
    queue_.queueEndMs = utils::monotonicMs();
}

std::vector<MatchCandidate> MatchmakingEngine::generatePool(
    size_t count, double myMmr, double difficulty) const {
    std::vector<MatchCandidate> pool;
    pool.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        MatchCandidate c;
        c.playerId = static_cast<int64_t>(i + 1001);
        c.name = "Player_" + std::to_string(i + 1);
        c.mmr = myMmr + rb_utils::gaussianRandom(
                            (difficulty - 0.5) * 400.0, 150.0);
        c.sigma = 60.0 + static_cast<double>((i * 37) % 200);
        c.role = static_cast<int>((i * 13) % 5);
        c.region = static_cast<int>((i * 7) % 8);
        c.latency = 15.0 + static_cast<double>((i * 11) % 80);
        c.winRate = 0.42 + static_cast<double>((i * 17) % 25) / 100.0;
        c.matches = 20 + static_cast<int>((i * 29) % 900);
        c.difficulty = difficultyOf(c, myMmr);
        c.fairnessScore = 0.3;
        c.synergyScore = 0.5;
        pool.push_back(c);
    }
    return pool;
}

MatchCandidate MatchmakingEngine::selectMatch(
    const std::vector<MatchCandidate>& pool, const MatchmakingRequest& req,
    double myMmr, double mySigma) const {
    if (pool.empty()) return MatchCandidate{};

    // Score candidates: prefer fair matches with good latency and desired
    // difficulty; add noise so selections never look robotic.
    double bestScore = -1e18;
    MatchCandidate best;
    double sigmaCombined = std::sqrt(mySigma * mySigma + 62500.0);

    for (const auto& c : pool) {
        double fairness = fairnessOf(c, myMmr, mySigma);
        double diff = difficultyOf(c, myMmr);
        double diffScore = 1.0 - std::fabs(diff - req.desiredDifficulty);
        double latScore = 1.0 - rb_utils::clamp01(c.latency / 120.0);
        double roleScore = 1.0;
        if (req.preferredRole > 0 && c.role != req.preferredRole) {
            roleScore = 0.6;
        }
        double synergy = synergyOf(c, PlayerProfile{});
        double noise = rb_utils::gaussianRandom(0.0, 0.08);
        double score = fairness * 0.4 + diffScore * 0.25 + latScore * 0.15 +
                       roleScore * 0.1 + synergy * 0.1 + noise;
        if (score > bestScore) {
            bestScore = score;
            best = c;
        }
    }

    // Stored fairness/synergy for telemetry.
    MatchCandidate result = best;
    result.fairnessScore = fairnessOf(result, myMmr, mySigma);
    result.synergyScore = synergyOf(result, PlayerProfile{});
    return result;
}

double MatchmakingEngine::difficultyOf(const MatchCandidate& c,
                                       double myMmr) const {
    double diff = c.mmr - myMmr;
    return rb_utils::clamp01(0.5 + diff / 1000.0);
}

double MatchmakingEngine::fairnessOf(const MatchCandidate& c, double myMmr,
                                     double mySigma) const {
    double diff = std::fabs(c.mmr - myMmr);
    double sigmaCombined = std::sqrt(mySigma * mySigma + c.sigma * c.sigma);
    double z = diff / sigmaCombined;
    return rb_utils::clamp01(1.0 - z * 0.5);
}

double MatchmakingEngine::synergyOf(const MatchCandidate& c,
                                    const PlayerProfile& me) const {
    (void)me;
    // Role complementarity heuristic: tank + marksman > double marksman.
    double base = 0.5;
    switch (c.role) {
        case 0: base = 0.55; break;   // any
        case 1: base = 0.62; break;   // tank
        case 2: base = 0.60; break;   // fighter
        case 3: base = 0.58; break;   // assassin
        case 4: base = 0.55; break;   // marksman
        default: base = 0.5; break;
    }
    return rb_utils::clamp01(base + (c.winRate - 0.5) * 0.3);
}

double MatchmakingEngine::estimateWait(const MatchmakingRequest& req,
                                       int poolSize) const {
    double base = 12.0;
    double difficultyFactor = 1.0 + (1.0 - req.desiredDifficulty) * 4.0;
    double partyFactor = 1.0 + 0.5 * static_cast<double>(req.partySize - 1);
    double regionFactor = req.regionBias > 0 ? 1.6 : 1.0;
    double poolFactor = poolSize > 0 ? 24.0 / static_cast<double>(poolSize) : 3.0;
    return base * difficultyFactor * partyFactor * regionFactor * poolFactor;
}

// ---------------------------------------------------------------------------
// Pool simulation detail
// ---------------------------------------------------------------------------

namespace {

// Simple xorshift PRNG for deterministic pool generation.
uint32_t xorshift(uint32_t& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

double hashToUnit(uint32_t v) {
    return static_cast<double>(v % 100000) / 100000.0;
}

}  // namespace

// Pool band distribution: how candidates spread around a difficulty center.
std::vector<double> poolBandDistribution(double difficulty) {
    std::vector<double> bands(5, 0.0);
    // Concentration around the difficulty center.
    bands[0] = 0.10 + (1.0 - difficulty) * 0.15;
    bands[1] = 0.20 + (1.0 - difficulty) * 0.10;
    bands[2] = 0.35;
    bands[3] = 0.20 + difficulty * 0.10;
    bands[4] = 0.10 + difficulty * 0.15;
    double sum = 0.0;
    for (double b : bands) sum += b;
    for (double& b : bands) b /= sum;
    return bands;
}

// Candidate fairness histogram over a pool (for UI telemetry).
std::vector<int> fairnessHistogram(const std::vector<MatchCandidate>& pool,
                                   double myMmr, double mySigma) {
    std::vector<int> hist(10, 0);
    for (const auto& c : pool) {
        double f = 0.0;
        double diff = std::fabs(c.mmr - myMmr);
        double sc = std::sqrt(mySigma * mySigma + c.sigma * c.sigma);
        f = rb_utils::clamp01(1.0 - (diff / sc) * 0.5);
        int idx = static_cast<int>(f * 10.0);
        if (idx > 9) idx = 9;
        hist[static_cast<size_t>(idx)] += 1;
    }
    return hist;
}

// Latency-weighted pick probability for a candidate.
double latencyPickProbability(double latencyMs) {
    return rb_utils::clamp01(1.0 - latencyMs / 250.0);
}

// Role preference satisfaction: does the pool cover my role needs?
double roleCoverage(const std::vector<MatchCandidate>& pool, int role) {
    if (pool.empty()) return 0.0;
    int covered = 0;
    for (const auto& c : pool) {
        if (c.role == role) covered += 1;
    }
    return static_cast<double>(covered) / static_cast<double>(pool.size());
}

// Pool quality composite: fairness, latency, synergy blend.
double poolQuality(const std::vector<MatchCandidate>& pool, double myMmr,
                   double mySigma) {
    if (pool.empty()) return 0.0;
    double acc = 0.0;
    for (const auto& c : pool) {
        double f = 0.0;
        double diff = std::fabs(c.mmr - myMmr);
        double sc = std::sqrt(mySigma * mySigma + c.sigma * c.sigma);
        f = rb_utils::clamp01(1.0 - (diff / sc) * 0.5);
        double lat = rb_utils::clamp01(1.0 - c.latency / 250.0);
        acc += f * 0.6 + lat * 0.4;
    }
    return acc / static_cast<double>(pool.size());
}

// Deterministic pool seed from player id + time bucket.
uint32_t poolSeed(int64_t playerId, int64_t timeBucket) {
    uint64_t h = static_cast<uint64_t>(playerId) * 0x9E3779B97F4A7C15ULL;
    h ^= static_cast<uint64_t>(timeBucket) * 0xBF58476D1CE4E5B9ULL;
    h ^= h >> 33;
    h *= 0xFF51AFD7ED558CCDULL;
    h ^= h >> 33;
    return static_cast<uint32_t>(h);
}

// Similarity between two candidates (for party grouping).
double candidateSimilarity(const MatchCandidate& a, const MatchCandidate& b) {
    double d = std::fabs(a.mmr - b.mmr) / 1000.0;
    double roleMatch = a.role == b.role ? 1.0 : 0.0;
    return rb_utils::clamp01(1.0 - d * 0.5 + roleMatch * 0.1);
}

// Weighted random pick from a pool using a scoring vector.
size_t weightedPick(const std::vector<double>& scores, uint32_t seed) {
    if (scores.empty()) return 0;
    double total = 0.0;
    for (double s : scores) total += s;
    if (total <= 0.0) return 0;
    uint32_t st = seed;
    double r = hashToUnit(xorshift(st)) * total;
    double acc = 0.0;
    for (size_t i = 0; i < scores.size(); ++i) {
        acc += scores[i];
        if (r <= acc) return i;
    }
    return scores.size() - 1;
}

// ---------------------------------------------------------------------------
// Queue telemetry
// ---------------------------------------------------------------------------

// Queue-result record for session summaries.
struct QueueResult {
    int64_t startMs = 0;
    int64_t endMs = 0;
    double waitSec = 0.0;
    bool found = false;
    double poolQuality = 0.0;
};

// Build a queue-result log from engine snapshots.
std::vector<QueueResult> queueResultLog(const std::vector<QueueSnapshot>& snaps) {
    std::vector<QueueResult> out;
    for (const auto& s : snaps) {
        QueueResult q;
        q.startMs = s.queueStartMs;
        q.endMs = s.queueEndMs;
        q.waitSec = s.estimatedWaitSec;
        q.found = s.found;
        out.push_back(q);
    }
    return out;
}

// Average queue wait across recent queue cycles.
double avgQueueWaitSec(const std::vector<QueueResult>& log) {
    if (log.empty()) return 0.0;
    double sum = 0.0;
    for (const auto& q : log) sum += q.waitSec;
    return sum / static_cast<double>(log.size());
}

// Queue find-rate (fraction that resolved).
double queueFindRate(const std::vector<QueueResult>& log) {
    if (log.empty()) return 0.0;
    int found = 0;
    for (const auto& q : log) {
        if (q.found) found += 1;
    }
    return static_cast<double>(found) / static_cast<double>(log.size());
}

// Wait-time distribution bucket (fast/normal/slow).
const char* waitBucket(double waitSec) {
    if (waitSec < 20.0) return "fast";
    if (waitSec < 60.0) return "normal";
    return "slow";
}

// Match-quality estimate from pool + selected candidate.
double matchQualityEstimate(const MatchCandidate& c, double myMmr,
                            double mySigma) {
    double f = 0.0;
    double diff = std::fabs(c.mmr - myMmr);
    double sc = std::sqrt(mySigma * mySigma + c.sigma * c.sigma);
    f = rb_utils::clamp01(1.0 - (diff / sc) * 0.5);
    double lat = rb_utils::clamp01(1.0 - c.latency / 250.0);
    return f * 0.6 + lat * 0.25 + c.synergyScore * 0.15;
}

// Pool diversity: spread of roles + mmr in the pool.
double poolDiversity(const std::vector<MatchCandidate>& pool) {
    if (pool.empty()) return 0.0;
    std::vector<double> mmrs;
    std::vector<int> roles;
    for (const auto& c : pool) {
        mmrs.push_back(c.mmr);
        roles.push_back(c.role);
    }
    double mmrSpread = rb_utils::standardDeviation(mmrs);
    double roleSpread = static_cast<double>(
        std::set<int>(roles.begin(), roles.end()).size());
    return rb_utils::clamp01(mmrSpread / 300.0 * 0.5 + roleSpread / 5.0 * 0.5);
}

// Requeue recommendation: should we cancel and restart?
bool recommendRequeue(const QueueSnapshot& snap, double patienceSec) {
    if (!snap.searching) return false;
    double waited = static_cast<double>(
        utils::monotonicMs() - snap.queueStartMs) / 1000.0;
    return waited > patienceSec;
}

// Best time window estimate for a queue given pool stats.
double optimalQueueWindow(const std::vector<MatchCandidate>& pool,
                          int64_t nowMs) {
    double pop = 0.35;
    time_t sec = static_cast<time_t>(nowMs / 1000);
    struct tm tmv{};
    localtime_r(&sec, &tmv);
    int hour = tmv.tm_hour;
    if (hour >= 19 && hour <= 23) pop = 0.85;
    else if (hour >= 12 && hour <= 14) pop = 0.7;
    else if (hour >= 15 && hour <= 18) pop = 0.6;
    return rb_utils::clamp01(pop + static_cast<double>(pool.size()) * 0.01);
}

}  // namespace rb
}  // namespace arift