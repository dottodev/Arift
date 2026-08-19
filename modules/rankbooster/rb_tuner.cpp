#include "rb_api.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "arift_utils.h"
#include "rb_math.h"

namespace arift {
namespace rb {

// ---------------------------------------------------------------------------
// AdaptiveTuner
// ---------------------------------------------------------------------------

TunerResult AdaptiveTuner::tune(const PlayerProfile& p,
                                const RbStatistics& stats,
                                const RbConfig& current,
                                AggressionLevel aggr) const {
    TunerResult out;
    out.runMs = utils::monotonicMs();
    out.suggested = current;

    // 1. Win-rate target scaling by aggression.
    double targetWr = 0.55;
    switch (aggr) {
        case AggressionLevel::kConservative: targetWr = 0.72; break;
        case AggressionLevel::kBalanced: targetWr = 0.62; break;
        case AggressionLevel::kAggressive: targetWr = 0.55; break;
        case AggressionLevel::kExtreme: targetWr = 0.50; break;
    }
    if (std::fabs(out.suggested.desiredWinRate - targetWr) > 0.01) {
        out.suggested.desiredWinRate = targetWr;
        out.changed = true;
    }

    // 2. Difficulty targeting from recent performance.
    double diffDelta = recommendedTargetDelta(stats, p.mmr,
                                              out.suggested.desiredWinRate);
    double baseDiff = 0.5;
    if (aggr == AggressionLevel::kAggressive) baseDiff = 0.6;
    if (aggr == AggressionLevel::kExtreme) baseDiff = 0.7;
    double targetDiff = rb_utils::clamp01(
        baseDiff + (diffDelta / 600.0) * 0.3);

    // 3. Queue timing: adjust wait budget with streak.
    if (p.streak >= 3) {
        out.suggested.maxQueueSeconds = std::max(45.0,
            current.maxQueueSeconds * 0.8);
        out.changed = true;
    } else if (p.streak <= -2) {
        out.suggested.maxQueueSeconds = std::min(150.0,
            current.maxQueueSeconds * 1.2);
        out.changed = true;
    }

    // 4. Sigma floor: tighten confidence as sample grows.
    if (p.matchesPlayed > 60 && out.suggested.sigmaFloor > 40.0) {
        out.suggested.sigmaFloor = 40.0;
        out.changed = true;
    }

    // 5. Guard sensitivity ramps with aggression.
    int wantSens = 3;
    if (aggr == AggressionLevel::kExtreme) wantSens = 5;
    if (aggr == AggressionLevel::kConservative) wantSens = 2;
    if (out.suggested.guardSensitivity != wantSens) {
        out.suggested.guardSensitivity = wantSens;
        out.changed = true;
    }

    // 6. Burst pattern control.
    bool wantBurst = aggr == AggressionLevel::kExtreme;
    if (out.suggested.burstPatternEnabled != wantBurst) {
        out.suggested.burstPatternEnabled = wantBurst;
        out.changed = true;
    }

    char buf[256];
    snprintf(buf, sizeof(buf),
             "tuned: targetWr=%.2f diff=%.2f queue=%.0fs sens=%d sigmaFloor=%.0f",
             out.suggested.desiredWinRate, targetDiff,
             out.suggested.maxQueueSeconds, wantSens,
             out.suggested.sigmaFloor);
    out.rationale = buf;
    return out;
}

double AdaptiveTuner::recommendedQueueDifficulty(const PlayerProfile& p,
                                                 const RbStatistics& stats,
                                                 const RbConfig& cfg) const {
    // Start from current win rate: below target -> easier matches.
    double base = 0.5;
    double wrGap = cfg.desiredWinRate - stats.winRate;
    base += wrGap * 0.4;
    // Confidence: low sigma means we trust the estimate -> tune more.
    double conf = p.sigma < 120.0 ? 0.15 : 0.0;
    base -= conf;
    // Streak guardrail.
    if (p.streak <= -3) base -= 0.15;
    if (p.streak >= 5) base += 0.05;
    return rb_utils::clamp01(base);
}

double AdaptiveTuner::recommendedPlaystyle(const RbConfig& cfg) const {
    if (!cfg.randomizePlaystyle) return 0.5;
    // Slight random walk so playstyle looks organic across sessions.
    double base = 0.5;
    switch (cfg.aggression) {
        case AggressionLevel::kConservative: base = 0.35; break;
        case AggressionLevel::kBalanced: base = 0.5; break;
        case AggressionLevel::kAggressive: base = 0.65; break;
        case AggressionLevel::kExtreme: base = 0.8; break;
    }
    return rb_utils::clamp01(base + rb_utils::gaussianRandom(0.0, 0.06));
}

bool AdaptiveTuner::shouldPause(const RbStatistics& stats,
                                const RbConfig& cfg) const {
    if (!cfg.autoPauseOnRisk) return false;
    // Long losing streak -> pause to reset.
    if (stats.currentStreak <= -4) return true;
    // Volatility spiking -> unstable rating, pause.
    if (stats.volatility > 0.11) return true;
    // Too many matches in short window.
    if (stats.matchesThisWeek > 60) return true;
    return false;
}

std::string AdaptiveTuner::explain(const TunerResult& r) const {
    return r.rationale;
}

// ---------------------------------------------------------------------------
// Deeper tuning dimensions
// ---------------------------------------------------------------------------

namespace {

// Map a 0..1 value into an ordinal band index.
int bandOf(double v, int bands) {
    int idx = static_cast<int>(v * static_cast<double>(bands));
    return rb_utils::clampInt(idx, 0, bands - 1);
}

}  // namespace

// Difficulty stepping: converge gradually to avoid oscillation.
double stepDifficulty(double current, double target, double step) {
    if (current < target) {
        return std::min(current + step, target);
    }
    return std::max(current - step, target);
}

// Queue-wait patience: how long to hold before widening search.
double patienceForDifficulty(double difficulty) {
    // Harder matches -> longer patience.
    return rb_utils::clamp(30.0 + difficulty * 90.0, 30.0, 150.0);
}

// Timing-noise budget by aggression level (ms of acceptable randomness).
double timingNoiseBudget(AggressionLevel aggr) {
    switch (aggr) {
        case AggressionLevel::kConservative: return 4000.0;
        case AggressionLevel::kBalanced: return 7000.0;
        case AggressionLevel::kAggressive: return 10000.0;
        case AggressionLevel::kExtreme: return 15000.0;
        default: return 7000.0;
    }
}

// Retune trigger: true when conditions changed enough to justify a rerun.
bool needsRetune(const RbConfig& current, const RbConfig& observed,
                 double sensitivity) {
    double d = 0.0;
    d += std::fabs(current.desiredWinRate - observed.desiredWinRate) * 50.0;
    d += std::fabs(current.maxQueueSeconds - observed.maxQueueSeconds) / 10.0;
    d += std::fabs(current.mmrTarget - observed.mmrTarget) / 100.0;
    return d > sensitivity;
}

// Sensitivity profile per aggression level (0..1).
double sensitivityFor(AggressionLevel aggr) {
    switch (aggr) {
        case AggressionLevel::kConservative: return 0.3;
        case AggressionLevel::kBalanced: return 0.5;
        case AggressionLevel::kAggressive: return 0.7;
        case AggressionLevel::kExtreme: return 0.9;
        default: return 0.5;
    }
}

// Suggest a queue-window length (hours) given a schedule budget.
double sessionHoursForBudget(int matches, double avgMatchMin) {
    double raw = static_cast<double>(matches) * avgMatchMin / 60.0;
    // Add inter-match gaps.
    raw += static_cast<double>(std::max(0, matches - 1)) * 1.2 / 60.0;
    return rb_utils::clamp(raw, 1.0, 6.0);
}

// Risk-adjusted win-rate target (lower target when guard risk is high).
double riskAdjustedTarget(double base, int guardRisk) {
    double adj = static_cast<double>(guardRisk) / 100.0 * 0.1;
    return rb_utils::clamp(base - adj, 0.35, 0.95);
}

// Blended tuner score across all dimensions (for A/B comparison).
double tunerScore(const TunerResult& r, const RbConfig& baseline) {
    double score = 0.0;
    score += std::fabs(r.suggested.desiredWinRate - baseline.desiredWinRate);
    score += std::fabs(r.suggested.maxQueueSeconds - baseline.maxQueueSeconds) / 150.0;
    score += r.changed ? 0.5 : 0.0;
    return rb_utils::clamp01(score);
}

// ---------------------------------------------------------------------------
// Parameter grid
// ---------------------------------------------------------------------------

// Sweep a parameter across a range and return the best value by score.
double sweepBest(const std::function<double(double)>& scorer, double lo,
                 double hi, int steps) {
    if (steps < 2) return lo;
    double bestV = lo;
    double bestS = -1e18;
    for (int i = 0; i <= steps; ++i) {
        double v = lo + (hi - lo) * static_cast<double>(i) /
                             static_cast<double>(steps);
        double s = scorer(v);
        if (s > bestS) {
            bestS = s;
            bestV = v;
        }
    }
    return bestV;
}

// Grid of queue-wait options by difficulty.
std::vector<double> queueWaitGrid(double difficulty) {
    std::vector<double> grid;
    double base = 30.0 + difficulty * 60.0;
    for (int i = 0; i < 5; ++i) {
        grid.push_back(base + static_cast<double>(i) * 15.0);
    }
    return grid;
}

// Grid of desired win-rate targets by aggression.
std::vector<double> winRateGrid(AggressionLevel aggr) {
    std::vector<double> grid;
    double center = 0.6;
    switch (aggr) {
        case AggressionLevel::kConservative: center = 0.7; break;
        case AggressionLevel::kBalanced: center = 0.62; break;
        case AggressionLevel::kAggressive: center = 0.56; break;
        case AggressionLevel::kExtreme: center = 0.5; break;
    }
    for (int i = -2; i <= 2; ++i) {
        grid.push_back(rb_utils::clamp(center + static_cast<double>(i) * 0.02,
                                       0.3, 0.95));
    }
    return grid;
}

// Normalize a parameter to a 0..1 effort score (for comparison).
double effortScore(double value, double lo, double hi) {
    return rb_utils::normalize(value, lo, hi);
}

// Config fingerprint: stable hash-ish string of key parameters.
std::string configFingerprint(const RbConfig& cfg) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%d|%.2f|%.1f|%.0f|%d|%.0f",
             static_cast<int>(cfg.aggression), cfg.desiredWinRate,
             cfg.maxQueueSeconds, cfg.mmrTarget, cfg.guardSensitivity,
             cfg.sigmaFloor);
    return std::string(buf);
}

// Parameter delta magnitude between two configs.
double configDelta(const RbConfig& a, const RbConfig& b) {
    double d = 0.0;
    d += std::fabs(a.desiredWinRate - b.desiredWinRate) * 50.0;
    d += std::fabs(a.maxQueueSeconds - b.maxQueueSeconds) / 10.0;
    d += std::fabs(a.sigmaFloor - b.sigmaFloor) / 50.0;
    d += std::fabs(a.mmrTarget - b.mmrTarget) / 100.0;
    return d;
}

// Stability check: config unchanged across recent runs.
bool configStable(const std::vector<std::string>& fingerprints,
                  int window) {
    if (fingerprints.size() < 2) return false;
    size_t start = fingerprints.size() >= static_cast<size_t>(window)
                       ? fingerprints.size() - static_cast<size_t>(window)
                       : 0;
    const std::string& ref = fingerprints[start];
    for (size_t i = start + 1; i < fingerprints.size(); ++i) {
        if (fingerprints[i] != ref) return false;
    }
    return true;
}

// Recommendation ranking of candidate configs (by score desc).
std::vector<RbConfig> rankConfigs(const std::vector<RbConfig>& candidates,
                                  const RbConfig& baseline) {
    std::vector<RbConfig> ranked = candidates;
    std::sort(ranked.begin(), ranked.end(),
              [&baseline](const RbConfig& a, const RbConfig& b) {
                  auto score = [&baseline](const RbConfig& c) {
                      double s = 0.0;
                      s += std::fabs(c.desiredWinRate - 0.6);
                      s += std::fabs(c.maxQueueSeconds - 60.0) / 150.0;
                      s += std::fabs(c.mmrTarget - baseline.mmrTarget) / 1000.0;
                      return -s;
                  };
                  return score(a) > score(b);
              });
    return ranked;
}

}  // namespace rb
}  // namespace arift