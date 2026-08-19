#include "rb_api.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>

#include "arift_utils.h"

namespace arift {
namespace rb {
namespace rb_utils {

double clamp01(double v) {
    return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
}

double clamp(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

int clampInt(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

double lerp(double a, double b, double t) {
    return a + (b - a) * t;
}

double damp(double current, double target, double lambda, double dt) {
    return lerp(current, target, 1.0 - std::exp(-lambda * dt));
}

double sigmoid(double x) {
    return 1.0 / (1.0 + std::exp(-x));
}

double logit(double p) {
    p = clamp01(p);
    if (p <= 0.0) return -10.0;
    if (p >= 1.0) return 10.0;
    return std::log(p / (1.0 - p));
}

double percentile(const std::vector<double>& values, double pct) {
    if (values.empty()) return 0.0;
    std::vector<double> sorted = values;
    std::sort(sorted.begin(), sorted.end());
    double idx = pct * static_cast<double>(sorted.size() - 1);
    size_t lo = static_cast<size_t>(std::floor(idx));
    size_t hi = static_cast<size_t>(std::ceil(idx));
    if (lo == hi) return sorted[lo];
    double frac = idx - static_cast<double>(lo);
    return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
}

double median(std::vector<double> values) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    size_t n = values.size();
    if (n % 2 == 1) return values[n / 2];
    return (values[n / 2 - 1] + values[n / 2]) * 0.5;
}

double mean(const std::vector<double>& values) {
    if (values.empty()) return 0.0;
    double sum = 0.0;
    for (double v : values) sum += v;
    return sum / static_cast<double>(values.size());
}

double variance(const std::vector<double>& values) {
    if (values.size() < 2) return 0.0;
    double m = mean(values);
    double acc = 0.0;
    for (double v : values) {
        double d = v - m;
        acc += d * d;
    }
    return acc / static_cast<double>(values.size() - 1);
}

double standardDeviation(const std::vector<double>& values) {
    return std::sqrt(variance(values));
}

int64_t hoursBetween(int64_t a, int64_t b) {
    int64_t diff = b - a;
    if (diff < 0) diff = -diff;
    return diff / 3600000;
}

std::string timestamp(int64_t ms) {
    time_t sec = static_cast<time_t>(ms / 1000);
    struct tm tmv{};
    localtime_r(&sec, &tmv);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
    return std::string(buf);
}

std::string kdaString(const MatchResult& r) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%d/%d/%d", r.kdaKills, r.kdaDeaths, r.kdaAssists);
    return std::string(buf);
}

double starEquivalents(const RankPoint& a, const RankPoint& b) {
    return static_cast<double>(b.absolute() - a.absolute());
}

bool isSameTier(const RankPoint& a, const RankPoint& b) {
    return a.tier == b.tier;
}

int tierDistance(const RankPoint& a, const RankPoint& b) {
    return static_cast<int>(a.tier) - static_cast<int>(b.tier);
}

double gaussianRandom(double mean, double sigma) {
    static std::mt19937_64 gen(std::random_device{}());
    std::normal_distribution<double> d(mean, sigma);
    return d(gen);
}

double jitter(double base, double amount) {
    double j = gaussianRandom(0.0, amount);
    return base + j;
}

std::string maskId(int64_t id) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%llX%04llX",
             static_cast<unsigned long long>(id >> 16) & 0xFFFFFFFFFFFFULL,
             static_cast<unsigned long long>(id & 0xFFFF));
    return std::string(buf);
}

}  // namespace rb_utils

// ---------------------------------------------------------------------------
// Extended utility functions
// ---------------------------------------------------------------------------

namespace {

// Linear interpolation helper for arrays.
double interp(const std::vector<double>& ys, double x) {
    if (ys.empty()) return 0.0;
    if (x <= 0.0) return ys.front();
    if (x >= static_cast<double>(ys.size() - 1)) return ys.back();
    size_t i = static_cast<size_t>(x);
    double frac = x - static_cast<double>(i);
    return ys[i] * (1.0 - frac) + ys[i + 1] * frac;
}

}  // namespace

// Normalized value within a range (0..1).
double rb_utils::normalize(double v, double lo, double hi) {
    if (hi <= lo) return 0.0;
    return clamp01((v - lo) / (hi - lo));
}

// Weighted moving average over a series.
double rb_utils::wma(const std::vector<double>& series,
                     const std::vector<double>& weights) {
    if (series.empty()) return 0.0;
    size_t n = std::min(series.size(), weights.size());
    if (n == 0) {
        double sum = 0.0;
        for (double v : series) sum += v;
        return sum / static_cast<double>(series.size());
    }
    double acc = 0.0;
    double wsum = 0.0;
    for (size_t i = 0; i < n; ++i) {
        acc += series[i] * weights[i];
        wsum += weights[i];
    }
    return wsum > 0.0 ? acc / wsum : 0.0;
}

// Rolling sum over the last n samples.
double rb_utils::rollingSum(const std::vector<double>& series, int n) {
    size_t start = series.size() >= static_cast<size_t>(n)
                       ? series.size() - static_cast<size_t>(n)
                       : 0;
    double sum = 0.0;
    for (size_t i = start; i < series.size(); ++i) sum += series[i];
    return sum;
}

// Decay-weighted average (older samples matter less).
double rb_utils::decayAverage(const std::vector<double>& series,
                              double decay) {
    if (series.empty()) return 0.0;
    double acc = 0.0;
    double wsum = 0.0;
    double w = 1.0;
    for (auto it = series.rbegin(); it != series.rend(); ++it) {
        acc += (*it) * w;
        wsum += w;
        w *= decay;
    }
    return wsum > 0.0 ? acc / wsum : 0.0;
}

// Triangle wave between lo and hi (for cyclical patterns).
double rb_utils::triangleWave(double t, double lo, double hi, double period) {
    double phase = std::fmod(t, period) / period;   // 0..1
    double v = phase < 0.5 ? phase * 2.0 : 2.0 - phase * 2.0;
    return lo + (hi - lo) * v;
}

// Interpolate a series at a fractional index.
double rb_utils::sampleAt(const std::vector<double>& ys, double x) {
    return interp(ys, x);
}

// Entropy of a probability vector (0 = deterministic).
double rb_utils::entropy(const std::vector<double>& probs) {
    double h = 0.0;
    for (double p : probs) {
        if (p <= 0.0) continue;
        h -= p * std::log2(p);
    }
    return h;
}

// Softmax normalization of a score vector.
std::vector<double> rb_utils::softmax(const std::vector<double>& scores) {
    std::vector<double> out(scores.size(), 0.0);
    if (scores.empty()) return out;
    double maxv = *std::max_element(scores.begin(), scores.end());
    double sum = 0.0;
    for (size_t i = 0; i < scores.size(); ++i) {
        out[i] = std::exp(scores[i] - maxv);
        sum += out[i];
    }
    if (sum > 0.0) {
        for (double& v : out) v /= sum;
    }
    return out;
}

// Z-score of a value within a distribution.
double rb_utils::zscore(double v, const std::vector<double>& population) {
    double m = mean(population);
    double sd = standardDeviation(population);
    if (sd <= 0.0) return 0.0;
    return (v - m) / sd;
}

// Min-max scale of a series into [0,1].
std::vector<double> rb_utils::minMaxScale(
    const std::vector<double>& values) {
    std::vector<double> out(values.size(), 0.0);
    if (values.empty()) return out;
    double lo = *std::min_element(values.begin(), values.end());
    double hi = *std::max_element(values.begin(), values.end());
    for (size_t i = 0; i < values.size(); ++i) {
        out[i] = normalize(values[i], lo, hi);
    }
    return out;
}

// Bucket a value into a histogram index.
int rb_utils::bucketIndex(double v, double lo, double hi, int buckets) {
    if (buckets <= 1) return 0;
    int idx = static_cast<int>((v - lo) / (hi - lo) * static_cast<double>(buckets));
    return clampInt(idx, 0, buckets - 1);
}

// Round to a given precision (e.g., 2 decimals).
double rb_utils::roundTo(double v, int decimals) {
    double factor = std::pow(10.0, static_cast<double>(decimals));
    return std::round(v * factor) / factor;
}

// ---------------------------------------------------------------------------
// EloEngine
// ---------------------------------------------------------------------------

double EloEngine::expectedScore(double ratingA, double ratingB) const {
    return 1.0 / (1.0 + std::pow(10.0, (ratingB - ratingA) / 400.0));
}

double EloEngine::kFactorFor(double rating) const {
    if (rating < 1600.0) return 32.0;
    if (rating < 2000.0) return 24.0;
    if (rating < 2400.0) return 16.0;
    return 12.0;
}

EloResult EloEngine::apply(double rating, double opponentRating, bool won,
                           int streak, bool protectionAvailable) const {
    EloResult out;
    out.expected = expectedScore(rating, opponentRating);
    out.kFactor = kFactorFor(rating);

    double margin = won ? 1.0 : 0.0;
    double delta = out.kFactor * (margin - out.expected);

    // Streak modifier: sustained wins earn slightly more.
    if (won && streak > 0) {
        delta += std::min(2.0, static_cast<double>(streak) * 0.5);
    } else if (!won && streak < 0) {
        delta -= std::min(2.0, static_cast<double>(-streak) * 0.5);
    }

    out.newRating = rating + delta;
    out.delta = delta;

    // Star movement.
    int stars = starsPerWin(RankTier::kMythic);
    if (won) {
        out.starDelta = stars;
        out.promoted = true;
    } else {
        if (protectionAvailable) {
            out.starDelta = 0;
            out.starProtectionUsed = true;
        } else {
            out.starDelta = -starsPerLoss(RankTier::kMythic);
            out.demoted = true;
        }
    }
    return out;
}

int EloEngine::starsPerWin(RankTier tier) const {
    switch (tier) {
        case RankTier::kWarrior: return 2;
        case RankTier::kElite: return 2;
        case RankTier::kMaster: return 1;
        case RankTier::kGrandmaster: return 1;
        case RankTier::kEpic: return 1;
        case RankTier::kLegend: return 1;
        case RankTier::kMythic: return 1;
        case RankTier::kMythicalGlory: return 1;
        case RankTier::kMythicalImmortal: return 1;
        default: return 1;
    }
}

int EloEngine::starsPerLoss(RankTier tier) const {
    switch (tier) {
        case RankTier::kWarrior: return 1;
        case RankTier::kElite: return 1;
        case RankTier::kMaster: return 1;
        case RankTier::kGrandmaster: return 1;
        case RankTier::kEpic: return 1;
        case RankTier::kLegend: return 1;
        case RankTier::kMythic: return 1;
        case RankTier::kMythicalGlory: return 1;
        case RankTier::kMythicalImmortal: return 1;
        default: return 1;
    }
}

RankPoint EloEngine::advanceRank(const RankPoint& before, int starDelta) const {
    RankPoint after = before;
    int abs = before.absolute() + starDelta;
    if (abs < 0) abs = 0;
    after = RankPoint::fromAbsolute(abs);
    return after;
}

// ---------------------------------------------------------------------------
// MMRModel
// ---------------------------------------------------------------------------

double MMRModel::g(double sigma) const {
    return 1.0 / std::sqrt(1.0 + 3.0 * sigma * sigma /
                                   (std::pow(std::acos(-1.0), 2.0) * 225.0));
}

double MMRModel::expected(double rating, double opponentRating,
                          double oppSigma) const {
    double gOpp = g(oppSigma);
    double exponent = -gOpp * (rating - opponentRating) / 400.0;
    return 1.0 / (1.0 + std::exp(exponent));
}

MMRUpdate MMRModel::update(const PlayerProfile& p, double opponentRating,
                           double opponentSigma, bool won, double tau) const {
    const double q = std::log(10.0) / 400.0;
    MMRUpdate out;
    out.newRating = p.mmr;
    out.newSigma = p.sigma;
    out.newVolatility = p.volatility;

    double gOpp = g(opponentSigma);
    double E = expected(p.mmr, opponentRating, opponentSigma);
    double score = won ? 1.0 : 0.0;

    double v = 1.0 / (q * q * gOpp * gOpp * E * (1.0 - E));
    double m = gOpp * (score - E);
    double newRating = p.mmr + q * v * m;

    // Volatility via Newton iteration (simplified Glicko-2 step).
    double a = std::log(p.volatility * p.volatility);
    double delta = q * std::sqrt(v) * m;
    double phi = p.sigma;
    double phiSq = phi * phi;
    double deltaSq = delta * delta;

    auto f = [&](double x) {
        double ex = std::exp(x);
        double inner = ex * deltaSq - phiSq - v - ex;
        return 0.5 * (ex * (deltaSq - phiSq - v - ex) /
                      ((phiSq + v + ex) * (phiSq + v + ex))) -
               (x - a) / (tau * tau);
    };

    double A = a;
    double B = deltaSq > phiSq + v
                   ? std::log(deltaSq - phiSq - v)
                   : a - std::max(0.0, tau * std::sqrt(std::max(0.0, deltaSq - phiSq - v)) -
                                           tau);

    const double eps = 1e-6;
    int iter = 0;
    double fa = f(A);
    double fb = f(B);
    while (std::fabs(B - A) > eps && iter < 100) {
        double C = A + (A - B) * fa / (fb - fa);
        double fc = f(C);
        if (fc * fb < 0.0) {
            A = B;
            fa = fb;
        } else {
            fa /= 2.0;
        }
        B = C;
        fb = fc;
        ++iter;
    }
    double sigmaNew = std::exp(A / 2.0);
    if (sigmaNew > 0.15) sigmaNew = 0.15;

    out.newVolatility = sigmaNew;
    double phiStar = std::sqrt(phi * phi + sigmaNew * sigmaNew);
    out.newSigma = 1.0 / std::sqrt(1.0 / (phiStar * phiStar) + 1.0 / v);
    out.newRating = p.mmr + q * out.newSigma * out.newSigma * gOpp * (score - E);
    out.delta = out.newRating - p.mmr;
    return out;
}

double MMRModel::provisionalFromRank(const RankPoint& rp) const {
    return 900.0 + static_cast<double>(rp.absolute()) * 12.0;
}

RankPoint MMRModel::rankFromMMR(double mmr) const {
    int abs = static_cast<int>((mmr - 900.0) / 12.0);
    return RankPoint::fromAbsolute(abs);
}

double MMRModel::confidenceOf(double sigma) const {
    return rb_utils::clamp01(1.0 - sigma / 350.0);
}

}  // namespace rb
}  // namespace arift