#include "rb_api.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

#include "arift_utils.h"
#include "rb_math.h"

namespace arift {
namespace rb {

// ---------------------------------------------------------------------------
// RbScheduler — session windows, budgets, pacing.
// ---------------------------------------------------------------------------

void RbScheduler::setActiveWindow(int64_t startMs, int64_t endMs) {
    window_start_ = startMs;
    window_end_ = endMs;
}

bool RbScheduler::inActiveWindow(int64_t nowMs) const {
    if (window_start_ == 0 || window_end_ == 0) return true;
    return nowMs >= window_start_ && nowMs < window_end_;
}

std::vector<ScheduleSlot> RbScheduler::buildSessionPlan(int64_t nowMs,
                                                        double hours) const {
    std::vector<ScheduleSlot> slots;
    double minutes = hours * 60.0;
    int matches = static_cast<int>(minutes / 25.0);
    for (int i = 0; i < matches; ++i) {
        ScheduleSlot s;
        s.startMs = nowMs + static_cast<int64_t>(i) * 1500000LL;
        s.endMs = s.startMs + 900000LL;
        s.active = true;
        s.label = "match_" + std::to_string(i + 1);
        slots.push_back(s);
    }
    // Insert rest slots after every 3rd match.
    size_t idx = 3;
    while (idx < slots.size()) {
        ScheduleSlot rest;
        rest.startMs = slots[idx - 1].endMs;
        rest.endMs = rest.startMs + 120000LL;
        rest.active = false;
        rest.label = "rest";
        slots.insert(slots.begin() + static_cast<long>(idx), rest);
        idx += 4;
    }
    return slots;
}

int64_t RbScheduler::nextPauseMs(int64_t nowMs) const {
    // Pause every 3 matches.
    return nowMs + 3LL * 1500000LL;
}

int RbScheduler::matchesRemaining(int64_t nowMs) const {
    int64_t elapsed = nowMs - window_start_;
    int played = static_cast<int>(elapsed / 1500000LL);
    return std::max(0, budget_ - played);
}

// ---------------------------------------------------------------------------
// Session pacing helpers
// ---------------------------------------------------------------------------

// Fatigue-adjusted star expectation across a session (diminishing returns).
double sessionStarExpectation(double winRate, int starsWin, int starsLoss,
                              int matches) {
    double total = 0.0;
    double currentWr = winRate;
    for (int i = 0; i < matches; ++i) {
        double fatigue = 1.0 - 0.03 * static_cast<double>(i);
        double wr = currentWr * std::max(0.5, fatigue);
        total += wr * static_cast<double>(starsWin) -
                 (1.0 - wr) * static_cast<double>(starsLoss);
    }
    return total;
}

// Optimal rest length given match duration statistics.
int64_t restForDuration(double avgMatchMinutes) {
    double restMin = 3.0 + avgMatchMinutes * 0.35;
    return static_cast<int64_t>(restMin * 60000.0);
}

// Rolling day budget cap based on historical cadence.
int dailyBudgetFromCadence(const std::vector<MatchRecord>& history) {
    if (history.size() < 10) return 12;
    int64_t now = utils::monotonicMs();
    int64_t dayStart = now - (now % 86400000);
    int today = 0;
    for (const auto& rec : history) {
        if (rec.endedAtMs >= dayStart) today += 1;
    }
    return std::max(0, 12 - today);
}

// Best time-of-day to queue given playtime analysis (0..23).
int bestQueueHour(const std::vector<MatchRecord>& history) {
    if (history.empty()) return 20;
    std::vector<int> hours(24, 0);
    for (const auto& rec : history) {
        time_t sec = static_cast<time_t>(rec.endedAtMs / 1000);
        struct tm tmv{};
        localtime_r(&sec, &tmv);
        hours[static_cast<size_t>(tmv.tm_hour)] += 1;
    }
    int best = 0;
    for (size_t i = 1; i < hours.size(); ++i) {
        if (hours[i] > hours[static_cast<size_t>(best)]) best = static_cast<int>(i);
    }
    return best;
}

// ---------------------------------------------------------------------------
// Session calendar
// ---------------------------------------------------------------------------

namespace {

// Convert ms to minutes since midnight (local time).
int minutesOfDay(int64_t ms) {
    time_t sec = static_cast<time_t>(ms / 1000);
    struct tm tmv{};
    localtime_r(&sec, &tmv);
    return tmv.tm_hour * 60 + tmv.tm_min;
}

}  // namespace

// Map a time-of-day (minutes) to a queue-favorability score 0..1.
double timeOfDayScore(int minutes) {
    // Favorability peaks: 19:00-23:00, 12:00-14:00.
    double score = 0.3;
    if (minutes >= 1140 && minutes <= 1380) score = 0.9;
    if (minutes >= 720 && minutes <= 840) score = 0.7;
    if (minutes >= 540 && minutes <= 660) score = 0.5;
    if (minutes >= 900 && minutes <= 1080) score = 0.6;
    return score;
}

// Weekly pattern: which weekdays the account historically queues.
std::vector<double> weekdayProfile(const std::vector<MatchRecord>& history) {
    std::vector<double> days(7, 0.0);
    std::vector<int> counts(7, 0);
    for (const auto& rec : history) {
        time_t sec = static_cast<time_t>(rec.endedAtMs / 1000);
        struct tm tmv{};
        localtime_r(&sec, &tmv);
        int wd = tmv.tm_wday;
        counts[static_cast<size_t>(wd)] += 1;
    }
    int total = 0;
    for (int c : counts) total += c;
    if (total == 0) return days;
    for (size_t i = 0; i < days.size(); ++i) {
        days[i] = static_cast<double>(counts[i]) / static_cast<double>(total);
    }
    return days;
}

// Session length distribution (minutes) summary.
struct SessionLengthStats {
    double meanMin = 20.0;
    double p10Min = 10.0;
    double p90Min = 30.0;
    int sessions = 0;
};

SessionLengthStats sessionLengthStats(const std::vector<MatchRecord>& history) {
    SessionLengthStats out;
    std::vector<double> lens;
    // Approximate session length from consecutive matches with short gaps.
    double current = 0.0;
    int64_t lastEnd = -1;
    for (const auto& rec : history) {
        if (lastEnd > 0 && rec.startedAtMs - lastEnd > 15 * 60000) {
            if (current > 0.0) lens.push_back(current);
            current = 0.0;
        }
        current += rec.result.durationMin;
        lastEnd = rec.endedAtMs;
    }
    if (current > 0.0) lens.push_back(current);
    if (!lens.empty()) {
        out.meanMin = rb_utils::mean(lens);
        out.p10Min = rb_utils::percentile(lens, 0.1);
        out.p90Min = rb_utils::percentile(lens, 0.9);
        out.sessions = static_cast<int>(lens.size());
    }
    return out;
}

// Recommend today's session window given history.
struct RecommendedWindow {
    int64_t startMs = 0;
    int64_t endMs = 0;
    int matches = 0;
    double score = 0.0;
};

RecommendedWindow recommendWindow(const std::vector<MatchRecord>& history,
                                   int64_t nowMs) {
    RecommendedWindow out;
    SessionLengthStats sl = sessionLengthStats(history);
    int bestHour = bestQueueHour(history);

    time_t sec = static_cast<time_t>(nowMs / 1000);
    struct tm tmv{};
    localtime_r(&sec, &tmv);
    tmv.tm_hour = bestHour;
    tmv.tm_min = 0;
    tmv.tm_sec = 0;
    time_t start = mktime(&tmv);
    out.startMs = static_cast<int64_t>(start) * 1000;
    out.endMs = out.startMs + static_cast<int64_t>(sl.meanMin * 4.0) * 60000;
    out.matches = std::max(3, static_cast<int>(4.0 * 25.0 / sl.meanMin));
    out.score = timeOfDayScore(minutesOfDay(out.startMs));
    return out;
}

// Daily budget tracking struct.
struct DailyBudget {
    int64_t day = 0;
    int used = 0;
    int limit = 12;

    void resetIfNewDay(int64_t nowMs) {
        int64_t d = nowMs / 86400000;
        if (d != day) {
            day = d;
            used = 0;
        }
    }

    bool canQueue() const { return used < limit; }
    void consume() { used += 1; }
};

static DailyBudget& dailyBudget() {
    static DailyBudget b;
    return b;
}

// Consume one slot from the daily budget; false if over limit.
bool tryConsumeDailySlot(int64_t nowMs) {
    DailyBudget& b = dailyBudget();
    b.resetIfNewDay(nowMs);
    if (!b.canQueue()) return false;
    b.consume();
    return true;
}

// Remaining daily slots.
int dailySlotsRemaining(int64_t nowMs) {
    DailyBudget& b = dailyBudget();
    b.resetIfNewDay(nowMs);
    return std::max(0, b.limit - b.used);
}

// ---------------------------------------------------------------------------
// Calendar-aware scheduling
// ---------------------------------------------------------------------------

namespace {

// Day-of-week index for a timestamp (0 = Sunday).
int weekdayOf(int64_t ms) {
    time_t sec = static_cast<time_t>(ms / 1000);
    struct tm tmv{};
    localtime_r(&sec, &tmv);
    return tmv.tm_wday;
}

// Is this a weekend (Fri evening .. Sun) per typical gaming patterns?
bool isWeekend(int64_t ms) {
    int wd = weekdayOf(ms);
    time_t sec = static_cast<time_t>(ms / 1000);
    struct tm tmv{};
    localtime_r(&sec, &tmv);
    int hour = tmv.tm_hour;
    if (wd == 0 || wd == 6) return true;
    if (wd == 5 && hour >= 17) return true;
    return false;
}

}  // namespace

// Weekend multiplier for queue activity (more players online).
double weekendQueueMultiplier(int64_t nowMs) {
    return isWeekend(nowMs) ? 1.4 : 1.0;
}

// Recommended match count for a calendar day.
int dailyRecommendation(int64_t nowMs,
                        const std::vector<MatchRecord>& history) {
    int budget = 12;
    if (isWeekend(nowMs)) budget = 16;
    SessionLengthStats sl = sessionLengthStats(history);
    if (sl.meanMin > 22.0) budget = static_cast<int>(budget * 0.8);
    return budget;
}

// Break schedule: minutes of rest after each match.
std::vector<double> breakSchedule(int matches, double baseRestMin) {
    std::vector<double> out;
    for (int i = 0; i < matches; ++i) {
        double rest = baseRestMin;
        if ((i + 1) % 3 == 0) rest *= 2.0;     // longer break every 3rd
        if ((i + 1) % 5 == 0) rest *= 1.5;     // extra after 5th
        out.push_back(rest);
    }
    return out;
}

// Star pace required to reach a target by a deadline.
double requiredPace(int starsRemaining, int daysRemaining) {
    if (daysRemaining <= 0) return 0.0;
    return static_cast<double>(starsRemaining) /
           static_cast<double>(daysRemaining);
}

// Feasibility: can the target be reached at current pace?
bool paceFeasible(double requiredPerDay, double currentPerDay) {
    return currentPerDay >= requiredPerDay * 0.85;
}

// Session spread: distribute matches across a window with rest.
struct SessionSpread {
    std::vector<int64_t> startTimes;
    std::vector<int64_t> endTimes;
};

SessionSpread spreadMatches(int64_t windowStartMs, int64_t windowEndMs,
                            int matches) {
    SessionSpread out;
    if (matches <= 0) return out;
    int64_t span = windowEndMs - windowStartMs;
    if (span <= 0) {
        for (int i = 0; i < matches; ++i) {
            out.startTimes.push_back(windowStartMs);
            out.endTimes.push_back(windowStartMs + 900000);
        }
        return out;
    }
    // Exponential-ish spacing (denser early, tapering late).
    for (int i = 0; i < matches; ++i) {
        double frac = static_cast<double>(i) / static_cast<double>(matches - 1);
        double eased = frac * frac;   // quadratic taper
        int64_t t = windowStartMs + static_cast<int64_t>(eased * span);
        out.startTimes.push_back(t);
        out.endTimes.push_back(t + 900000);
    }
    return out;
}

// Time until the next natural break (ms).
int64_t timeUntilBreak(int64_t nowMs, const std::vector<int64_t>& breakTimes) {
    for (int64_t b : breakTimes) {
        if (b > nowMs) return b - nowMs;
    }
    return 0;
}

}  // namespace rb
}  // namespace arift