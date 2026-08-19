#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace arift {
namespace rb {

// ---------------------------------------------------------------------------
// Core data types shared across the rank booster engine.
// ---------------------------------------------------------------------------

// Rank tiers (mirrors MLBB ladder).
enum class RankTier : int {
    kWarrior = 0,
    kElite = 1,
    kMaster = 2,
    kGrandmaster = 3,
    kEpic = 4,
    kLegend = 5,
    kMythic = 6,
    kMythicalGlory = 7,
    kMythicalImmortal = 8,
    kCount = 9,
};

struct RankPoint {
    RankTier tier = RankTier::kWarrior;
    int stars = 0;              // stars within tier
    int protection = 0;         // star protection points
    int score = 0;              // hidden MMR score

    int absolute() const {
        return static_cast<int>(tier) * 100 + stars;
    }
    std::string toString() const;
    static RankPoint fromAbsolute(int abs);
};

struct MatchResult {
    bool won = false;
    bool ranked = true;
    int kdaKills = 0;
    int kdaDeaths = 0;
    int kdaAssists = 0;
    int mvpScore = 0;
    int teamScore = 0;
    int enemyScore = 0;
    double durationMin = 0.0;
    int performancePercentile = 50;
    int64_t endedAtMs = 0;
};

struct MatchRecord {
    int64_t matchId = 0;
    int64_t startedAtMs = 0;
    int64_t endedAtMs = 0;
    MatchResult result;
    RankPoint rankBefore;
    RankPoint rankAfter;
    double mmrDelta = 0.0;
    double skillRating = 0.0;
    double confidence = 0.0;
    std::string mode;           // "ranked" | "classic" | "brawl" ...
    std::vector<std::string> teamComposition;
};

struct PlayerProfile {
    std::string name;
    std::string region;
    RankPoint rank;
    double mmr = 1500.0;
    double sigma = 350.0;       // rating uncertainty (Glicko-style)
    double volatility = 0.06;
    double winRate = 0.5;
    int64_t matchesPlayed = 0;
    int64_t matchesWon = 0;
    int64_t matchesLost = 0;
    int64_t streak = 0;         // +win streak, -lose streak
    int64_t lastMatchMs = 0;
    bool protectedLoss = false;
    double performanceIndex = 50.0;
};

struct MatchmakingRequest {
    std::string mode;
    int preferredRole = 0;      // 0 any, 1 tank, 2 fighter, 3 assassin...
    int maxQueueSeconds = 60;
    int partySize = 1;
    double desiredDifficulty = 0.5;   // 0 easy .. 1 hard
    int regionBias = 0;
    bool useWideSearch = false;
};

struct QueueSnapshot {
    int64_t queueStartMs = 0;
    int64_t queueEndMs = 0;
    double estimatedWaitSec = 0.0;
    int currentPosition = 0;
    int totalInQueue = 0;
    std::string mode;
    bool searching = false;
    bool found = false;
    int64_t matchId = 0;
};

enum class BoosterState : int {
    kIdle = 0,
    kCollecting = 1,
    kAnalyzing = 2,
    kTuning = 3,
    kReady = 4,
    kActive = 5,
    kPaused = 6,
    kError = 7,
};

enum class AggressionLevel : int {
    kConservative = 1,
    kBalanced = 2,
    kAggressive = 3,
    kExtreme = 4,
};

struct BoosterTelemetry {
    int64_t sessions = 0;
    int64_t matchesTracked = 0;
    int64_t matchesWon = 0;
    int64_t queueJoins = 0;
    double avgMmrDelta = 0.0;
    double bestMmrDelta = 0.0;
    double worstMmrDelta = 0.0;
    double avgWinRate = 0.0;
    double avgPerformance = 50.0;
    int64_t tunerRuns = 0;
    int64_t guardEvents = 0;
    int64_t lastSessionMs = 0;
};

// Serialization helpers (compact binary/JSON-ish).
std::string serializeRank(const RankPoint& rp);
RankPoint deserializeRank(const std::string& s);
std::string serializeMatch(const MatchRecord& rec);
MatchRecord deserializeMatch(const std::string& s);

// Binary serialization (for caches/telemetry).
std::vector<uint8_t> encodeMatchBinary(const MatchRecord& rec);
MatchRecord decodeMatchBinary(const std::vector<uint8_t>& in);
std::vector<uint8_t> encodeProfileBinary(const PlayerProfile& p);
PlayerProfile decodeProfileBinary(const std::vector<uint8_t>& in);

// Type helpers.
const char* tierCode(RankTier tier);
const char* aggressionName(AggressionLevel level);
const char* boosterStateName(BoosterState state);
bool parseTierCode(const std::string& code, RankTier& out);

// Extended types shared across modules.

// Aggregate of a ranked match for reporting.
struct MatchSummary {
    int64_t matchId = 0;
    std::string rankBefore;
    std::string rankAfter;
    double mmrDelta = 0.0;
    bool won = false;
    std::string kda;
    int performance = 50;
    double durationMin = 0.0;
    std::string mode;

    std::string line() const;
};

// Per-tier star economy snapshot for the ladder UI.
struct TierInfo {
    RankTier tier = RankTier::kWarrior;
    std::string name;
    int winStars = 1;
    int lossStars = 1;
    double mmrThreshold = 1000.0;
    std::string icon = "S";

    std::string toString() const;
};

// Session bookmark: where a session left off.
struct SessionBookmark {
    int64_t savedAtMs = 0;
    RankPoint rank;
    double mmr = 0.0;
    int matchesPlayed = 0;
    std::string note;

    std::string serialize() const;
    static SessionBookmark deserialize(const std::string& s);
};

// Difficulty calibration record.
struct CalibrationPoint {
    double mmr = 0.0;
    double winRate = 0.5;
    int samples = 0;
};

}  // namespace rb
}  // namespace arift