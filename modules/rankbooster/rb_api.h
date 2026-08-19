#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "arift_thread.h"
#include "rb_types.h"

namespace arift {
namespace rb {

struct StarEconomy;

// ---------------------------------------------------------------------------
// Rank booster configuration
// ---------------------------------------------------------------------------

struct RbConfig {
    bool enabled = false;
    AggressionLevel aggression = AggressionLevel::kBalanced;
    RankTier targetTier = RankTier::kMythic;
    int targetStars = 0;
    double desiredWinRate = 0.65;
    double maxQueueSeconds = 90.0;
    double tuneIntervalSec = 300.0;
    double telemetryRetentionDays = 30.0;
    int guardSensitivity = 3;        // 1..5
    bool autoPauseOnRisk = true;
    bool protectStreak = true;
    bool preferredRoleEnabled = false;
    int preferredRole = 0;
    bool partyAssistEnabled = false;
    bool adaptiveDifficulty = true;
    double mmrTarget = 2600.0;
    double sigmaFloor = 60.0;
    double volatilityCap = 0.12;
    int performanceBias = 15;        // extra performance percentile bias
    bool lobbyTimingEnabled = true;
    double lobbyDelayMinMs = 800.0;
    double lobbyDelayMaxMs = 2500.0;
    bool randomizePlaystyle = true;
    bool burstPatternEnabled = false;
    double burstLimitMatches = 2.0;
    bool preferEvenings = true;

    void loadFromConfig();
    void saveToConfig() const;

    // Validation / helpers.
    void sanitize();
    std::string toString() const;
    std::string toCacheString() const;
    bool fromCacheString(const std::string& s);
};

class RbConfigStore {
public:
    static RbConfigStore& instance();
    RbConfig& config() { return config_; }
    const RbConfig& config() const { return config_; }
    void load();
    void save();
    void resetToDefaults();

private:
    RbConfigStore() = default;
    RbConfig config_;
};

// ---------------------------------------------------------------------------
// Elo math
// ---------------------------------------------------------------------------

struct EloResult {
    double expected = 0.5;
    double newRating = 1500.0;
    double delta = 0.0;
    double kFactor = 32.0;
    int starDelta = 0;
    bool promoted = false;
    bool demoted = false;
    bool starProtectionUsed = false;
};

class EloEngine {
public:
    double expectedScore(double ratingA, double ratingB) const;
    double kFactorFor(double rating) const;
    EloResult apply(double rating, double opponentRating, bool won,
                    int streak, bool protectionAvailable) const;

    // Star conversions for a given tier.
    int starsPerWin(RankTier tier) const;
    int starsPerLoss(RankTier tier) const;
    RankPoint advanceRank(const RankPoint& before, int starDelta) const;
};

// ---------------------------------------------------------------------------
// MMR model (Glicko-2 style with adaptations)
// ---------------------------------------------------------------------------

struct MMRUpdate {
    double newRating = 0.0;
    double newSigma = 0.0;
    double newVolatility = 0.0;
    double delta = 0.0;
};

class MMRModel {
public:
    double g(double sigma) const;
    double expected(double rating, double opponentRating, double oppSigma) const;
    MMRUpdate update(const PlayerProfile& p, double opponentRating,
                     double opponentSigma, bool won, double tau) const;
    double provisionalFromRank(const RankPoint& rp) const;
    RankPoint rankFromMMR(double mmr) const;
    double confidenceOf(double sigma) const;
};

// ---------------------------------------------------------------------------
// Matchmaking engine
// ---------------------------------------------------------------------------

struct MatchCandidate {
    int64_t playerId = 0;
    std::string name;
    double mmr = 1500.0;
    double sigma = 350.0;
    int role = 0;
    int region = 0;
    double latency = 40.0;
    double winRate = 0.5;
    int64_t matches = 0;
    double difficulty = 0.5;
    double fairnessScore = 0.5;
    double synergyScore = 0.5;
};

class MatchmakingEngine {
public:
    QueueSnapshot beginQueue(const MatchmakingRequest& req);
    QueueSnapshot pollQueue();
    void cancelQueue();
    bool isSearching() const { return searching_; }

    // Pool simulation + selection
    std::vector<MatchCandidate> generatePool(size_t count, double myMmr,
                                             double difficulty) const;
    MatchCandidate selectMatch(const std::vector<MatchCandidate>& pool,
                               const MatchmakingRequest& req,
                               double myMmr, double mySigma) const;
    double difficultyOf(const MatchCandidate& c, double myMmr) const;
    double fairnessOf(const MatchCandidate& c, double myMmr, double mySigma) const;
    double synergyOf(const MatchCandidate& c, const PlayerProfile& me) const;

    // Timing estimation
    double estimateWait(const MatchmakingRequest& req, int poolSize) const;

    int64_t lastQueueStartMs() const { return queue_.queueStartMs; }
    const QueueSnapshot& snapshot() const { return queue_; }

private:
    bool searching_ = false;
    QueueSnapshot queue_;
    int64_t queue_deadline_ms_ = 0;
};

// ---------------------------------------------------------------------------
// Lobby-side hooks
// ---------------------------------------------------------------------------

struct LobbyState {
    bool inLobby = false;
    bool inRoom = false;
    bool rankedReady = false;
    std::string mode;
    int64_t joinedRoomMs = 0;
    std::vector<std::string> roomMembers;
    int roomSize = 0;
    double elapsedMs = 0.0;
};

class LobbyHooks {
public:
    LobbyState& state() { return state_; }
    const LobbyState& state() const { return state_; }

    bool onEnterLobby(int64_t nowMs);
    bool onJoinRoom(int64_t nowMs, const std::vector<std::string>& members);
    bool onStartSearch(int64_t nowMs);
    bool onMatchFound(int64_t nowMs, int64_t matchId);
    bool onMatchStart(int64_t nowMs);

    // Lobby timing manipulation: returns the recommended delay before
    // accepting a match (randomized human-like).
    double recommendedAcceptDelayMs(const RbConfig& cfg) const;

    // Room state diagnostics
    std::string diag() const;

private:
    LobbyState state_;
    int64_t search_start_ms_ = 0;
    int64_t last_accept_ms_ = 0;
};

// ---------------------------------------------------------------------------
// Profile store
// ---------------------------------------------------------------------------

class ProfileStore {
public:
    static ProfileStore& instance();

    PlayerProfile& local() { return local_; }
    const PlayerProfile& local() const { return local_; }

    bool load(const std::string& baseDir);
    bool save(const std::string& baseDir) const;

    void recordMatch(const MatchRecord& rec);
    double rollingWinRate(int64_t windowMs) const;
    void reset();

    const std::vector<MatchRecord>& history() const { return history_; }
    size_t historySize() const { return history_.size(); }
    int64_t lastMatchMs() const { return local_.lastMatchMs; }

private:
    ProfileStore() = default;
    PlayerProfile local_;
    std::vector<MatchRecord> history_;
};

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

struct RbStatistics {
    double winRate = 0.0;
    double recentWinRate = 0.0;
    double avgMmrDelta = 0.0;
    double mmrProgress = 0.0;
    double sigma = 0.0;
    double volatility = 0.0;
    double expectedStarsPerMatch = 0.0;
    double projectedStarsPerDay = 0.0;
    int64_t totalMatches = 0;
    int64_t wins = 0;
    int64_t losses = 0;
    int64_t currentStreak = 0;
    int64_t bestStreak = 0;
    int64_t worstStreak = 0;
    double avgPerformance = 50.0;
    double avgKills = 0.0;
    double avgDeaths = 0.0;
    double avgAssists = 0.0;
    double kdaRatio = 0.0;
    double mvpRate = 0.0;
    double survivalRate = 0.0;
    double damagePerMinute = 0.0;
    double goldPerMinute = 0.0;
    double firstBloodRate = 0.0;
    double comebackRate = 0.0;
    double earlyGameWinRate = 0.0;
    double lateGameWinRate = 0.0;
    double rankedMatchShare = 0.0;
    int64_t longestMatchMin = 0;
    int64_t shortestMatchMin = 0;
    double avgMatchDuration = 0.0;
    int64_t matchesThisWeek = 0;
    int64_t matchesThisMonth = 0;

    void compute(const PlayerProfile& p, const std::vector<MatchRecord>& history);
    std::string toString() const;
};

// ---------------------------------------------------------------------------
// Adaptive tuner
// ---------------------------------------------------------------------------

struct TunerResult {
    RbConfig suggested;
    std::string rationale;
    int64_t runMs = 0;
    bool changed = false;
};

class AdaptiveTuner {
public:
    TunerResult tune(const PlayerProfile& p, const RbStatistics& stats,
                     const RbConfig& current, AggressionLevel aggr) const;

    double recommendedQueueDifficulty(const PlayerProfile& p,
                                      const RbStatistics& stats,
                                      const RbConfig& cfg) const;
    double recommendedPlaystyle(const RbConfig& cfg) const;
    bool shouldPause(const RbStatistics& stats, const RbConfig& cfg) const;
    std::string explain(const TunerResult& r) const;
};

// ---------------------------------------------------------------------------
// Guard (booster-specific anti-detection)
// ---------------------------------------------------------------------------

struct RiskEvent {
    int code = 0;
    std::string message;
    int severity = 0;      // 1..5
    int64_t atMs = 0;
};

class RbGuard {
public:
    void recordEvent(const RiskEvent& ev);
    bool riskLevel(const RbConfig& cfg) const;      // true = elevated
    int currentRisk() const { return risk_score_; }
    int64_t lastEventMs() const { return last_event_ms_; }
    std::string dump() const;
    void reset();

    // Behavioral checks
    bool checkMatchPattern(const std::vector<MatchRecord>& history,
                           const RbConfig& cfg) const;
    bool checkTimingPattern(const std::vector<MatchRecord>& history,
                            const RbConfig& cfg) const;
    bool checkPerformanceOutlier(const PlayerProfile& p,
                                 const RbConfig& cfg) const;

private:
    std::vector<RiskEvent> events_;
    int risk_score_ = 0;
    int64_t last_event_ms_ = 0;
};

// ---------------------------------------------------------------------------
// Scheduler
// ---------------------------------------------------------------------------

struct ScheduleSlot {
    int64_t startMs = 0;
    int64_t endMs = 0;
    bool active = false;
    std::string label;
};

class RbScheduler {
public:
    void setActiveWindow(int64_t startMs, int64_t endMs);
    bool inActiveWindow(int64_t nowMs) const;
    std::vector<ScheduleSlot> buildSessionPlan(int64_t nowMs, double hours) const;
    int64_t nextPauseMs(int64_t nowMs) const;
    void setSessionBudget(int matches) { budget_ = matches; }
    int matchesRemaining(int64_t nowMs) const;

private:
    int64_t window_start_ = 0;
    int64_t window_end_ = 0;
    int budget_ = 6;
    mutable int64_t used_ = 0;
};

// ---------------------------------------------------------------------------
// AI weighting
// ---------------------------------------------------------------------------

struct AiProfile {
    double aggression = 0.5;
    double riskTaking = 0.5;
    double mapAwareness = 0.6;
    double mechanicalSkill = 0.5;
    double macroSkill = 0.5;
    double decisionSpeed = 0.5;
    double heroDiversity = 0.3;
    double consistency = 0.5;
    double adaptation = 0.5;
    double emotionalStability = 0.7;
    double predictedPerformance = 50.0;

    std::string toString() const;
};

class RbAi {
public:
    AiProfile buildProfile(const PlayerProfile& p,
                           const RbStatistics& stats) const;
    double predictPerformance(const PlayerProfile& p, const RbStatistics& stats,
                              const RbConfig& cfg) const;
    double recommendedPlaystyleIndex(const AiProfile& profile,
                                     const RbConfig& cfg) const;
    std::string playstyleLabel(double index) const;
};

// ---------------------------------------------------------------------------
// Network / packet layer
// ---------------------------------------------------------------------------

struct LobbyPacket {
    uint8_t opcode = 0;
    uint8_t flags = 0;
    uint16_t sequence = 0;
    uint32_t sessionId = 0;
    uint64_t timestamp = 0;
    std::vector<uint8_t> payload;
};

class RbNetwork {
public:
    LobbyPacket craftJoinRequest(const PlayerProfile& p,
                                 const MatchmakingRequest& req) const;
    LobbyPacket craftReadySignal(const PlayerProfile& p, double delayMs) const;
    LobbyPacket craftPing(double rttMs) const;
    LobbyPacket craftHeartbeat(int64_t sessionId) const;

    // Traffic shaping
    std::vector<uint8_t> shapePacket(const std::vector<uint8_t>& raw) const;
    std::vector<uint8_t> unshapePacket(const std::vector<uint8_t>& shaped) const;

    double simulateRtt(double baseMs, double jitterMs) const;
    uint32_t nextSessionId() const;
    uint16_t nextSequence() const;

    uint64_t packetsSent() const { return packets_sent_; }
    uint64_t packetsShaped() const { return packets_shaped_; }

private:
    mutable uint32_t session_counter_ = 0x1000;
    mutable uint16_t seq_counter_ = 0;
    mutable uint64_t packets_sent_ = 0;
    mutable uint64_t packets_shaped_ = 0;
};

// ---------------------------------------------------------------------------
// Telemetry
// ---------------------------------------------------------------------------

class RbTelemetry {
public:
    static RbTelemetry& instance();
    void recordSessionStart(int64_t nowMs);
    void recordSessionEnd(int64_t nowMs);
    void recordMatch(const MatchRecord& rec);
    void recordQueueJoin(int64_t nowMs);
    void recordTunerRun(int64_t nowMs);
    void recordGuardEvent(int64_t nowMs);
    void snapshot(BoosterTelemetry& out) const;
    std::string dump() const;
    void reset();
    const BoosterTelemetry& current() const { return t_; }

private:
    RbTelemetry() = default;
    BoosterTelemetry t_;
    int64_t session_start_ = 0;
    mutable std::mutex mutex_;
};

// ---------------------------------------------------------------------------
// Cache
// ---------------------------------------------------------------------------

class RbCache {
public:
    static RbCache& instance();
    void setBaseDir(const std::string& dir) { base_dir_ = dir; }

    bool put(const std::string& key, const std::string& value, int64_t ttlMs);
    bool get(const std::string& key, std::string& out);
    bool has(const std::string& key) const;
    void remove(const std::string& key);
    void flush();

    size_t size() const { return entries_.size(); }
    int64_t bytesStored() const;

private:
    struct Entry {
        std::string value;
        int64_t expiresMs = 0;
    };
    std::string base_dir_;
    mutable std::mutex mutex_;
    std::map<std::string, Entry> entries_;
};

// ---------------------------------------------------------------------------
// Utils
// ---------------------------------------------------------------------------

namespace rb_utils {
double clamp01(double v);
double clamp(double v, double lo, double hi);
int clampInt(int v, int lo, int hi);
double lerp(double a, double b, double t);
double damp(double current, double target, double lambda, double dt);
double sigmoid(double x);
double logit(double p);
double percentile(const std::vector<double>& values, double pct);
double median(std::vector<double> values);
double mean(const std::vector<double>& values);
double variance(const std::vector<double>& values);
double standardDeviation(const std::vector<double>& values);
int64_t hoursBetween(int64_t a, int64_t b);
std::string timestamp(int64_t ms);
std::string kdaString(const MatchResult& r);
double starEquivalents(const RankPoint& a, const RankPoint& b);
bool isSameTier(const RankPoint& a, const RankPoint& b);
int tierDistance(const RankPoint& a, const RankPoint& b);
double gaussianRandom(double mean, double sigma);
double jitter(double base, double amount);
std::string maskId(int64_t id);

// Extended utilities.
double normalize(double v, double lo, double hi);
double wma(const std::vector<double>& series,
           const std::vector<double>& weights);
double rollingSum(const std::vector<double>& series, int n);
double decayAverage(const std::vector<double>& series, double decay);
double triangleWave(double t, double lo, double hi, double period);
double sampleAt(const std::vector<double>& ys, double x);
double entropy(const std::vector<double>& probs);
std::vector<double> softmax(const std::vector<double>& scores);
double zscore(double v, const std::vector<double>& population);
std::vector<double> minMaxScale(const std::vector<double>& values);
int bucketIndex(double v, double lo, double hi, int buckets);
double roundTo(double v, int decimals);
}

// ---------------------------------------------------------------------------
// Protocol codec (packet-level framing/obfuscation for lobby traffic)
// ---------------------------------------------------------------------------

struct FramedPacket {
    uint16_t magic = 0xAF;
    uint8_t version = 1;
    uint8_t type = 0;
    uint16_t payloadLen = 0;
    uint32_t checksum = 0;
    std::vector<uint8_t> payload;
};

class ProtocolCodec {
public:
    // Frame + obfuscate a raw payload.
    FramedPacket encode(const LobbyPacket& pkt) const;
    LobbyPacket decode(const FramedPacket& framed) const;

    std::vector<uint8_t> frameRaw(const std::vector<uint8_t>& payload,
                                  uint8_t type) const;
    FramedPacket parse(const std::vector<uint8_t>& bytes) const;

    // Integrity + obfuscation primitives.
    uint32_t checksum(const std::vector<uint8_t>& data) const;
    uint32_t crc32(const uint8_t* data, size_t len) const;
    std::vector<uint8_t> obfuscate(const std::vector<uint8_t>& data) const;
    std::vector<uint8_t> deobfuscate(const std::vector<uint8_t>& data) const;

    // Session keys.
    void setKey(uint32_t key) { key_ = key; }
    uint32_t key() const { return key_; }

    // Diagnostics
    std::string dump(const FramedPacket& fp) const;

private:
    uint32_t key_ = 0x5A5AA5A5;
    mutable uint32_t counter_ = 0;
};

std::vector<uint8_t> wireFrame(const ProtocolCodec& codec,
                               const LobbyPacket& pkt);

// ---------------------------------------------------------------------------
// Engine orchestrator (drives the booster lifecycle loop)
// ---------------------------------------------------------------------------

struct EngineSnapshot {
    BoosterState state = BoosterState::kIdle;
    bool searching = false;
    int64_t uptimeMs = 0;
    int64_t matchesTracked = 0;
    double effectiveWinRate = 0.5;
    double predictedPerformance = 50.0;
    int guardRisk = 0;
    double difficulty = 0.5;
    int queuesSinceMatch = 0;
    std::string mode;
};

class RbEngine {
public:
    static RbEngine& instance();

    // Lifecycle.
    void start(const RbConfig& cfg);
    void stop();
    bool running() const { return running_.load(); }

    // Tick driven by the host loop (called from RankBooster::pump).
    void tick();

    // Session control.
    void beginSession();
    void endSession();
    void onMatchFinished(const MatchResult& result);

    // Behavior decisions.
    bool shouldStartQueue(const MatchmakingRequest& req) const;
    double queueDifficulty(const MatchmakingRequest& req) const;
    int64_t recommendedDelayBeforeAcceptMs() const;

    // Snapshots.
    EngineSnapshot snapshot() const;
    std::string summary() const;

private:
    RbEngine() = default;
    RbEngine(const RbEngine&) = delete;
    RbEngine& operator=(const RbEngine&) = delete;

    void advanceState(int64_t nowMs);

    std::atomic<bool> running_{false};
    std::atomic<BoosterState> state_{BoosterState::kIdle};
    int64_t started_ms_ = 0;
    int64_t last_tick_ms_ = 0;
    int64_t match_counter_ = 0;
    double session_win_rate_ = 0.5;
    bool session_active_ = false;
    MatchmakingRequest active_request_;
};

// ---------------------------------------------------------------------------
// Core manager
// ---------------------------------------------------------------------------

class RankBooster {
public:
    static RankBooster& instance();

    int start();
    int stop();
    bool running() const { return running_.load(); }
    BoosterState state() const { return state_.load(); }

    // Feature entry points (wired to FeatureSwitch).
    void setEnabled(bool enabled);
    void setAggression(int level);
    void setTargetRank(int rankAbs);
    void pump();                        // called every frame by the core
    void onMatchEnded(const MatchResult& result);

    // Session / queue lifecycle.
    void beginSession();
    void endSession();
    bool loadProfile(const std::string& baseDir);
    int startQueueCycle();
    int pollQueueCycle();
    void cancelQueueCycle();
    void processMatchEnd(const MatchResult& result);

    // Integration helpers (JNI / FeatureSwitch wiring).
    std::string statusBlob() const;
    double currentWinRate() const;
    bool initData(const std::string& baseDir);
    void flushAll();
    int evaluateGuardNow();
    void onLobbyEvent(int code, const std::vector<std::string>& members);
    void onGameMatchEnded(const MatchResult& result);
    double predictedDifficulty() const;
    int sessionMatchesLeft() const;
    std::string projectionText() const;
    std::string playstyleLabel() const;

    // Diagnostics / monitoring helpers.
    std::string hudLine() const;
    bool shouldAttemptQueueNow() const;
    StarEconomy currentEconomy() const;
    int sessionsToTargetNow() const;
    const char* guardBand() const;
    double weeklyStarDelta() const;

    // Accessors
    ProfileStore& profiles() { return profile_store_; }
    MatchmakingEngine& matchmaking() { return matchmaking_; }
    LobbyHooks& lobby() { return lobby_; }
    RbGuard& guard() { return guard_; }
    RbTelemetry& telemetry() { return telemetry_; }
    RbNetwork& network() { return network_; }
    RbCache& cache() { return cache_; }
    RbScheduler& scheduler() { return scheduler_; }
    RbAi& ai() { return ai_; }
    AdaptiveTuner& tuner() { return tuner_; }
    EloEngine& elo() { return elo_; }
    MMRModel& mmr() { return mmr_; }

    // Snapshot for UI/JNI
    std::string snapshot() const;
    std::string diag() const;

private:
    RankBooster();
    ~RankBooster();

    void runLoop();
    void applyConfigToComponents();
    MatchmakingRequest makeQueueRequest() const;
    void guardSweep();

    std::atomic<bool> running_{false};
    std::atomic<BoosterState> state_{BoosterState::kIdle};
    Thread thread_{"arift-rankbooster"};

    ProfileStore& profile_store_ = ProfileStore::instance();
    MatchmakingEngine matchmaking_;
    LobbyHooks lobby_;
    RbGuard guard_;
    RbTelemetry& telemetry_;
    RbNetwork network_;
    RbCache& cache_;
    RbScheduler scheduler_;
    RbAi ai_;
    AdaptiveTuner tuner_;
    EloEngine elo_;
    MMRModel mmr_;
    RbEngine& engine_;

    RbConfig config_;
    RbStatistics stats_;
    MatchmakingRequest queue_req_;
    int64_t last_tune_ms_ = 0;
    int64_t last_pump_ms_ = 0;
    int64_t last_queue_start_ms_ = 0;
    int64_t engine_delay_ms_ = 0;
    std::string base_dir_;
};

}  // namespace rb
}  // namespace arift