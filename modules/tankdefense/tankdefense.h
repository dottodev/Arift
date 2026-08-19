#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "arift_thread.h"
#include "esp_types.h"

namespace arift {
namespace tankdefense {

// Trigger condition for a defensive action.
enum class TriggerKind {
    kLowHp = 0,
    kCcEscaped = 1,
    kTeamFight = 2,
    kUnderTower = 3,
    kTimer = 4,
};

// Defensive action type.
enum class DefenseAction {
    kNone = 0,
    kActivateItem = 1,   // Aegis / Vengeance / Winter Truncheon
    kCastShield = 2,     // shield/heal skill
    kCastEscape = 3,     // blink / cleanse
    kGuard = 4,          // passive guard posture
};

// Tank Defense config.
struct TankDefenseConfig {
    bool enabled = true;
    bool autoItems = true;
    bool autoShields = true;
    bool autoEscape = true;
    bool towerGuard = true;
    float lowHpThreshold = 0.35f;     // activate under this HP
    float criticalHp = 0.20f;         // critical band
    float teamFightRadius = 900.0f;
    int itemCooldownMs = 8000;
    int escapeCooldownMs = 15000;
    int shieldCooldownMs = 6000;
    int guardCooldownMs = 20000;
    int aggressionBias = 50;          // 0..100 (higher = more trigger-happy)
};

// Trigger evaluation result.
struct TriggerEval {
    bool triggered = false;
    TriggerKind kind = TriggerKind::kLowHp;
    DefenseAction action = DefenseAction::kNone;
    float score = 0.0f;
    std::string reason;
    int itemSlot = -1;
    int skillIndex = -1;
};

// Defensive item state.
struct DefensiveItem {
    std::string name;
    int slot = -1;
    int64_t usedAtMs = 0;
    int cooldownMs = 8000;
    float value = 0.0f;
    bool active = true;
};

// Team fight context snapshot.
struct FightContext {
    bool inFight = false;
    int alliesNear = 0;
    int enemiesNear = 0;
    float avgAllyHp = 1.0f;
    float nearestEnemyDist = 1e9f;
    bool underTower = false;
    int64_t fightStartMs = 0;
};

// Free functions for the bridge.
void enable(bool on);
bool enabled();
void setThreshold(float hp);
void setAutoItems(bool on);
TriggerEval evaluate(const PlayerSnapshot& self,
                     const std::vector<PlayerSnapshot>& players,
                     const std::vector<DefensiveItem>& items,
                     int64_t nowMs);
void execute(const TriggerEval& eval, int64_t nowMs);
std::string tankDefenseDiag();

// Module class.
class TankDefense {
public:
    static TankDefense& instance();

    int start();
    int stop();
    bool running() const { return running_.load(); }

    void setEnabled(bool v);
    void setThreshold(float hp);
    void setAutoItems(bool v);

    void feed(const PlayerSnapshot& self,
              const std::vector<PlayerSnapshot>& players,
              const std::vector<DefensiveItem>& items, int64_t nowMs);
    TriggerEval lastEval() const;
    std::string diag() const;
    const TankDefenseConfig& cfg() const;
    const PlayerSnapshot& selfRef() const;

private:
    TankDefense() = default;
    ~TankDefense() { stop(); }
    void loop();

    Thread thread_{"arift-tankdefense"};
    std::atomic<bool> running_{false};
    TankDefenseConfig cfg_;
    mutable std::mutex state_mutex_;
    PlayerSnapshot self_;
    std::vector<PlayerSnapshot> players_;
    std::vector<DefensiveItem> items_;
    int64_t nowMs_ = 0;
    TriggerEval lastEval_;
};

}  // namespace tankdefense
}  // namespace arift