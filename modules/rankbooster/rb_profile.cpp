#include "rb_api.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "arift_fs.h"
#include "arift_log.h"
#include "arift_utils.h"
#include "rb_math.h"

namespace arift {
namespace rb {

// ---------------------------------------------------------------------------
// ProfileStore
// ---------------------------------------------------------------------------

ProfileStore& ProfileStore::instance() {
    static ProfileStore store;
    return store;
}

bool ProfileStore::load(const std::string& baseDir) {
    std::string profilePath = baseDir + "/rb_profile.txt";
    std::string content;
    if (!fs::readFile(profilePath, content)) {
        ARIFT_DEBUG(kTagRankBooster, "No profile file yet at %s", profilePath.c_str());
        return false;
    }
    for (const auto& line : utils::split(content, '\n')) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        if (key == "name") local_.name = val;
        else if (key == "region") local_.region = val;
        else if (key == "rank") local_.rank = deserializeRank(val);
        else if (key == "mmr") local_.mmr = std::atof(val.c_str());
        else if (key == "sigma") local_.sigma = std::atof(val.c_str());
        else if (key == "volatility") local_.volatility = std::atof(val.c_str());
        else if (key == "win_rate") local_.winRate = std::atof(val.c_str());
        else if (key == "matches") local_.matchesPlayed = std::atoll(val.c_str());
        else if (key == "wins") local_.matchesWon = std::atoll(val.c_str());
        else if (key == "losses") local_.matchesLost = std::atoll(val.c_str());
        else if (key == "streak") local_.streak = std::atoll(val.c_str());
        else if (key == "last_match") local_.lastMatchMs = std::atoll(val.c_str());
        else if (key == "protection") local_.protectedLoss = val == "1";
        else if (key == "performance") local_.performanceIndex = std::atof(val.c_str());
    }

    std::string histPath = baseDir + "/rb_history.txt";
    std::string histContent;
    if (fs::readFile(histPath, histContent)) {
        for (const auto& line : utils::split(histContent, '\n')) {
            if (line.empty()) continue;
            history_.push_back(deserializeMatch(line));
        }
    }
    ARIFT_INFO(kTagRankBooster, "Profile loaded: %lld matches",
               static_cast<long long>(history_.size()));
    return true;
}

bool ProfileStore::save(const std::string& baseDir) const {
    std::string profile;
    profile += "name=" + local_.name + "\n";
    profile += "region=" + local_.region + "\n";
    profile += "rank=" + serializeRank(local_.rank) + "\n";
    char buf[256];
    snprintf(buf, sizeof(buf), "mmr=%.2f\nsigma=%.2f\nvolatility=%.4f\nwin_rate=%.4f\n",
             local_.mmr, local_.sigma, local_.volatility, local_.winRate);
    profile += buf;
    snprintf(buf, sizeof(buf), "matches=%lld\nwins=%lld\nlosses=%lld\nstreak=%lld\n",
             static_cast<long long>(local_.matchesPlayed),
             static_cast<long long>(local_.matchesWon),
             static_cast<long long>(local_.matchesLost),
             static_cast<long long>(local_.streak));
    profile += buf;
    snprintf(buf, sizeof(buf), "last_match=%lld\nprotection=%d\nperformance=%.2f\n",
             static_cast<long long>(local_.lastMatchMs),
             local_.protectedLoss ? 1 : 0, local_.performanceIndex);
    profile += buf;
    fs::writeFile(baseDir + "/rb_profile.txt", profile);

    std::string hist;
    size_t start = history_.size() > 200 ? history_.size() - 200 : 0;
    for (size_t i = start; i < history_.size(); ++i) {
        hist += serializeMatch(history_[i]) + "\n";
    }
    fs::writeFile(baseDir + "/rb_history.txt", hist);
    return true;
}

void ProfileStore::recordMatch(const MatchRecord& rec) {
    history_.push_back(rec);
    if (history_.size() > 500) {
        history_.erase(history_.begin(), history_.begin() + (history_.size() - 500));
    }

    local_.matchesPlayed += 1;
    if (rec.result.won) {
        local_.matchesWon += 1;
        local_.streak = local_.streak > 0 ? local_.streak + 1 : 1;
        if (local_.streak > 0) local_.protectedLoss = true;
    } else {
        local_.matchesLost += 1;
        local_.streak = local_.streak < 0 ? local_.streak - 1 : -1;
        if (local_.protectedLoss) {
            local_.protectedLoss = false;
        }
    }
    local_.rank = rec.rankAfter;
    local_.mmr += rec.mmrDelta;
    local_.winRate = local_.matchesPlayed > 0
                         ? static_cast<double>(local_.matchesWon) /
                               static_cast<double>(local_.matchesPlayed)
                         : 0.5;
    local_.lastMatchMs = rec.endedAtMs;
    local_.performanceIndex = rb_utils::lerp(
        local_.performanceIndex,
        static_cast<double>(rec.result.performancePercentile), 0.3);
}

double ProfileStore::rollingWinRate(int64_t windowMs) const {
    if (history_.empty()) return 0.5;
    int64_t cutoff = utils::monotonicMs() - windowMs;
    int64_t total = 0;
    int64_t wins = 0;
    for (auto it = history_.rbegin(); it != history_.rend(); ++it) {
        if (it->endedAtMs < cutoff) break;
        total += 1;
        if (it->result.won) wins += 1;
    }
    if (total == 0) return local_.winRate;
    return static_cast<double>(wins) / static_cast<double>(total);
}

void ProfileStore::reset() {
    local_ = PlayerProfile{};
    history_.clear();
}

// ---------------------------------------------------------------------------
// Extended profile helpers
// ---------------------------------------------------------------------------

namespace {

// Parse "k:d:a" style KDA strings.
void parseKda(const std::string& s, int& k, int& d, int& a) {
    auto parts = utils::split(s, '/');
    if (parts.size() >= 1) k = std::atoi(parts[0].c_str());
    if (parts.size() >= 2) d = std::atoi(parts[1].c_str());
    if (parts.size() >= 3) a = std::atoi(parts[2].c_str());
}

}  // namespace

// Best recorded match (by performance percentile).
const MatchRecord* bestMatch(const std::vector<MatchRecord>& history) {
    const MatchRecord* best = nullptr;
    for (const auto& rec : history) {
        if (!best || rec.result.performancePercentile >
                         best->result.performancePercentile) {
            best = &rec;
        }
    }
    return best;
}

// Worst recorded match.
const MatchRecord* worstMatch(const std::vector<MatchRecord>& history) {
    const MatchRecord* worst = nullptr;
    for (const auto& rec : history) {
        if (!worst || rec.result.performancePercentile <
                          worst->result.performancePercentile) {
            worst = &rec;
        }
    }
    return worst;
}

// Win streak history (consecutive wins ending at the latest match).
int currentWinRun(const std::vector<MatchRecord>& history) {
    int run = 0;
    for (auto it = history.rbegin(); it != history.rend(); ++it) {
        if (!it->result.won) break;
        run += 1;
    }
    return run;
}

// MMR range spanned by the profile's history.
struct MmrRange {
    double min = 0.0;
    double max = 0.0;
    double span = 0.0;
};

MmrRange mmrRange(const std::vector<MatchRecord>& history) {
    MmrRange r;
    bool first = true;
    for (const auto& rec : history) {
        double v = rec.rankBefore.score > 0
                       ? static_cast<double>(rec.rankBefore.score)
                       : rec.mmrDelta;
        if (first) {
            r.min = r.max = v;
            first = false;
        } else {
            if (v < r.min) r.min = v;
            if (v > r.max) r.max = v;
        }
    }
    r.span = r.max - r.min;
    return r;
}

// KDA trend across history (smoothed kills/deaths/assists).
struct KdaTrend {
    double kills = 0.0;
    double deaths = 0.0;
    double assists = 0.0;
    double trend = 0.0;   // + improving, - declining
};

KdaTrend kdaTrend(const std::vector<MatchRecord>& history) {
    KdaTrend t;
    if (history.size() < 3) {
        for (const auto& rec : history) {
            t.kills += rec.result.kdaKills;
            t.deaths += rec.result.kdaDeaths;
            t.assists += rec.result.kdaAssists;
        }
        double n = static_cast<double>(std::max<size_t>(history.size(), 1));
        t.kills /= n;
        t.deaths /= n;
        t.assists /= n;
        return t;
    }
    std::vector<double> kills;
    std::vector<double> deaths;
    std::vector<double> assists;
    for (const auto& rec : history) {
        kills.push_back(static_cast<double>(rec.result.kdaKills));
        deaths.push_back(static_cast<double>(rec.result.kdaDeaths));
        assists.push_back(static_cast<double>(rec.result.kdaAssists));
    }
    t.kills = rb_utils::mean(kills);
    t.deaths = rb_utils::mean(deaths);
    t.assists = rb_utils::mean(assists);
    size_t half = kills.size() / 2;
    double early = rb_utils::mean(
        std::vector<double>(kills.begin(),
                            kills.begin() + static_cast<long>(half)));
    double late = rb_utils::mean(
        std::vector<double>(kills.begin() + static_cast<long>(half),
                            kills.end()));
    t.trend = late - early;
    return t;
}

// Match outcome pattern summary (W/L alternation).
struct OutcomePattern {
    int alternations = 0;
    int runs = 0;
    double alternationRate = 0.0;
};

OutcomePattern outcomePattern(const std::vector<MatchRecord>& history) {
    OutcomePattern o;
    if (history.size() < 2) return o;
    int prev = history[0].result.won ? 1 : 0;
    int runs = 1;
    for (size_t i = 1; i < history.size(); ++i) {
        int cur = history[i].result.won ? 1 : 0;
        if (cur == prev) {
            runs += 1;
        } else {
            o.alternations += 1;
            runs = 0;
        }
        prev = cur;
    }
    o.runs = runs;
    o.alternationRate = static_cast<double>(o.alternations) /
                        static_cast<double>(history.size() - 1);
    return o;
}

// Import a raw result into the store (used by match callbacks).
void importResult(ProfileStore& store, const MatchResult& result,
                  int64_t startedAtMs) {
    MatchRecord rec;
    rec.matchId = utils::monotonicMs();
    rec.startedAtMs = startedAtMs > 0 ? startedAtMs
                                      : utils::monotonicMs() - 600000;
    rec.endedAtMs = result.endedAtMs > 0 ? result.endedAtMs
                                         : utils::monotonicMs();
    rec.result = result;
    rec.mode = result.ranked ? "ranked" : "classic";
    rec.rankBefore = store.local().rank;
    rec.rankAfter = store.local().rank;
    store.recordMatch(rec);
}

// Write a human-readable summary of the store.
std::string profileSummary(const ProfileStore& store) {
    const PlayerProfile& p = store.local();
    char buf[512];
    snprintf(buf, sizeof(buf),
             "profile: %s [%s] rank=%s mmr=%.0f wr=%.1f%% matches=%lld "
             "streak=%lld",
             p.name.empty() ? "unnamed" : p.name.c_str(),
             p.region.empty() ? "unknown" : p.region.c_str(),
             p.rank.toString().c_str(), p.mmr, p.winRate * 100.0,
             static_cast<long long>(p.matchesPlayed),
             static_cast<long long>(p.streak));
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// RbStatistics
// ---------------------------------------------------------------------------

void RbStatistics::compute(const PlayerProfile& p,
                           const std::vector<MatchRecord>& history) {
    totalMatches = static_cast<int64_t>(history.size());
    wins = 0;
    losses = 0;
    avgMmrDelta = 0.0;
    avgPerformance = 0.0;
    avgKills = 0.0;
    avgDeaths = 0.0;
    avgAssists = 0.0;
    double mvpCount = 0.0;
    double rankedCount = 0.0;
    double durationSum = 0.0;
    double survival = 0.0;
    int64_t bestS = 0;
    int64_t worstS = 0;
    int64_t streak = 0;
    int64_t matchesThisWeek = 0;
    int64_t matchesThisMonth = 0;
    double firstBlood = 0.0;
    double comeback = 0.0;
    double earlyWins = 0.0;
    double earlyTotal = 0.0;
    double lateWins = 0.0;
    double lateTotal = 0.0;

    int64_t nowMs = utils::monotonicMs();

    for (const auto& rec : history) {
        if (rec.result.won) {
            wins += 1;
            streak = streak > 0 ? streak + 1 : 1;
        } else {
            losses += 1;
            streak = streak < 0 ? streak - 1 : -1;
        }
        if (streak > bestS) bestS = streak;
        if (streak < worstS) worstS = streak;

        avgMmrDelta += rec.mmrDelta;
        avgPerformance += static_cast<double>(rec.result.performancePercentile);
        avgKills += static_cast<double>(rec.result.kdaKills);
        avgDeaths += static_cast<double>(rec.result.kdaDeaths);
        avgAssists += static_cast<double>(rec.result.kdaAssists);
        durationSum += rec.result.durationMin;
        if (rec.result.mvpScore >= 10) mvpCount += 1.0;
        if (rec.result.ranked) rankedCount += 1.0;
        if (rec.result.kdaDeaths == 0) survival += 1.0;
        if (rec.result.kdaKills >= 1) firstBlood += 0.1;
        if (rec.result.enemyScore > 0 && rec.result.won) comeback += 1.0;

        double dur = rec.result.durationMin;
        if (dur < 12.0) {
            earlyTotal += 1.0;
            if (rec.result.won) earlyWins += 1.0;
        } else {
            lateTotal += 1.0;
            if (rec.result.won) lateWins += 1.0;
        }

        int64_t age = nowMs - rec.endedAtMs;
        if (age <= 7 * 86400000) matchesThisWeek += 1;
        if (age <= 30LL * 86400000) matchesThisMonth += 1;
    }

    int64_t n = totalMatches;
    winRate = n > 0 ? static_cast<double>(wins) / static_cast<double>(n) : 0.5;
    recentWinRate = p.winRate;
    avgMmrDelta = n > 0 ? avgMmrDelta / static_cast<double>(n) : 0.0;
    avgPerformance = n > 0 ? avgPerformance / static_cast<double>(n) : 50.0;
    avgKills = n > 0 ? avgKills / static_cast<double>(n) : 0.0;
    avgDeaths = n > 0 ? avgDeaths / static_cast<double>(n) : 0.0;
    avgAssists = n > 0 ? avgAssists / static_cast<double>(n) : 0.0;
    kdaRatio = avgDeaths > 0.0 ? (avgKills + avgAssists) / avgDeaths
                               : avgKills + avgAssists;
    mvpRate = n > 0 ? mvpCount / static_cast<double>(n) : 0.0;
    survivalRate = n > 0 ? survival / static_cast<double>(n) : 0.0;
    rankedMatchShare = n > 0 ? rankedCount / static_cast<double>(n) : 0.0;
    currentStreak = p.streak;
    bestStreak = bestS;
    worstStreak = worstS;
    sigma = p.sigma;
    volatility = p.volatility;
    avgMatchDuration = n > 0 ? durationSum / static_cast<double>(n) : 0.0;
    firstBloodRate = firstBlood;
    comebackRate = comeback;
    earlyGameWinRate = earlyTotal > 0.0 ? earlyWins / earlyTotal : 0.5;
    lateGameWinRate = lateTotal > 0.0 ? lateWins / lateTotal : 0.5;
    longestMatchMin = 0;
    shortestMatchMin = 0;
    for (const auto& rec : history) {
        int64_t d = static_cast<int64_t>(rec.result.durationMin);
        if (d > longestMatchMin) longestMatchMin = d;
        if (shortestMatchMin == 0 || (d > 0 && d < shortestMatchMin)) shortestMatchMin = d;
    }

    // Projections.
    auto eloStars = [](RankTier tier) -> int {
        switch (tier) {
            case RankTier::kWarrior:
            case RankTier::kElite: return 2;
            default: return 1;
        }
    };
    double winStars = static_cast<double>(eloStars(p.rank.tier));
    double lossStars = static_cast<double>(eloStars(p.rank.tier));
    expectedStarsPerMatch = ::arift::rb::expectedStarsPerMatch(winRate, winStars, lossStars);
    projectedStarsPerDay = expectedStarsPerMatch * 8.0;
    mmrProgress = rb_utils::clamp01((p.mmr - 900.0) / 2400.0);
}

std::string RbStatistics::toString() const {
    char buf[1024];
    snprintf(buf, sizeof(buf),
             "matches=%lld (W:%lld L:%lld) wr=%.1f%% recent=%.1f%% "
             "streak=%lld best=%lld worst=%lld\n"
             "mmr_delta_avg=%.2f sigma=%.2f vol=%.3f perf=%.1f kda=%.1f "
             "mvp=%.0f%%\n"
             "stars/match=%.3f stars/day=%.2f prog=%.0f%%",
             static_cast<long long>(totalMatches),
             static_cast<long long>(wins),
             static_cast<long long>(losses),
             winRate * 100.0,
             recentWinRate * 100.0,
             static_cast<long long>(currentStreak),
             static_cast<long long>(bestStreak),
             static_cast<long long>(worstStreak),
             avgMmrDelta,
             sigma,
             volatility,
             avgPerformance,
             kdaRatio,
             mvpRate * 100.0,
             expectedStarsPerMatch,
             projectedStarsPerDay,
             mmrProgress * 100.0);
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Roster and milestones
// ---------------------------------------------------------------------------

// Milestone tracking: rank/matches/wins thresholds reached.
struct MilestoneState {
    bool reachedEpic = false;
    bool reachedMythic = false;
    bool reachedGlory = false;
    int matches50 = 0;
    int wins100 = 0;
};

MilestoneState milestoneState(const PlayerProfile& p) {
    MilestoneState m;
    m.reachedEpic = static_cast<int>(p.rank.tier) >=
                    static_cast<int>(RankTier::kEpic);
    m.reachedMythic = static_cast<int>(p.rank.tier) >=
                      static_cast<int>(RankTier::kMythic);
    m.reachedGlory = static_cast<int>(p.rank.tier) >=
                     static_cast<int>(RankTier::kMythicalGlory);
    m.matches50 = static_cast<int>(p.matchesPlayed / 50);
    m.wins100 = static_cast<int>(p.matchesWon / 100);
    return m;
}

// Milestones remaining before the configured target.
std::vector<std::string> upcomingMilestones(const PlayerProfile& p,
                                            const RankPoint& target) {
    std::vector<std::string> out;
    if (p.rank.absolute() < 600) out.push_back("reach Epic");
    if (p.rank.absolute() < 700) out.push_back("reach Mythic");
    if (p.rank.absolute() < 800) out.push_back("reach Mythical Glory");
    if (p.matchesPlayed < 100) out.push_back("play 100 matches");
    if (p.matchesWon < 100) out.push_back("win 100 matches");
    if (target.absolute() > p.rank.absolute()) {
        out.push_back("reach " + target.toString());
    }
    return out;
}

// Bookmark the current position (for crash-safe resume).
SessionBookmark makeBookmark(const PlayerProfile& p) {
    SessionBookmark b;
    b.savedAtMs = utils::monotonicMs();
    b.rank = p.rank;
    b.mmr = p.mmr;
    b.matchesPlayed = static_cast<int>(p.matchesPlayed);
    b.note = "auto";
    return b;
}

// Session gap between now and the last recorded activity.
double sessionGapHours(const PlayerProfile& p) {
    if (p.lastMatchMs <= 0) return 24.0;
    int64_t gap = utils::monotonicMs() - p.lastMatchMs;
    if (gap < 0) gap = 0;
    return static_cast<double>(gap) / 3600000.0;
}

// Weekly activity total (matches in the last 7 days).
int weeklyMatches(const std::vector<MatchRecord>& history) {
    int64_t cutoff = utils::monotonicMs() - 7LL * 86400000;
    int n = 0;
    for (const auto& rec : history) {
        if (rec.endedAtMs >= cutoff) n += 1;
    }
    return n;
}

// ETA to next milestone in days at the current pace.
double etaDaysToMilestone(const PlayerProfile& p, const RbStatistics& stats,
                          const RankPoint& milestone) {
    int stars = std::max(0, milestone.absolute() - p.rank.absolute());
    double ev = stats.expectedStarsPerMatch;
    if (ev <= 0.0) return 1e9;
    double matches = static_cast<double>(stars) / ev;
    return matches / 8.0;
}

// Profile age in days (from first recorded match).
double profileAgeDays(const std::vector<MatchRecord>& history) {
    if (history.empty()) return 0.0;
    int64_t first = history.front().startedAtMs;
    int64_t now = utils::monotonicMs();
    if (now <= first) return 0.0;
    return static_cast<double>(now - first) / 86400000.0;
}

}  // namespace rb
}  // namespace arift