#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "rb_api.h"

namespace arift {
namespace rb {

// ---------------------------------------------------------------------------
// Free math/analysis functions used across the booster (declared here so
// rb_elo.cpp / rb_mmr.cpp / others can share them).
// ---------------------------------------------------------------------------

double winProbability(const RankPoint& me, const RankPoint& opponent,
                      double sigmaCombined);
double winProbability(double myMmr, double oppMmr, double sigmaCombined);
double ratingNeeded(double targetWinRate, double oppRating,
                    double sigmaCombined);
double expectedStarsPerMatch(double winRate, int starsWin, int starsLoss);
double matchesNeeded(int stars, double winRate, int starsWin, int starsLoss);
std::vector<double> ratingTrajectory(const PlayerProfile& p,
                                     const std::vector<MatchRecord>& history);

struct CalibrationResult {
    double skillEstimate = 1500.0;
    double confidence95Low = 1500.0;
    double confidence95High = 1500.0;
    double sampleSize = 0.0;
    bool sufficient = false;
};

CalibrationResult calibrateSkill(const std::vector<MatchRecord>& history);
double performanceRating(const std::vector<double>& opponentRatings,
                         double score, double rounds);
int protectionForLoss(int tierIdx, int streak);

struct ChoiceAnalysis {
    double winRate = 0.5;
    double starsPer100 = 0.0;
    double risk = 0.5;
    double value = 0.0;
};

ChoiceAnalysis analyzeChoice(double myMmr, double oppMmr, double sigma,
                             int starsWin, int starsLoss);

struct MmrTrend {
    double slope = 0.0;
    double intercept = 0.0;
    double projected50 = 0.0;
    double projected100 = 0.0;
    double variance = 0.0;
    double stability = 0.0;
    bool improving = false;
    size_t window = 0;
};

MmrTrend analyzeTrend(const PlayerProfile& p,
                      const std::vector<MatchRecord>& history,
                      size_t window = 30);
double predictMmrAfter(const MmrTrend& trend, size_t n);

struct LadderProjection {
    int matchesToTarget = 0;
    int daysToTarget = 0;
    double matchesPerDay = 0.0;
    double probabilityOfReaching = 0.0;
    std::string targetRankStr;
};

LadderProjection projectLadder(const PlayerProfile& p,
                               const std::vector<MatchRecord>& history,
                               const RankPoint& target,
                               double matchesPerDay);

double effectiveMmr(const PlayerProfile& p, double ratingThreshold = 0.3);

struct PoolAnalysis {
    double expectedWinRate = 0.5;
    double avgOpponentMmr = 1500.0;
    double minOpponentMmr = 0.0;
    double maxOpponentMmr = 0.0;
    double sigmaOfPool = 0.0;
    double bestValueMatchIdx = 0;
};

PoolAnalysis analyzePool(const std::vector<double>& opponentMmrs,
                         double myMmr, double sigmaCombined);

struct StarEconomy {
    int tier = 0;
    int winStars = 1;
    int lossStars = 1;
    int promotionBonus = 0;
    int demotionPenalty = 0;
};

StarEconomy economyForTier(RankTier tier);

struct SessionPlan {
    int matches = 6;
    double expectedStars = 1.2;
    double fatigueFactor = 0.15;
    double expectedStarsWithFatigue = 1.0;
    int recommendedRestMs = 900000;
};

SessionPlan optimizeSession(double winRate, int starsWin, int starsLoss);

// ---------------------------------------------------------------------------
// Statistics analysis functions
// ---------------------------------------------------------------------------

struct Histogram {
    double min = 0.0;
    double max = 100.0;
    int buckets = 10;
    std::vector<int> counts;
    std::vector<double> centers;
    void build(const std::vector<double>& values, double lo, double hi, int n);
    int total() const;
    double peak() const;
};

Histogram kdaHistogram(const std::vector<MatchRecord>& history);
Histogram performanceHistogram(const std::vector<MatchRecord>& history);
Histogram durationHistogram(const std::vector<MatchRecord>& history);

struct MmrBandWinRates {
    std::vector<double> bandCenters;
    std::vector<double> winRates;
    std::vector<int> sampleCounts;
};

MmrBandWinRates winRateByMmrBand(const std::vector<MatchRecord>& history,
                                 double myMmr);
double consistencyScore(const std::vector<MatchRecord>& history);

struct PerformanceWindow {
    int startHour = 0;
    int endHour = 0;
    double winRate = 0.5;
    double avgPerformance = 50.0;
    int samples = 0;
};

PerformanceWindow bestPerformanceWindow(const std::vector<MatchRecord>& history);

struct StarProjection {
    double evPerMatch = 0.0;
    double matchesToTarget = 0.0;
    double daysToTarget = 0.0;
    double confidence = 0.5;
    std::string text;
};

StarProjection projectStars(const PlayerProfile& p, const RbStatistics& stats,
                            const RankPoint& target,
                            const StarEconomy& economy);

struct BurstAnalysis {
    double burstiness = 0.0;
    int maxConsecutiveWins = 0;
    int maxConsecutiveLosses = 0;
    int burstCount = 0;
};

BurstAnalysis analyzeBursts(const std::vector<MatchRecord>& history);
double recommendedTargetDelta(const RbStatistics& stats, double currentMmr,
                              double desiredWinRate);

struct WilsonInterval {
    double lower = 0.0;
    double upper = 0.0;
    double estimate = 0.5;
};

WilsonInterval wilsonInterval(int wins, int total, double z = 1.96);
std::string statisticsReport(const PlayerProfile& p,
                             const std::vector<MatchRecord>& history);

// ---------------------------------------------------------------------------
// Additional analytics (implemented across rb_* files)
// ---------------------------------------------------------------------------

double performanceMomentum(const std::vector<MatchRecord>& history, int n);
double kdaVolatility(const std::vector<MatchRecord>& history);
double earlyLeadRate(const std::vector<MatchRecord>& history);
double lateCollapseRate(const std::vector<MatchRecord>& history);
double rollercoasterIndex(const std::vector<MatchRecord>& history);
double quantile(const std::vector<double>& values, double q);
double iqr(const std::vector<double>& values);
double skewness(const std::vector<double>& values);
double kurtosis(const std::vector<double>& values);
int outlierCount(const std::vector<double>& values, double k);
double autocorrelationLag1(const std::vector<double>& values);
const char* performanceRegime(const std::vector<double>& values);

double thresholdRating(RankTier tier);
double matchValue(double mmr, double sigma, double oppMmr);
std::pair<double, double> ratingBand(double rating, double sigma);
double glickoG(double sigma);
double groupExpected(const PlayerProfile& p,
                     const std::vector<double>& oppRatings,
                     const std::vector<double>& oppSigmas);
double uncertaintyAdjusted(double rating, double sigma);
double matchesUntilStable(double sigma, double floor);
double mmrPercentile(double mmr);
double eloGlickoRatio(double sigma);
double starsPerWinAdjusted(const StarEconomy& economy, double confidence);
double volatilityTrend(const std::vector<MatchRecord>& history, int n);

double localNormCdf(double x);
int protectionAccrual(RankTier tier);
int protectionBurnThreshold(RankTier tier, int protection);
double momentumEloDelta(int streak, double baseDelta);
double deviationPenalty(double rating, double oppRating, double band);
double ratingDiffForWinRate(double p);
double recalibrationDelta(double sigma);
double expectedStars(double winRate, const StarEconomy& econ);
double performanceFromKda(int kills, int deaths, int assists,
                          double durationMin);
int rankSpread(const std::vector<RankPoint>& ranks);
double projectedRating(double start, double avgDelta, double n);
double protectionSavings(int protectionsUsed, double starsPerWin);

double difficultyTarget(const RbStatistics& stats, const RbConfig& cfg,
                        double sessionWr);
double acceptProbability(double difficulty, double myMmr, double oppMmr);
double queueDelayModel(double difficulty, double baseSeconds);

// Lobby dwell analysis (defined in rb_lobby.cpp).
struct LobbyDwellAnalysis {
    double avgDwellMs = 20000.0;
    double p90DwellMs = 40000.0;
    int samples = 0;
};

LobbyDwellAnalysis analyzeDwell(const std::vector<MatchRecord>& history);
double recommendQueueStartDelayMs(const RbConfig& cfg,
                                  const LobbyDwellAnalysis& dwell);
double naturalQueueProbability(double elapsedMs,
                               const LobbyDwellAnalysis& dwell);
double naturalMatchGapMs(const RbConfig& cfg);
double naturalRoomIdleMs(const RbConfig& cfg, int roomSize);
double queuePopulatedness(int64_t nowMs);
double inviteReactionDelayMs(const RbConfig& cfg);
double requeueProbability(double waitSeconds, double patienceSeconds);

double momentumIndex(const PlayerProfile& p, const RbStatistics& stats);
double clutchFactor(const std::vector<MatchRecord>& history);
double fatigueIndex(const std::vector<MatchRecord>& history);
double objectiveFocus(const RbStatistics& stats);
double lanePressureStyle(const RbStatistics& stats);
double heroPoolBreadth(const std::vector<int>& roleCounts);
double styleDrift(const std::vector<double>& styleHistory);
int preferredLane(const std::vector<int>& roleCounts);
double adaptabilityIndex(const std::vector<MatchRecord>& history);
double metaAwareness(const std::vector<MatchRecord>& history);
double composureIndex(const std::vector<MatchRecord>& history);
double earlyAggressionShare(const RbStatistics& stats);
double objectiveEfficiency(const RbStatistics& stats);

double stepDifficulty(double current, double target, double step);
double patienceForDifficulty(double difficulty);
double timingNoiseBudget(AggressionLevel aggr);
bool needsRetune(const RbConfig& current, const RbConfig& observed,
                 double sensitivity);
double sensitivityFor(AggressionLevel aggr);
double sessionHoursForBudget(int matches, double avgMatchMin);
double riskAdjustedTarget(double base, int guardRisk);
double tunerScore(const TunerResult& r, const RbConfig& baseline);
double sweepBest(const std::function<double(double)>& scorer, double lo,
                 double hi, int steps);
std::vector<double> queueWaitGrid(double difficulty);
std::vector<double> winRateGrid(AggressionLevel aggr);
double effortScore(double value, double lo, double hi);
std::string configFingerprint(const RbConfig& cfg);
double configDelta(const RbConfig& a, const RbConfig& b);
bool configStable(const std::vector<std::string>& fingerprints, int window);
std::vector<RbConfig> rankConfigs(const std::vector<RbConfig>& candidates,
                                  const RbConfig& baseline);

const MatchRecord* bestMatch(const std::vector<MatchRecord>& history);
const MatchRecord* worstMatch(const std::vector<MatchRecord>& history);
int currentWinRun(const std::vector<MatchRecord>& history);
std::string profileSummary(const ProfileStore& store);

double expectedValuePerMatch(const RbStatistics& stats,
                             const StarEconomy& economy);
double sessionEfficiency(const RbStatistics& stats, double starsEarned);

const char* waitBucket(double waitSec);
double avgSessionLengthMin();
double sessionTrend();
std::string bestSessionSummary();
int sessionsLast24h();
std::vector<int> matchesPerSessionHistogram();
std::string telemetryBlob();

std::vector<int64_t> throttleSchedule(int64_t startMs, int sends,
                                      double baseGapMs);
uint64_t sessionSignature(const std::vector<LobbyPacket>& burst);
int replayAnomalies(const std::vector<int64_t>& sendTimes,
                    const std::vector<double>& expectedGapsMs);
bool sessionMixOrganic(const std::vector<uint8_t>& opcodes);
std::vector<uint8_t> xorRotate(const std::vector<uint8_t>& data,
                               uint8_t offset);
std::vector<uint8_t> shuffleBytes(const std::vector<uint8_t>& data);
std::vector<uint8_t> unshuffleBytes(const std::vector<uint8_t>& data);
std::vector<uint8_t> prefixLength(const std::vector<uint8_t>& segment);
uint32_t keyForTime(int64_t ms);
size_t worstCaseFrameSize(size_t payloadBytes);

double rankProgress(const PlayerProfile& p, const RankPoint& target);
double sessionsRemaining(const RbStatistics& stats, const RankPoint& target,
                         const RankPoint& current);
std::vector<double> starPaceCurve(const RbStatistics& stats, int starsNeeded,
                                  int sessions);

double sessionStarExpectation(double winRate, int starsWin, int starsLoss,
                              int matches);
int64_t restForDuration(double avgMatchMinutes);
int dailyBudgetFromCadence(const std::vector<MatchRecord>& history);
int bestQueueHour(const std::vector<MatchRecord>& history);
double timeOfDayScore(int minutes);
std::vector<double> weekdayProfile(const std::vector<MatchRecord>& history);
double weekendQueueMultiplier(int64_t nowMs);
int dailyRecommendation(int64_t nowMs,
                        const std::vector<MatchRecord>& history);
std::vector<double> breakSchedule(int matches, double baseRestMin);
double requiredPace(int starsRemaining, int daysRemaining);
bool paceFeasible(double requiredPerDay, double currentPerDay);
bool tryConsumeDailySlot(int64_t nowMs);
int dailySlotsRemaining(int64_t nowMs);

double riskProbability(int rawScore);
double compositeSignal(const PlayerProfile& p,
                       const std::vector<MatchRecord>& history);
int decayedRisk(int rawScore, int64_t elapsedMs, double halfLifeMs);
double gapDiscount(int64_t gapMs);
int streakRisk(int streak);
int performanceBandRisk(double avgPerformance);
int kdaAnomalyRisk(const std::vector<MatchRecord>& history);
double safetyMargin(int risk, int threshold);

AggressionLevel aggressionForPerformance(double avgPerformance);
std::vector<std::string> diffConfigs(const RbConfig& a, const RbConfig& b);

// Cross-file helpers used by rb_core.cpp / rb_protocol.cpp.
std::string cacheKey(const std::string& ns, const std::string& id);
bool extremeStreakPattern(const std::vector<MatchRecord>& history,
                          int threshold);
const char* trafficCategory(uint8_t opcode);

}  // namespace rb
}  // namespace arift