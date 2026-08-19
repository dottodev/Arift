#include "rb_api.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

#include "rb_math.h"

namespace arift {
namespace rb {

// ---------------------------------------------------------------------------
// Extended statistics: histograms, distributions, match analysis.
// ---------------------------------------------------------------------------

void Histogram::build(const std::vector<double>& values, double lo, double hi,
                      int n) {
    min = lo;
    max = hi;
    buckets = n;
    counts.assign(static_cast<size_t>(n), 0);
    centers.resize(static_cast<size_t>(n));
    double step = (hi - lo) / static_cast<double>(n);
    for (int i = 0; i < n; ++i) {
        centers[static_cast<size_t>(i)] = lo + (static_cast<double>(i) + 0.5) * step;
    }
    for (double v : values) {
        if (v < lo || v >= hi) continue;
        int idx = static_cast<int>((v - lo) / step);
        if (idx < 0) idx = 0;
        if (idx >= n) idx = n - 1;
        counts[static_cast<size_t>(idx)] += 1;
    }
}

int Histogram::total() const {
    int t = 0;
    for (int c : counts) t += c;
    return t;
}

double Histogram::peak() const {
    int best = 0;
    for (size_t i = 0; i < counts.size(); ++i) {
        if (counts[i] > counts[best]) best = static_cast<int>(i);
    }
    return centers[static_cast<size_t>(best)];
}

// KDA distribution over match history.
Histogram kdaHistogram(const std::vector<MatchRecord>& history) {
    std::vector<double> kdas;
    for (const auto& rec : history) {
        double kda = rec.result.kdaDeaths > 0
                         ? static_cast<double>(rec.result.kdaKills +
                                               rec.result.kdaAssists) /
                               static_cast<double>(rec.result.kdaDeaths)
                         : static_cast<double>(rec.result.kdaKills +
                                               rec.result.kdaAssists);
        kdas.push_back(kda);
    }
    Histogram h;
    h.build(kdas, 0.0, 12.0, 12);
    return h;
}

// Performance percentile distribution.
Histogram performanceHistogram(const std::vector<MatchRecord>& history) {
    std::vector<double> perfs;
    for (const auto& rec : history) {
        perfs.push_back(static_cast<double>(rec.result.performancePercentile));
    }
    Histogram h;
    h.build(perfs, 0.0, 100.0, 10);
    return h;
}

// Match-duration distribution.
Histogram durationHistogram(const std::vector<MatchRecord>& history) {
    std::vector<double> durations;
    for (const auto& rec : history) {
        durations.push_back(rec.result.durationMin);
    }
    Histogram h;
    h.build(durations, 5.0, 30.0, 10);
    return h;
}

// Win rate vs. enemy MMR band (for matchmaking tuning).
MmrBandWinRates winRateByMmrBand(const std::vector<MatchRecord>& history,
                                 double myMmr) {
    MmrBandWinRates out;
    std::vector<double> centers;
    std::vector<int> counts(8, 0);
    std::vector<int> wins(8, 0);
    for (int i = 0; i < 8; ++i) {
        centers.push_back(-400.0 + static_cast<double>(i) * 120.0);
    }
    for (const auto& rec : history) {
        // Reconstruct opponent MMR from delta heuristic.
        double oppDiff = -rec.mmrDelta * 20.0;
        int band = static_cast<int>((oppDiff + 400.0) / 120.0);
        if (band < 0) band = 0;
        if (band > 7) band = 7;
        counts[static_cast<size_t>(band)] += 1;
        if (rec.result.won) wins[static_cast<size_t>(band)] += 1;
    }
    for (size_t i = 0; i < centers.size(); ++i) {
        out.bandCenters.push_back(centers[i]);
        out.winRates.push_back(counts[i] > 0
                                   ? static_cast<double>(wins[i]) /
                                         static_cast<double>(counts[i])
                                   : 0.5);
        out.sampleCounts.push_back(counts[i]);
    }
    return out;
}

// Consistency score: how stable is performance between matches.
double consistencyScore(const std::vector<MatchRecord>& history) {
    std::vector<double> perfs;
    for (const auto& rec : history) {
        perfs.push_back(static_cast<double>(rec.result.performancePercentile));
    }
    if (perfs.size() < 3) return 0.5;
    double sd = rb_utils::standardDeviation(perfs);
    return rb_utils::clamp01(1.0 - sd / 25.0);
}

// Peak performance window detection (when the player plays best).
PerformanceWindow bestPerformanceWindow(const std::vector<MatchRecord>& history) {
    PerformanceWindow best;
    double bestScore = -1.0;
    for (int start = 0; start < 24; start += 3) {
        int end = start + 3;
        int samples = 0;
        int wins = 0;
        double perfSum = 0.0;
        for (const auto& rec : history) {
            time_t sec = static_cast<time_t>(rec.endedAtMs / 1000);
            struct tm tmv{};
            localtime_r(&sec, &tmv);
            int hour = tmv.tm_hour;
            bool inWindow = start <= end ? (hour >= start && hour < end)
                                         : (hour >= start || hour < end);
            if (!inWindow) continue;
            samples += 1;
            if (rec.result.won) wins += 1;
            perfSum += static_cast<double>(rec.result.performancePercentile);
        }
        if (samples < 2) continue;
        double wr = static_cast<double>(wins) / static_cast<double>(samples);
        double perf = perfSum / static_cast<double>(samples);
        double score = wr * 0.6 + (perf / 100.0) * 0.4;
        if (score > bestScore) {
            bestScore = score;
            best.startHour = start;
            best.endHour = end;
            best.winRate = wr;
            best.avgPerformance = perf;
            best.samples = samples;
        }
    }
    if (best.samples == 0) {
        best.winRate = 0.5;
        best.avgPerformance = 50.0;
    }
    return best;
}

// Burst analysis: are wins/losses clustered (streak-heavy) or uniform?
BurstAnalysis analyzeBursts(const std::vector<MatchRecord>& history) {
    BurstAnalysis out;
    int winRun = 0;
    int lossRun = 0;
    int transitions = 0;
    int prevWon = -1;
    for (const auto& rec : history) {
        if (rec.result.won) {
            winRun += 1;
            lossRun = 0;
            if (winRun > out.maxConsecutiveWins) out.maxConsecutiveWins = winRun;
        } else {
            lossRun += 1;
            winRun = 0;
            if (lossRun > out.maxConsecutiveLosses) out.maxConsecutiveLosses = lossRun;
        }
        int cur = rec.result.won ? 1 : 0;
        if (prevWon >= 0 && cur != prevWon) transitions += 1;
        prevWon = cur;
    }
    if (history.size() < 2) return out;
    double expectedTransitions =
        static_cast<double>(history.size() - 1) * 0.5;
    out.burstiness = rb_utils::clamp01(
        static_cast<double>(transitions) / std::max(expectedTransitions, 1.0));
    out.burstCount = transitions + 1;
    return out;
}

// Difficulty adaptation: given recent outcomes, recommend MMR targeting.
double recommendedTargetDelta(const RbStatistics& stats, double currentMmr,
                              double desiredWinRate) {
    double current = stats.winRate;
    double delta = (desiredWinRate - current) * 600.0;
    return rb_utils::clamp(delta, -350.0, 350.0);
}

// Confidence interval of win rate (Wilson score).
WilsonInterval wilsonInterval(int wins, int total, double z) {
    WilsonInterval out;
    if (total == 0) {
        out.lower = 0.0;
        out.upper = 1.0;
        out.estimate = 0.5;
        return out;
    }
    double p = static_cast<double>(wins) / static_cast<double>(total);
    double denom = 1.0 + z * z / static_cast<double>(total);
    double centre = (p + z * z / (2.0 * static_cast<double>(total))) / denom;
    double margin = z * std::sqrt(p * (1.0 - p) / static_cast<double>(total) +
                                  z * z / (4.0 * static_cast<double>(total) *
                                           static_cast<double>(total))) /
                    denom;
    out.estimate = p;
    out.lower = std::max(0.0, centre - margin);
    out.upper = std::min(1.0, centre + margin);
    return out;
}

// Aggregate statistics string for telemetry/UI.
std::string statisticsReport(const PlayerProfile& p,
                             const std::vector<MatchRecord>& history) {
    RbStatistics stats;
    stats.compute(p, history);
    WilsonInterval wi = wilsonInterval(static_cast<int>(stats.wins),
                                       static_cast<int>(stats.totalMatches));
    Histogram perf = performanceHistogram(history);
    BurstAnalysis burst = analyzeBursts(history);

    char buf[1024];
    snprintf(buf, sizeof(buf),
             "win_rate=%.1f%% (ci %.1f..%.1f) consistency=%.2f "
             "burstiness=%.2f perf_peak=%.1f\n"
             "perf_hist[%d]=%d,%d,%d,%d,%d,%d,%d,%d,%d",
             stats.winRate * 100.0, wi.lower * 100.0, wi.upper * 100.0,
             consistencyScore(history), burst.burstiness,
             performanceHistogram(history).peak(),
             perf.buckets, perf.counts[0], perf.counts[1], perf.counts[2],
             perf.counts[3], perf.counts[4], perf.counts[5], perf.counts[6],
             perf.counts[7], perf.counts[8]);
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Deep statistics
// ---------------------------------------------------------------------------

// Performance momentum: percentile trend over the last N matches.
double performanceMomentum(const std::vector<MatchRecord>& history, int n) {
    if (history.size() < 2) return 0.0;
    size_t start = history.size() >= static_cast<size_t>(n)
                       ? history.size() - static_cast<size_t>(n)
                       : 0;
    std::vector<double> series;
    for (size_t i = start; i < history.size(); ++i) {
        series.push_back(static_cast<double>(history[i].result.performancePercentile));
    }
    if (series.size() < 2) return 0.0;
    size_t half = series.size() / 2;
    double early = rb_utils::mean(
        std::vector<double>(series.begin(),
                            series.begin() + static_cast<long>(half)));
    double late = rb_utils::mean(
        std::vector<double>(series.begin() + static_cast<long>(half),
                            series.end()));
    return late - early;
}

// KDA volatility: spread of KDA values (high = feast or famine).
double kdaVolatility(const std::vector<MatchRecord>& history) {
    std::vector<double> kdas;
    for (const auto& rec : history) {
        double kda = rec.result.kdaDeaths > 0
                         ? static_cast<double>(rec.result.kdaKills +
                                               rec.result.kdaAssists) /
                               static_cast<double>(rec.result.kdaDeaths)
                         : static_cast<double>(rec.result.kdaKills +
                                               rec.result.kdaAssists);
        kdas.push_back(kda);
    }
    return rb_utils::standardDeviation(kdas);
}

// First-blood frequency: share of matches with >= 1 early kill.
double earlyLeadRate(const std::vector<MatchRecord>& history) {
    if (history.empty()) return 0.0;
    int lead = 0;
    for (const auto& rec : history) {
        if (rec.result.kdaKills >= 1 && rec.result.teamScore >= rec.result.enemyScore) {
            lead += 1;
        }
    }
    return static_cast<double>(lead) / static_cast<double>(history.size());
}

// Late-game collapse rate: losses where we led at some point.
double lateCollapseRate(const std::vector<MatchRecord>& history) {
    if (history.empty()) return 0.0;
    int collapsed = 0;
    int sample = 0;
    for (const auto& rec : history) {
        if (rec.result.teamScore > rec.result.enemyScore) {
            sample += 1;
            if (!rec.result.won) collapsed += 1;
        }
    }
    return sample > 0 ? static_cast<double>(collapsed) /
                            static_cast<double>(sample) : 0.0;
}

// Rollercoaster index: variance of streak lengths (entropy of outcomes).
double rollercoasterIndex(const std::vector<MatchRecord>& history) {
    if (history.size() < 4) return 0.0;
    std::vector<int> runs;
    int cur = 0;
    bool prevWon = history[0].result.won;
    for (const auto& rec : history) {
        if (rec.result.won == prevWon) {
            cur += 1;
        } else {
            runs.push_back(cur);
            cur = 1;
            prevWon = rec.result.won;
        }
    }
    runs.push_back(cur);
    std::vector<double> asDoubles(runs.begin(), runs.end());
    return rb_utils::clamp01(rb_utils::standardDeviation(asDoubles) / 3.0);
}

// Expected-value per match of the current win rate.
double expectedValuePerMatch(const RbStatistics& stats,
                             const StarEconomy& economy) {
    double ev = stats.winRate * static_cast<double>(economy.winStars) -
                (1.0 - stats.winRate) * static_cast<double>(economy.lossStars);
    return ev;
}

// Star projection report combining several models.
StarProjection projectStars(const PlayerProfile& p,
                            const RbStatistics& stats,
                            const RankPoint& target,
                            const StarEconomy& economy) {
    StarProjection out;
    out.evPerMatch = expectedValuePerMatch(stats, economy);
    int starsNeeded = std::max(0, target.absolute() - p.rank.absolute());
    out.matchesToTarget = out.evPerMatch > 0.0
                              ? static_cast<double>(starsNeeded) / out.evPerMatch
                              : 1e9;
    out.daysToTarget = out.matchesToTarget / 8.0;
    out.confidence = rb_utils::clamp01(
        1.0 - stats.sigma / 350.0 - (out.matchesToTarget > 1000.0 ? 0.5 : 0.0));
    char buf[256];
    snprintf(buf, sizeof(buf),
             "ev=%.2f stars/match, %d stars to %s -> %.0f matches (%.1f days)",
             out.evPerMatch, starsNeeded, target.toString().c_str(),
             out.matchesToTarget, out.daysToTarget);
    out.text = buf;
    return out;
}

// Opponent-strength profile from pool history.
struct OpponentProfile {
    double avgMmr = 1500.0;
    double variance = 100.0;
    double hardestMmr = 0.0;
    double easiestMmr = 0.0;
};

OpponentProfile opponentProfile(const std::vector<MatchRecord>& history,
                                double myMmr) {
    OpponentProfile o;
    std::vector<double> opps;
    for (const auto& rec : history) {
        double opp = myMmr - rec.mmrDelta * 20.0;
        opps.push_back(opp);
    }
    if (!opps.empty()) {
        o.avgMmr = rb_utils::mean(opps);
        o.variance = rb_utils::variance(opps);
        o.hardestMmr = *std::max_element(opps.begin(), opps.end());
        o.easiestMmr = *std::min_element(opps.begin(), opps.end());
    }
    return o;
}

// Session efficiency: stars earned per minute played.
double sessionEfficiency(const RbStatistics& stats, double starsEarned) {
    double minutes = stats.avgMatchDuration *
                     static_cast<double>(stats.totalMatches);
    if (minutes <= 0.0) return 0.0;
    return starsEarned / minutes;
}

// ---------------------------------------------------------------------------
// Quantile and distribution analysis
// ---------------------------------------------------------------------------

namespace {

// Cumulative distribution of a value series.
std::vector<double> cdfOf(const std::vector<double>& values) {
    std::vector<double> sorted = values;
    std::sort(sorted.begin(), sorted.end());
    std::vector<double> cdf;
    cdf.reserve(sorted.size());
    double n = static_cast<double>(std::max<size_t>(sorted.size(), 1));
    for (size_t i = 0; i < sorted.size(); ++i) {
        cdf.push_back(static_cast<double>(i + 1) / n);
    }
    return cdf;
}

}  // namespace

// Quantile function (inverse CDF) of a value series.
double quantile(const std::vector<double>& values, double q) {
    if (values.empty()) return 0.0;
    std::vector<double> sorted = values;
    std::sort(sorted.begin(), sorted.end());
    double idx = q * static_cast<double>(sorted.size() - 1);
    size_t lo = static_cast<size_t>(std::floor(idx));
    size_t hi = static_cast<size_t>(std::ceil(idx));
    if (lo == hi) return sorted[lo];
    double frac = idx - static_cast<double>(lo);
    return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
}

// Interquartile range (robust spread measure).
double iqr(const std::vector<double>& values) {
    return quantile(values, 0.75) - quantile(values, 0.25);
}

// Skewness of a value series.
double skewness(const std::vector<double>& values) {
    if (values.size() < 3) return 0.0;
    double m = rb_utils::mean(values);
    double sd = rb_utils::standardDeviation(values);
    if (sd <= 0.0) return 0.0;
    double acc = 0.0;
    for (double v : values) {
        double d = (v - m) / sd;
        acc += d * d * d;
    }
    return acc / static_cast<double>(values.size());
}

// Kurtosis (tail weight) of a value series.
double kurtosis(const std::vector<double>& values) {
    if (values.size() < 4) return 0.0;
    double m = rb_utils::mean(values);
    double sd = rb_utils::standardDeviation(values);
    if (sd <= 0.0) return 0.0;
    double acc = 0.0;
    for (double v : values) {
        double d = (v - m) / sd;
        acc += d * d * d * d;
    }
    return acc / static_cast<double>(values.size()) - 3.0;
}

// Outlier count beyond k standard deviations.
int outlierCount(const std::vector<double>& values, double k) {
    if (values.size() < 4) return 0;
    double m = rb_utils::mean(values);
    double sd = rb_utils::standardDeviation(values);
    if (sd <= 0.0) return 0;
    int n = 0;
    for (double v : values) {
        if (std::fabs(v - m) > k * sd) n += 1;
    }
    return n;
}

// Autocorrelation at lag 1 (persistence of performance).
double autocorrelationLag1(const std::vector<double>& values) {
    if (values.size() < 4) return 0.0;
    double m = rb_utils::mean(values);
    double num = 0.0;
    double den = 0.0;
    for (size_t i = 1; i < values.size(); ++i) {
        num += (values[i - 1] - m) * (values[i] - m);
        den += (values[i] - m) * (values[i] - m);
    }
    if (den <= 0.0) return 0.0;
    return num / den;
}

// Regime detection: stable, trending, or volatile performance.
const char* performanceRegime(const std::vector<double>& values) {
    if (values.size() < 5) return "insufficient";
    double sd = rb_utils::standardDeviation(values);
    double ac1 = autocorrelationLag1(values);
    if (ac1 > 0.5 && sd < 8.0) return "stable";
    if (ac1 > 0.3 && sd >= 8.0) return "trending";
    if (ac1 < -0.2) return "alternating";
    return "volatile";
}

// Match-quality histogram from performance + outcome pairs.
struct QualityHistogram {
    std::vector<int> bins;
    double avg = 0.0;
};

QualityHistogram qualityHistogram(const std::vector<MatchRecord>& history) {
    QualityHistogram q;
    q.bins.assign(10, 0);
    std::vector<double> qs;
    for (const auto& rec : history) {
        double quality = static_cast<double>(rec.result.performancePercentile) *
                         (rec.result.won ? 1.0 : 0.6);
        qs.push_back(quality);
        int idx = static_cast<int>(quality / 10.0);
        if (idx > 9) idx = 9;
        q.bins[static_cast<size_t>(idx)] += 1;
    }
    q.avg = rb_utils::mean(qs);
    return q;
}

// Time-series of win/loss for plotting (1 = win, 0 = loss).
std::vector<int> outcomeSeries(const std::vector<MatchRecord>& history) {
    std::vector<int> out;
    out.reserve(history.size());
    for (const auto& rec : history) out.push_back(rec.result.won ? 1 : 0);
    return out;
}

// Rolling win-rate series over a window.
std::vector<double> rollingWinRateSeries(const std::vector<MatchRecord>& history,
                                         int window) {
    std::vector<double> out;
    for (size_t i = 0; i < history.size(); ++i) {
        size_t start = i >= static_cast<size_t>(window) ? i - window + 1 : 0;
        int wins = 0;
        for (size_t j = start; j <= i; ++j) {
            if (history[j].result.won) wins += 1;
        }
        out.push_back(static_cast<double>(wins) /
                      static_cast<double>(i - start + 1));
    }
    return out;
}

// Best improvement window: largest win-rate gain over a span.
struct ImprovementWindow {
    size_t start = 0;
    size_t end = 0;
    double gain = 0.0;
};

ImprovementWindow bestImprovement(const std::vector<MatchRecord>& history,
                                  int span) {
    ImprovementWindow best;
    std::vector<double> wr = rollingWinRateSeries(history, span);
    for (size_t i = 0; i + 1 < wr.size(); ++i) {
        double gain = wr[i + 1] - wr[i];
        if (gain > best.gain) {
            best.gain = gain;
            best.start = i;
            best.end = i + 1;
        }
    }
    return best;
}

}  // namespace rb
}  // namespace arift