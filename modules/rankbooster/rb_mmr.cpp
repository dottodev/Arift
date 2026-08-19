#include "rb_api.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "rb_math.h"

namespace arift {
namespace rb {

// ---------------------------------------------------------------------------
// MMR prediction, trend analysis and ladder projection.
// ---------------------------------------------------------------------------

namespace {

double linregSlope(const std::vector<double>& ys) {
    size_t n = ys.size();
    if (n < 2) return 0.0;
    double sumX = 0.0, sumY = 0.0, sumXY = 0.0, sumXX = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double x = static_cast<double>(i);
        sumX += x;
        sumY += ys[i];
        sumXY += x * ys[i];
        sumXX += x * x;
    }
    double denom = n * sumXX - sumX * sumX;
    if (std::fabs(denom) < 1e-9) return 0.0;
    return (n * sumXY - sumX * sumY) / denom;
}

}  // namespace

// MMR trend summary over a window of matches (declared in rb_math.h).

MmrTrend analyzeTrend(const PlayerProfile& p,
                      const std::vector<MatchRecord>& history,
                      size_t window) {
    MmrTrend t;
    std::vector<double> series;
    double r = p.mmr;
    // Reconstruct series in chronological order.
    std::vector<MatchRecord> chrono = history;
    std::sort(chrono.begin(), chrono.end(),
              [](const MatchRecord& a, const MatchRecord& b) {
                  return a.endedAtMs < b.endedAtMs;
              });
    size_t start = chrono.size() > window ? chrono.size() - window : 0;
    for (size_t i = start; i < chrono.size(); ++i) {
        r -= chrono[i].mmrDelta;
        series.push_back(r);
    }
    series.push_back(p.mmr);

    t.window = series.size();
    if (series.size() < 2) return t;
    t.slope = linregSlope(series);
    t.intercept = series[0];
    t.projected50 = p.mmr + t.slope * 50.0;
    t.projected100 = p.mmr + t.slope * 100.0;

    // Variance of deltas.
    std::vector<double> deltas;
    for (size_t i = 1; i < series.size(); ++i) {
        deltas.push_back(series[i] - series[i - 1]);
    }
    double m = 0.0;
    for (double d : deltas) m += d;
    m /= static_cast<double>(deltas.size());
    double acc = 0.0;
    for (double d : deltas) acc += (d - m) * (d - m);
    t.variance = acc / static_cast<double>(deltas.size());
    t.stability = rb_utils::clamp01(1.0 - t.variance / 900.0);
    t.improving = t.slope > 0.0;
    return t;
}

// Predicted MMR after playing n more matches, given current trend.
double predictMmrAfter(const MmrTrend& trend, size_t n) {
    return trend.intercept + trend.slope * (static_cast<double>(trend.window) +
                                            static_cast<double>(n));
}

// Ladder projection: how many matches to reach a target rank.
LadderProjection projectLadder(const PlayerProfile& p,
                               const std::vector<MatchRecord>& history,
                               const RankPoint& target,
                               double matchesPerDay) {
    LadderProjection proj;
    proj.targetRankStr = target.toString();
    proj.matchesPerDay = matchesPerDay;

    int currentAbs = p.rank.absolute();
    int targetAbs = target.absolute();
    int gap = targetAbs - currentAbs;
    if (gap <= 0) {
        proj.matchesToTarget = 0;
        proj.daysToTarget = 0;
        proj.probabilityOfReaching = 1.0;
        return proj;
    }

    MmrTrend trend = analyzeTrend(p, history, 30);
    double perMatch = trend.slope > 0.0 ? trend.slope : 4.0;
    if (perMatch < 1.0) perMatch = 1.0;
    proj.matchesToTarget = static_cast<int>(std::ceil(gap / perMatch));
    proj.daysToTarget = matchesPerDay > 0.0
                            ? static_cast<int>(std::ceil(
                                  static_cast<double>(proj.matchesToTarget) /
                                  matchesPerDay))
                            : 0;
    // Probability heuristic: smooth based on trend stability.
    double stability = trend.stability;
    double pReach = 0.35 + 0.55 * stability;
    pReach *= rb_utils::clamp01(1.0 - static_cast<double>(proj.matchesToTarget) / 400.0);
    proj.probabilityOfReaching = rb_utils::clamp01(pReach);
    return proj;
}

// Confidence-weighted MMR: how much to trust the current rating.
double effectiveMmr(const PlayerProfile& p, double ratingThreshold) {
    double conf = 1.0 - p.sigma / 350.0;
    if (conf < ratingThreshold) {
        // Not enough data — regress toward provisional estimate.
        double provisional = 900.0 + static_cast<double>(p.rank.absolute()) * 12.0;
        return rb_utils::lerp(provisional, p.mmr, conf);
    }
    return p.mmr;
}

// Opponent pool analysis: given enemy MMRs, what's our expected outcome?
PoolAnalysis analyzePool(const std::vector<double>& opponentMmrs,
                         double myMmr, double sigmaCombined) {
    PoolAnalysis a;
    if (opponentMmrs.empty()) return a;
    double sum = 0.0;
    double minV = opponentMmrs[0];
    double maxV = opponentMmrs[0];
    std::vector<double> probs;
    for (size_t i = 0; i < opponentMmrs.size(); ++i) {
        sum += opponentMmrs[i];
        if (opponentMmrs[i] < minV) minV = opponentMmrs[i];
        if (opponentMmrs[i] > maxV) maxV = opponentMmrs[i];
        double p = winProbability(myMmr, opponentMmrs[i], sigmaCombined);
        probs.push_back(p);
    }
    a.avgOpponentMmr = sum / static_cast<double>(opponentMmrs.size());
    a.minOpponentMmr = minV;
    a.maxOpponentMmr = maxV;
    a.expectedWinRate = 0.0;
    double best = -1.0;
    for (size_t i = 0; i < probs.size(); ++i) {
        a.expectedWinRate += probs[i];
        if (probs[i] > best) {
            best = probs[i];
            a.bestValueMatchIdx = i;
        }
    }
    a.expectedWinRate /= static_cast<double>(probs.size());
    // Pool sigma.
    double acc = 0.0;
    for (double o : opponentMmrs) {
        double d = o - a.avgOpponentMmr;
        acc += d * d;
    }
    a.sigmaOfPool = std::sqrt(acc / static_cast<double>(opponentMmrs.size()));
    return a;
}

// Star economy: win/loss star rates per tier.
StarEconomy economyForTier(RankTier tier) {
    StarEconomy e;
    e.tier = static_cast<int>(tier);
    switch (tier) {
        case RankTier::kWarrior:
            e.winStars = 2; e.lossStars = 1; e.promotionBonus = 1; break;
        case RankTier::kElite:
            e.winStars = 2; e.lossStars = 1; e.promotionBonus = 1; break;
        case RankTier::kMaster:
            e.winStars = 1; e.lossStars = 1; e.promotionBonus = 1; break;
        case RankTier::kGrandmaster:
            e.winStars = 1; e.lossStars = 1; e.promotionBonus = 1; break;
        case RankTier::kEpic:
            e.winStars = 1; e.lossStars = 1; e.promotionBonus = 1; break;
        case RankTier::kLegend:
            e.winStars = 1; e.lossStars = 1; e.promotionBonus = 1; break;
        default:
            e.winStars = 1; e.lossStars = 1; e.promotionBonus = 0; break;
    }
    return e;
}

// Session optimization (struct declared in rb_math.h).
SessionPlan optimizeSession(double winRate, int starsWin, int starsLoss) {
    SessionPlan s;
    double best = -1e9;
    for (int m = 2; m <= 12; ++m) {
        double fatigue = 0.02 * static_cast<double>(m - 2);
        double effectiveWR = winRate * (1.0 - fatigue);
        double stars = expectedStarsPerMatch(effectiveWR, starsWin, starsLoss);
        double total = stars * static_cast<double>(m);
        if (total > best) {
            best = total;
            s.matches = m;
            s.expectedStars = stars;
            s.fatigueFactor = fatigue;
            s.expectedStarsWithFatigue = total;
        }
    }
    return s;
}

// ---------------------------------------------------------------------------
// MMR analytics helpers
// ---------------------------------------------------------------------------

// Rating band transition estimate (e.g., Mythic -> Glory threshold).
double thresholdRating(RankTier tier) {
    switch (tier) {
        case RankTier::kWarrior: return 1000.0;
        case RankTier::kElite: return 1200.0;
        case RankTier::kMaster: return 1400.0;
        case RankTier::kGrandmaster: return 1600.0;
        case RankTier::kEpic: return 1850.0;
        case RankTier::kLegend: return 2150.0;
        case RankTier::kMythic: return 2450.0;
        case RankTier::kMythicalGlory: return 2800.0;
        case RankTier::kMythicalImmortal: return 3200.0;
        default: return 1500.0;
    }
}

// Match value: how much rating movement a match should produce.
double matchValue(double mmr, double sigma, double oppMmr) {
    double diff = std::fabs(mmr - oppMmr);
    double base = 16.0 + (350.0 - sigma) * 0.1;
    return base + diff * 0.01;
}

// Rating deviation range for a confidence band.
std::pair<double, double> ratingBand(double rating, double sigma) {
    return {rating - 2.0 * sigma, rating + 2.0 * sigma};
}

// MMR milestone: nearest next-tier threshold above current rating.
struct Milestone {
    RankTier tier = RankTier::kWarrior;
    double threshold = 0.0;
    double remaining = 0.0;
};

Milestone nextMilestone(const PlayerProfile& p) {
    Milestone m;
    int nextIdx = static_cast<int>(p.rank.tier) + 1;
    if (nextIdx >= static_cast<int>(RankTier::kCount)) {
        nextIdx = static_cast<int>(RankTier::kCount) - 1;
    }
    m.tier = static_cast<RankTier>(nextIdx);
    m.threshold = thresholdRating(m.tier);
    m.remaining = std::max(0.0, m.threshold - p.mmr);
    return m;
}

// Glicko-2 g() function.
double glickoG(double sigma) {
    double q = 0.0057565;
    double s2 = sigma * sigma;
    return 1.0 / std::sqrt(1.0 + 3.0 * q * q * s2 / (M_PI * M_PI));
}

// Expected score vs a group of opponents (simplified Glicko).
double groupExpected(const PlayerProfile& p,
                     const std::vector<double>& oppRatings,
                     const std::vector<double>& oppSigmas) {
    double sum = 0.0;
    size_t n = std::min(oppRatings.size(), oppSigmas.size());
    for (size_t i = 0; i < n; ++i) {
        double g = glickoG(oppSigmas[i]);
        double e = 1.0 / (1.0 + std::pow(10.0, -g * (p.mmr - oppRatings[i]) / 400.0));
        sum += e;
    }
    return n > 0 ? sum / static_cast<double>(n) : 0.5;
}

// Uncertainty-weighted rating (for comparisons against sigma-heavy accounts).
double uncertaintyAdjusted(double rating, double sigma) {
    return rating - sigma * 0.1;
}

// Rating convergence: how many more matches until sigma stabilizes.
double matchesUntilStable(double sigma, double floor) {
    if (sigma <= floor) return 0.0;
    double decay = std::max(0.15, 1.0 - (sigma - floor) / (350.0 - floor));
    return 1.0 / decay;
}

// MMR percentile rank within the account's region (heuristic).
double mmrPercentile(double mmr) {
    return rb_utils::clamp01((mmr - 1000.0) / 2500.0);
}

// Deviation ratio between two rating systems (elo vs glicko).
double eloGlickoRatio(double sigma) {
    return 1.0 + sigma / 400.0;
}

// Effective stars-per-win for a tier given current confidence.
double starsPerWinAdjusted(const StarEconomy& economy, double confidence) {
    double base = static_cast<double>(economy.winStars);
    return base + (1.0 - confidence) * 0.5;
}

// MMR volatility trend over recent matches.
double volatilityTrend(const std::vector<MatchRecord>& history, int n) {
    if (history.size() < 2) return 0.0;
    size_t start = history.size() >= static_cast<size_t>(n)
                       ? history.size() - static_cast<size_t>(n)
                       : 0;
    std::vector<double> deltas;
    for (size_t i = start; i < history.size(); ++i) {
        deltas.push_back(history[i].mmrDelta);
    }
    return rb_utils::standardDeviation(deltas);
}

}  // namespace rb
}  // namespace arift