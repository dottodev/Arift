#include "esp_entities.h"

#include <cmath>
#include <cstring>

#include "arift_log.h"
#include "arift_utils.h"
#include "memory_scanner.h"

namespace arift {

EspEntityReader::EspEntityReader() {
    // Default offset template (placeholder; real values come from RE).
    offsets_.entityListPtr = 0;
    offsets_.entityCountOffset = 0x18;
    offsets_.entityPtrArrayOffset = 0x20;
    offsets_.idOffset = 0x04;
    offsets_.namePtrOffset = 0x10;
    offsets_.heroNamePtrOffset = 0x18;
    offsets_.teamOffset = 0x2C;
    offsets_.kindOffset = 0x30;
    offsets_.posOffset = 0x40;
    offsets_.velOffset = 0x4C;
    offsets_.healthOffset = 0x60;
    offsets_.maxHealthOffset = 0x64;
    offsets_.manaOffset = 0x68;
    offsets_.maxManaOffset = 0x6C;
    offsets_.attackRangeOffset = 0x70;
    offsets_.moveSpeedOffset = 0x74;
    offsets_.armorOffset = 0x78;
    offsets_.magicResistOffset = 0x7C;
    offsets_.levelOffset = 0x80;
    offsets_.aliveOffset = 0x84;
    offsets_.visibleOffset = 0x88;
    offsets_.killsOffset = 0x8C;
    offsets_.deathsOffset = 0x90;
    offsets_.assistsOffset = 0x94;
    offsets_.abilityArrayOffset = 0x98;
    offsets_.abilityCooldownStride = 0x20;
    offsets_.abilityCooldownOffset = 0x08;
    offsets_.abilityMaxCooldownOffset = 0x0C;
    offsets_.respawnOffset = 0xA0;
}

bool EspEntityReader::attach(int pid) {
    pid_ = pid;
    players_.clear();
    objectives_.clear();
    read_failures_ = 0;
    ARIFT_INFO(kTagEsp, "Entity reader attached to pid=%d", pid);
    return pid > 0;
}

size_t EspEntityReader::refresh() {
    if (simulation_) {
        simulateEntities();
        return players_.size();
    }
    if (pid_ <= 0) return 0;

    players_.clear();
    objectives_.clear();
    last_refresh_ms_ = utils::monotonicMs();

    ProcessMemory mem;
    if (!mem.open(pid_)) {
        ++read_failures_;
        return 0;
    }

    // Resolve the entity list head.
    if (offsets_.entityListPtr == 0) {
        // Fallback: search a known pattern is done elsewhere; here we just
        // keep an empty list and rely on the simulation flag.
        return 0;
    }

    uint32_t count = 0;
    if (!mem.read32(offsets_.entityListPtr + offsets_.entityCountOffset, count)) {
        ++read_failures_;
        return 0;
    }
    if (count > static_cast<uint32_t>(4096)) count = 4096;

    uint64_t arrayPtr = 0;
    if (!mem.read64(offsets_.entityListPtr + offsets_.entityPtrArrayOffset, arrayPtr)) {
        ++read_failures_;
        return 0;
    }

    for (uint32_t i = 0; i < count; ++i) {
        uint64_t entityPtr = 0;
        if (!mem.read64(arrayPtr + i * 8, entityPtr)) {
            ++read_failures_;
            continue;
        }
        if (entityPtr == 0) continue;
        PlayerSnapshot snap;
        readEntity(static_cast<uintptr_t>(entityPtr), snap);
        if (snap.id != 0) {
            players_.push_back(snap);
        }
    }

    ARIFT_TRACE(kTagEsp, "Refresh: %zu players", players_.size());
    return players_.size();
}

void EspEntityReader::readEntity(uintptr_t base, PlayerSnapshot& out) const {
    ProcessMemory mem;
    if (!mem.open(pid_)) return;

    mem.read32(base + offsets_.idOffset, out.id);

    uint64_t namePtr = 0;
    if (mem.read64(base + offsets_.namePtrOffset, namePtr)) {
        out.name = readStringSafe(static_cast<uintptr_t>(namePtr));
    }
    uint64_t heroPtr = 0;
    if (mem.read64(base + offsets_.heroNamePtrOffset, heroPtr)) {
        out.heroName = readStringSafe(static_cast<uintptr_t>(heroPtr));
    }

    uint32_t team = 0;
    if (mem.read32(base + offsets_.teamOffset, team)) {
        out.team = static_cast<EntityTeam>(team);
    }
    uint32_t kind = 0;
    if (mem.read32(base + offsets_.kindOffset, kind)) {
        out.kind = static_cast<EntityKind>(kind);
    }

    mem.readFloat(base + offsets_.posOffset, out.position.x);
    mem.readFloat(base + offsets_.posOffset + 4, out.position.y);
    mem.readFloat(base + offsets_.posOffset + 8, out.position.z);
    mem.readFloat(base + offsets_.velOffset, out.velocity.x);
    mem.readFloat(base + offsets_.velOffset + 4, out.velocity.y);
    mem.readFloat(base + offsets_.velOffset + 8, out.velocity.z);

    mem.readFloat(base + offsets_.healthOffset, out.health);
    mem.readFloat(base + offsets_.maxHealthOffset, out.maxHealth);
    mem.readFloat(base + offsets_.manaOffset, out.mana);
    mem.readFloat(base + offsets_.maxManaOffset, out.maxMana);
    mem.readFloat(base + offsets_.attackRangeOffset, out.attackRange);
    mem.readFloat(base + offsets_.moveSpeedOffset, out.movementSpeed);
    mem.readFloat(base + offsets_.armorOffset, out.armor);
    mem.readFloat(base + offsets_.magicResistOffset, out.magicResist);
    mem.readFloat(base + offsets_.levelOffset, out.level);

    uint32_t flags = 0;
    if (mem.read32(base + offsets_.aliveOffset, flags)) {
        out.alive = (flags & 1) != 0;
    }
    if (mem.read32(base + offsets_.visibleOffset, flags)) {
        out.visible = (flags & 1) != 0;
        out.inFog = !out.visible;
    }

    mem.read32(base + offsets_.killsOffset, out.kills);
    mem.read32(base + offsets_.deathsOffset, out.deaths);
    mem.read32(base + offsets_.assistsOffset, out.assists);

    // Abilities
    for (int s = 0; s < 5; ++s) {
        uintptr_t slotBase = base + offsets_.abilityArrayOffset +
                             static_cast<uintptr_t>(s * offsets_.abilityCooldownStride);
        AbilityInfo& ab = out.abilities[s];
        ab.slot = s;
        mem.readFloat(slotBase + offsets_.abilityCooldownOffset, ab.cooldown);
        mem.readFloat(slotBase + offsets_.abilityMaxCooldownOffset, ab.maxCooldown);
        mem.readFloat(slotBase + 0x10, ab.range);
        mem.readFloat(slotBase + 0x14, ab.manaCost);
    }

    out.lastSeenMs = utils::monotonicMs();
}

bool EspEntityReader::readFloatSafe(uintptr_t addr, float& out) const {
    ProcessMemory mem;
    if (!mem.open(pid_)) return false;
    return mem.readFloat(addr, out);
}

bool EspEntityReader::readU32Safe(uintptr_t addr, uint32_t& out) const {
    ProcessMemory mem;
    if (!mem.open(pid_)) return false;
    return mem.read32(addr, out);
}

std::string EspEntityReader::readStringSafe(uintptr_t addr) const {
    ProcessMemory mem;
    if (!mem.open(pid_)) return "";
    return mem.readCString(addr, 64);
}

const PlayerSnapshot* EspEntityReader::findPlayer(uint32_t id) const {
    for (const auto& p : players_) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

const PlayerSnapshot* EspEntityReader::localPlayer() const {
    for (const auto& p : players_) {
        if (p.team == EntityTeam::kAlly && p.kind == EntityKind::kHero) {
            // First ally hero is our best guess; refined via offsets later.
            return &p;
        }
    }
    return players_.empty() ? nullptr : &players_[0];
}

std::vector<const PlayerSnapshot*> EspEntityReader::enemies() const {
    std::vector<const PlayerSnapshot*> out;
    for (const auto& p : players_) {
        if (p.team == EntityTeam::kEnemy) out.push_back(&p);
    }
    return out;
}

std::vector<const PlayerSnapshot*> EspEntityReader::allies() const {
    std::vector<const PlayerSnapshot*> out;
    for (const auto& p : players_) {
        if (p.team == EntityTeam::kAlly) out.push_back(&p);
    }
    return out;
}

void EspEntityReader::setCamera(const Vec3& pos, const Vec3& forward, const Vec3& up,
                                float fovDeg, int screenW, int screenH) {
    camera_pos_ = pos;
    camera_forward_ = forward;
    camera_up_ = up;
    fov_rad_ = fovDeg * static_cast<float>(M_PI) / 180.0f;
    screen_w_ = screenW > 0 ? screenW : 1080;
    screen_h_ = screenH > 0 ? screenH : 2340;
}

ScreenProjection EspEntityReader::projectToScreen(const Vec3& world) const {
    ScreenProjection proj;
    Vec3 dir = world - camera_pos_;
    Vec3 fwd = camera_forward_;
    float fwdLen = fwd.length();
    if (fwdLen < 1e-6f) return proj;
    fwd = fwd * (1.0f / fwdLen);

    float distAlong = dir.x * fwd.x + dir.y * fwd.y + dir.z * fwd.z;
    if (distAlong <= 0.0f) return proj;

    Vec3 right = Vec3(fwd.z, 0.0f, -fwd.x);
    float rightLen = right.length();
    if (rightLen < 1e-6f) return proj;
    right = right * (1.0f / rightLen);
    Vec3 up = Vec3(right.y * fwd.z - right.z * fwd.y,
                  right.z * fwd.x - right.x * fwd.z,
                  right.x * fwd.y - right.y * fwd.x);

    float xCam = dir.x * right.x + dir.y * right.y + dir.z * right.z;
    float yCam = dir.x * up.x + dir.y * up.y + dir.z * up.z;

    float halfFov = fov_rad_ * 0.5f;
    float tanHalf = tanf(halfFov);
    float focal = static_cast<float>(screen_h_) * 0.5f / tanHalf;

    float sx = static_cast<float>(screen_w_) * 0.5f + (xCam * focal) / distAlong;
    float sy = static_cast<float>(screen_h_) * 0.5f - (yCam * focal) / distAlong;

    proj.valid = true;
    proj.screenPos = Vec2(sx, sy);
    proj.depth = distAlong;
    return proj;
}

void EspEntityReader::useSimulation(bool enabled, size_t entityCount) {
    simulation_ = enabled;
    sim_count_ = entityCount;
    ARIFT_INFO(kTagEsp, "Simulation mode %s (%zu entities)",
               enabled ? "ON" : "OFF", entityCount);
}

void EspEntityReader::simulateEntities() {
    players_.clear();
    objectives_.clear();
    ++sim_clock_;
    uint64_t now = utils::monotonicMs();

    // Local hero
    PlayerSnapshot local;
    local.id = 1;
    local.name = "LocalHero";
    local.heroName = "Lancelot";
    local.team = EntityTeam::kAlly;
    local.kind = EntityKind::kHero;
    local.position = Vec3(1200.0f, 0.0f, 1200.0f);
    local.health = 4200.0f;
    local.maxHealth = 4800.0f;
    local.mana = 300.0f;
    local.maxMana = 500.0f;
    local.level = 12.0f;
    local.alive = true;
    local.visible = true;
    local.attackRange = 180.0f;
    local.movementSpeed = 260.0f;
    local.lastSeenMs = now;
    players_.push_back(local);

    // Enemy heroes: patrol in a circle around the map.
    const char* enemyNames[] = {"Gusion", "Chou", "Layla", "Nana", "Granger"};
    const char* enemyHeros[] = {"Assassin", "Fighter", "Marksman", "Mage", "Marksman"};
    for (size_t i = 0; i < sim_count_; ++i) {
        PlayerSnapshot p;
        p.id = static_cast<uint32_t>(100 + i);
        p.name = enemyNames[i % 5];
        p.heroName = enemyHeros[i % 5];
        p.team = EntityTeam::kEnemy;
        p.kind = EntityKind::kHero;
        float angle = static_cast<float>(sim_clock_) * 0.001f +
                      static_cast<float>(i) * 1.2566f;
        p.position = Vec3(3500.0f + 900.0f * cosf(angle), 0.0f,
                          3500.0f + 900.0f * sinf(angle));
        p.velocity = Vec3(-sinf(angle) * 260.0f, 0.0f, cosf(angle) * 260.0f);
        p.health = 1800.0f + static_cast<float>(i) * 350.0f;
        p.maxHealth = 3000.0f + static_cast<float>(i) * 400.0f;
        p.mana = 250.0f;
        p.maxMana = 480.0f;
        p.attackRange = 170.0f;
        p.movementSpeed = 260.0f;
        p.armor = 60.0f + static_cast<float>(i) * 5.0f;
        p.magicResist = 40.0f + static_cast<float>(i) * 6.0f;
        p.level = 10.0f + static_cast<float>(i % 4);
        p.alive = true;
        p.visible = true;
        p.inFog = false;
        p.kills = static_cast<uint32_t>(i * 2);
        p.deaths = static_cast<uint32_t>(i);
        p.assists = static_cast<uint32_t>(i * 3);
        for (int s = 0; s < 5; ++s) {
            p.abilities[s].slot = s;
            p.abilities[s].maxCooldown = 6.0f + static_cast<float>(s) * 5.0f;
            p.abilities[s].cooldown =
                static_cast<float>((sim_clock_ + i * 7) % 20) / 20.0f *
                p.abilities[s].maxCooldown;
            p.abilities[s].range = 250.0f + static_cast<float>(s) * 100.0f;
        }
        p.lastSeenMs = now;
        players_.push_back(p);
    }

    // Objective: Lord + Turtle
    ObjectiveSnapshot lord;
    lord.kind = EntityKind::kLord;
    lord.name = "Lord";
    lord.position = Vec3(5000.0f, 0.0f, 5000.0f);
    lord.health = 9000.0f;
    lord.maxHealth = 12000.0f;
    lord.alive = true;
    objectives_.push_back(lord);

    ObjectiveSnapshot turtle;
    turtle.kind = EntityKind::kTurtle;
    turtle.name = "Turtle";
    turtle.position = Vec3(2500.0f, 0.0f, 2500.0f);
    turtle.health = 6000.0f;
    turtle.maxHealth = 8000.0f;
    turtle.alive = true;
    turtle.respawnAtMs = now + 180000;
    objectives_.push_back(turtle);
}

}  // namespace arift