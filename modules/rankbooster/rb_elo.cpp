#include "rb_api.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "rb_math.h"

namespace arift {
namespace rb {

// ---------------------------------------------------------------------------
// Extended Elo tools: distributions, calibration and win-probability models.
// ---------------------------------------------------------------------------

namespace {

// Cumulative distribution function of the standard normal.
double normCdf(double x) {
    return 0.5 * (1.0 + std::erf(x / std::sqrt(2.0)));
}

// Inverse normal CDF via Acklam's approximation.
double normInv(double p) {
    if (p <= 0.0) return -8.0;
    if (p >= 1.0) return 8.0;
    static const double a[] = {-3.969683028665376e+01, 2.209460984245205e+02,
                               -2.759285104469687e+02, 1.383577518672690e+02,
                               -3.066479806614716e+01, 2.506628277459239e+00};
    static const double b[] = {-5.447609879822406e+01, 1.615858368580409e+02,
                               -1.556989798598866e+02, 6.680131188771972e+01,
                               -1.328068155288572e+01};
    static const double c[] = {-7.784894002430293e-03, -3.223964580411365e-01,
                               -2.400758277161838e+00, -2.549732539343734e+00,
                               4.374664141464968e+00, 2.938163982698783e+00};
    static const double d[] = {7.784695709041462e-03, 3.224671290700398e-01,
                               2.445134137142996e+00, 3.754408661907416e+00};
    static const double p_low = 0.02425;
    static const double p_high = 1.0 - p_low;

    double q = 0.0;
    double r = 0.0;
    if (p < p_low) {
        q = std::sqrt(-2.0 * std::log(p));
        return (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q +
                c[5]) /
               ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0);
    } else if (p <= p_high) {
        q = p - 0.5;
        r = q * q;
        return (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r +
                a[5]) * q /
               (((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r +
                1.0);
    } else {
        q = std::sqrt(-2.0 * std::log(1.0 - p));
        return -(((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q +
                 c[5]) /
               ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0);
    }
}

double winProbFromRatingDiff(double diff, double sigmaCombined) {
    double z = -diff / sigmaCombined;
    return normCdf(z);
}

}  // namespace

// ---------------------------------------------------------------------------
// Win-probability model
// ---------------------------------------------------------------------------

double winProbability(const RankPoint& me, const RankPoint& opponent,
                      double sigmaCombined) {
    double diff = static_cast<double>(me.absolute() - opponent.absolute()) * 12.0;
    return winProbFromRatingDiff(diff, sigmaCombined);
}

double winProbability(double myMmr, double oppMmr, double sigmaCombined) {
    return winProbFromRatingDiff(myMmr - oppMmr, sigmaCombined);
}

// Rating required to reach a target win probability.
double ratingNeeded(double targetWinRate, double oppRating,
                    double sigmaCombined) {
    double z = normInv(targetWinRate);
    return oppRating - z * sigmaCombined;
}

// Expected stars gained per match for a given win rate.
double expectedStarsPerMatch(double winRate, int starsWin, int starsLoss) {
    return winRate * static_cast<double>(starsWin) -
           (1.0 - winRate) * static_cast<double>(starsLoss);
}

// Number of matches needed to climb `stars` at a given win rate.
double matchesNeeded(int stars, double winRate, int starsWin, int starsLoss) {
    double perMatch = expectedStarsPerMatch(winRate, starsWin, starsLoss);
    if (std::fabs(perMatch) < 1e-6) return 1e9;
    return static_cast<double>(stars) / perMatch;
}

// Elo rating distribution over time (for telemetry charts).
std::vector<double> ratingTrajectory(const PlayerProfile& p,
                                     const std::vector<MatchRecord>& history) {
    std::vector<double> out;
    double r = p.mmr;
    for (const auto& rec : history) {
        r += rec.mmrDelta;
        out.push_back(r);
    }
    return out;
}

// Calibration: given recent results, estimate true skill with confidence.
CalibrationResult calibrateSkill(const std::vector<MatchRecord>& history) {
    CalibrationResult out;
    double sum = 0.0;
    double sumSq = 0.0;
    double n = 0.0;
    for (const auto& rec : history) {
        if (!rec.result.ranked) continue;
        sum += rec.skillRating;
        sumSq += rec.skillRating * rec.skillRating;
        n += 1.0;
    }
    if (n < 4.0) {
        out.skillEstimate = 1500.0;
        out.confidence95Low = 1000.0;
        out.confidence95High = 2000.0;
        out.sampleSize = n;
        return out;
    }
    double mean = sum / n;
    double var = sumSq / n - mean * mean;
    if (var < 0.0) var = 0.0;
    double sd = std::sqrt(var);
    double se = sd / std::sqrt(n);
    out.skillEstimate = mean;
    out.confidence95Low = mean - 1.96 * se;
    out.confidence95High = mean + 1.96 * se;
    out.sampleSize = n;
    out.sufficient = n >= 10.0;
    return out;
}

// FIDE-style performance rating: rating such that expected score matches
// actual score against a given opponent pool.
double performanceRating(const std::vector<double>& opponentRatings,
                         double score, double rounds) {
    if (opponentRatings.empty() || rounds <= 0.0) return 1500.0;
    double avgOpp = 0.0;
    for (double r : opponentRatings) avgOpp += r;
    avgOpp /= static_cast<double>(opponentRatings.size());
    double actual = score / rounds;
    double expected = 0.0;
    double rating = avgOpp;
    // Iterate until convergence.
    for (int i = 0; i < 50; ++i) {
        expected = 1.0 / (1.0 + std::pow(10.0, (avgOpp - rating) / 400.0));
        double diff = 400.0 * std::log10(actual / std::max(expected, 1e-6));
        rating = avgOpp + diff;
    }
    return rating;
}

// Star-protection engine: converts losses into protection points.
int protectionForLoss(int tierIdx, int streak) {
    int base = 2 + tierIdx / 2;
    if (streak <= -3) base += 1;
    return base;
}

// Expected-value analysis of a matchmaking choice (declared in rb_math.h).
ChoiceAnalysis analyzeChoice(double myMmr, double oppMmr, double sigma,
                             int starsWin, int starsLoss) {
    ChoiceAnalysis a;
    a.winRate = winProbability(myMmr, oppMmr, sigma);
    a.starsPer100 = 100.0 * expectedStarsPerMatch(a.winRate, starsWin, starsLoss);
    a.risk = rb_utils::clamp01(1.0 - a.winRate);
    a.value = a.starsPer100 - a.risk * 20.0;
    return a;
}

// ---------------------------------------------------------------------------
// Extended elo math
// ---------------------------------------------------------------------------

// Normal CDF via Abramowitz-Stegun (duplicated locally for portability).
double localNormCdf(double x) {
    return normCdf(x);
}

// Star-protection accrual: points gained per loss in a tier.
int protectionAccrual(RankTier tier) {
    switch (tier) {
        case RankTier::kLegend:
        case RankTier::kEpic:
        case RankTier::kGrandmaster: return 2;
        default: return 1;
    }
}

// Protection burn threshold: losses to burn before losing stars.
int protectionBurnThreshold(RankTier tier, int protection) {
    int cap = tier >= RankTier::kEpic ? 4 : 2;
    return std::max(0, cap - protection);
}

// Elo delta curve with momentum bonus for streaks.
double momentumEloDelta(int streak, double baseDelta) {
    if (streak > 3) return baseDelta * 1.2;
    if (streak < -3) return baseDelta * 0.8;
    return baseDelta;
}

// Deviation penalty: how much off-rating play costs.
double deviationPenalty(double rating, double oppRating, double band) {
    double dev = std::fabs(rating - oppRating);
    if (dev <= band) return 0.0;
    return (dev - band) * 0.02;
}

// Equivalent rating difference for a win probability.
double ratingDiffForWinRate(double p) {
    p = rb_utils::clamp01(p);
    if (p <= 0.0) return 1e9;
    if (p >= 1.0) return -1e9;
    return -400.0 * std::log10((1.0 - p) / p);
}

// Stability window: rating change needed to trigger rank recalibration.
double recalibrationDelta(double sigma) {
    return 25.0 + (350.0 - sigma) * 0.15;
}

// Expected stars from a win rate and tier economy.
double expectedStars(double winRate, const StarEconomy& econ) {
    return winRate * static_cast<double>(econ.winStars) -
           (1.0 - winRate) * static_cast<double>(econ.lossStars);
}

// Normalized performance score from KDA (0..100).
double performanceFromKda(int kills, int deaths, int assists,
                          double durationMin) {
    double kda = deaths > 0
                     ? static_cast<double>(kills + assists) /
                           static_cast<double>(deaths)
                     : static_cast<double>(kills + assists);
    double tempo = durationMin > 0.0
                       ? static_cast<double>(kills + assists) / durationMin
                       : 0.0;
    double score = 40.0 + kda * 8.0 + tempo * 12.0;
    return rb_utils::clamp(score, 0.0, 100.0);
}

// Rank parity check for a lobby (tier spread heuristic).
int rankSpread(const std::vector<RankPoint>& ranks) {
    if (ranks.empty()) return 0;
    int minAbs = ranks[0].absolute();
    int maxAbs = ranks[0].absolute();
    for (const auto& r : ranks) {
        int a = r.absolute();
        if (a < minAbs) minAbs = a;
        if (a > maxAbs) maxAbs = a;
    }
    return maxAbs - minAbs;
}

// Rating projection after N matches at a constant delta.
double projectedRating(double start, double avgDelta, double n) {
    return start + avgDelta * n;
}

// Protection bonus: stars saved by protection over a window.
double protectionSavings(int protectionsUsed, double starsPerWin) {
    return static_cast<double>(protectionsUsed) * starsPerWin;
}

}  // namespace rb
}  // namespace arift