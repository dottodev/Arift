#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace arift {

// ---------------------------------------------------------------------------
// ESP shared types
// ---------------------------------------------------------------------------

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;

    Vec2() = default;
    Vec2(float px, float py) : x(px), y(py) {}

    float length() const { return sqrtf(x * x + y * y); }
    Vec2 operator+(const Vec2& o) const { return Vec2(x + o.x, y + o.y); }
    Vec2 operator-(const Vec2& o) const { return Vec2(x - o.x, y - o.y); }
    Vec2 operator*(float s) const { return Vec2(x * s, y * s); }
};

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    Vec3() = default;
    Vec3(float px, float py, float pz) : x(px), y(py), z(pz) {}

    float length() const {
        return sqrtf(x * x + y * y + z * z);
    }
    float distanceTo(const Vec3& o) const {
        float dx = x - o.x;
        float dy = y - o.y;
        float dz = z - o.z;
        return sqrtf(dx * dx + dy * dy + dz * dz);
    }
    Vec3 operator+(const Vec3& o) const { return Vec3(x + o.x, y + o.y, z + o.z); }
    Vec3 operator-(const Vec3& o) const { return Vec3(x - o.x, y - o.y, z - o.z); }
    Vec3 operator*(float s) const { return Vec3(x * s, y * s, z * s); }
};

struct Rect2D {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;

    float width() const { return right - left; }
    float height() const { return bottom - top; }
    bool valid() const { return width() > 0.0f && height() > 0.0f; }
    Vec2 center() const { return Vec2((left + right) * 0.5f, (top + bottom) * 0.5f); }
};

enum class EntityTeam : int {
    kUnknown = 0,
    kAlly = 1,
    kEnemy = 2,
    kNeutral = 3,
};

enum class EntityKind : int {
    kHero = 0,
    kMinion = 1,
    kJungleMonster = 2,
    kTower = 3,
    kLord = 4,
    kTurtle = 5,
    kSummoned = 6,
    kUnknown = 7,
};

enum class EspRenderMode : int {
    kScreenOverlay = 0,
    kGlHook = 1,
    kSurfaceFlinger = 2,
    kDisabled = 3,
};

struct AbilityInfo {
    int slot = 0;              // 0..3 basic skills, 4 ultimate
    float cooldown = 0.0f;     // seconds remaining
    float maxCooldown = 0.0f;
    float range = 0.0f;
    float manaCost = 0.0f;
    bool isReady() const { return cooldown <= 0.0f; }
    float readyRatio() const {
        if (maxCooldown <= 0.0f) return 1.0f;
        return 1.0f - cooldown / maxCooldown;
    }
};

struct PlayerSnapshot {
    uint32_t id = 0;
    std::string name;
    std::string heroName;
    EntityTeam team = EntityTeam::kUnknown;
    EntityKind kind = EntityKind::kUnknown;
    Vec3 position;
    Vec3 velocity;
    float health = 0.0f;
    float maxHealth = 0.0f;
    float mana = 0.0f;
    float maxMana = 0.0f;
    float attackRange = 0.0f;
    float movementSpeed = 0.0f;
    float armor = 0.0f;
    float magicResist = 0.0f;
    float level = 0.0f;
    bool alive = true;
    bool visible = false;
    bool inFog = true;
    uint32_t kills = 0;
    uint32_t deaths = 0;
    uint32_t assists = 0;
    uint64_t lastSeenMs = 0;
    AbilityInfo abilities[5];

    float healthRatio() const {
        if (maxHealth <= 0.0f) return 0.0f;
        return health / maxHealth;
    }
};

struct ObjectiveSnapshot {
    EntityKind kind = EntityKind::kUnknown;
    std::string name;
    Vec3 position;
    float health = 0.0f;
    float maxHealth = 0.0f;
    uint64_t respawnAtMs = 0;
    bool alive = true;
};

// Projection result used by the renderer.
struct ScreenProjection {
    bool valid = false;
    Vec2 screenPos;
    float depth = 0.0f;
};

// Renderer draw primitives (kept host-agnostic; the adapter converts them).
struct DrawLine { Vec2 a; Vec2 b; uint32_t color; float thickness; };
struct DrawRect { Rect2D rect; uint32_t color; float thickness; bool filled; };
struct DrawCircle { Vec2 center; float radius; uint32_t color; float thickness; bool filled; };
struct DrawText { Vec2 pos; std::string text; uint32_t color; float size; };
struct DrawBar {
    Rect2D rect;
    float ratio = 1.0f;
    uint32_t color;
    uint32_t bgColor;
};

struct RenderFrame {
    std::vector<DrawLine> lines;
    std::vector<DrawRect> rects;
    std::vector<DrawCircle> circles;
    std::vector<DrawText> texts;
    std::vector<DrawBar> bars;
    int width = 0;
    int height = 0;
    uint64_t frameMs = 0;
};

}  // namespace arift