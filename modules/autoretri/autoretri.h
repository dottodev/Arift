#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "arift_thread.h"
#include "esp_types.h"

namespace arift {
namespace autoretri {

// Pickup priority: high-value items first, then buffs, then gold.
enum class PickupPriority {
    kNone = 0,
    kItem = 1,       // equipment drops
    kBuff = 2,       // buff camps (blood buff, turtle)
    kGold = 3,       // gold coins on the ground
    kExp = 4,        // exp orbs / minion orbs
};

// Item decision when a dropped item is picked up.
enum class ItemAction {
    kKeep = 0,
    kSell = 1,
    kEquip = 2,
    kUpgrade = 3,
};

// Auto-shop rule: when to auto-buy a recommended item.
enum class ShopMode {
    kOff = 0,
    kRecommended = 1,   // buy from the recommended list only
    kSmart = 2,         // buy based on hero role + gold
};

// Runtime config for the Auto Retri module.
struct AutoRetriConfig {
    bool enabled = true;
    bool autoPickup = true;
    bool autoBuy = false;
    bool autoSell = true;
    bool autoEquip = true;
    bool pickupItems = true;
    bool pickupBuffs = true;
    bool pickupGold = true;
    bool pickupExp = true;
    float pickupRadius = 260.0f;      // game units
    int pickupCooldownMs = 400;       // min gap between pickups
    int sellThresholdGold = 1200;     // sell dropped items above this gold
    int buyBudgetRatio = 60;          // % of gold kept for buying
    ShopMode shopMode = ShopMode::kSmart;
    int buyLevelGap = 2;              // buy when 2+ levels ahead of items
};

// A pickup candidate on the ground.
struct PickupCandidate {
    PickupPriority priority = PickupPriority::kNone;
    Vec3 position;
    std::string name;
    float value = 0.0f;               // gold value / buff strength
    float distance = 0.0f;
    int64_t firstSeenMs = 0;
    int64_t expiresMs = 0;            // 0 = never
};

// A ground pickup sweep result.
struct PickupSweep {
    bool any = false;
    int count = 0;
    PickupPriority best = PickupPriority::kNone;
    float bestDistance = 0.0f;
    std::string bestName;
    Vec3 bestPosition;
};

// Inventory summary used by the shop logic.
struct InventorySummary {
    int slots = 6;
    int used = 0;
    int free() const { return slots - used; }
    int gold = 0;
    float totalItemValue = 0.0f;
    int level = 1;
    bool hasSellable = false;
};

// Purchase recommendation from the shop logic.
struct BuyRecommendation {
    bool shouldBuy = false;
    std::string item;
    int cost = 0;
    int priority = 0;
    std::string reason;
};

// Item auto-sell decision.
struct SellDecision {
    bool sell = false;
    std::string item;
    int gain = 0;
    std::string reason;
};

// Free functions exposed to the host / JNI bridge.
void enable(bool on);
bool enabled();
void setPickupRadius(float radius);
void setAutoBuy(bool on);
void setShopMode(int mode);
void sweepGround(const std::vector<PlayerSnapshot>& players,
                 const std::vector<PickupCandidate>& ground,
                 const Vec3& heroPos, int64_t nowMs);
PickupSweep lastSweep();
std::string pickupsLine();
std::string autoRetriDiag();

// Module class (used by the bridge when a full instance is needed).
class AutoRetri {
public:
    static AutoRetri& instance();

    int start();
    int stop();
    bool running() const { return running_.load(); }

    void setEnabled(bool v);
    void setPickupRadius(float r);
    void setAutoBuy(bool v);
    void setShopMode(int m);

    void feedEntities(const std::vector<PlayerSnapshot>& players,
                      const std::vector<PickupCandidate>& ground,
                      const Vec3& heroPos, int64_t nowMs);
    std::string diag() const;
    float cfgRadius() const;
    const AutoRetriConfig& cfg() const;

private:
    AutoRetri() = default;
    ~AutoRetri() { stop(); }
    void loop();

    Thread thread_{"arift-autoretri"};
    std::atomic<bool> running_{false};
    AutoRetriConfig cfg_;
    std::mutex state_mutex_;
    std::vector<PickupCandidate> ground_;
    std::vector<PlayerSnapshot> players_;
    Vec3 heroPos_;
    int64_t nowMs_ = 0;
    std::vector<PickupSweep> sweepHistory_;
};

}  // namespace autoretri
}  // namespace arift