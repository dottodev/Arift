#include "fog_bypass.h"

#include <cmath>
#include <cstring>

#include "arift_log.h"
#include "arift_utils.h"
#include "memory_scanner.h"

namespace arift {

// ---------------------------------------------------------------------------
// VisionGrid
// ---------------------------------------------------------------------------

void VisionGrid::resize(int x, int y, float size, const Vec2& origin) {
    cellsX = x;
    cellsY = y;
    cellSize = size;
    this->origin = origin;
    cells.assign(static_cast<size_t>(x * y), VisionCell{});
}

VisionCell& VisionGrid::cellAt(const Vec2& world) {
    static VisionCell fallback;
    int cx = static_cast<int>((world.x - origin.x) / cellSize);
    int cy = static_cast<int>((world.y - origin.y) / cellSize);
    if (cx < 0 || cy < 0 || cx >= cellsX || cy >= cellsY) {
        fallback = VisionCell{};
        return fallback;
    }
    return cells[static_cast<size_t>(cy * cellsX + cx)];
}

const VisionCell& VisionGrid::cellAt(const Vec2& world) const {
    static const VisionCell fallback{};
    int cx = static_cast<int>((world.x - origin.x) / cellSize);
    int cy = static_cast<int>((world.y - origin.y) / cellSize);
    if (cx < 0 || cy < 0 || cx >= cellsX || cy >= cellsY) return fallback;
    return cells[static_cast<size_t>(cy * cellsX + cx)];
}

void VisionGrid::clear() {
    for (auto& c : cells) c = VisionCell{};
}

void VisionGrid::revealAll() {
    int64_t now = utils::monotonicMs();
    for (auto& c : cells) {
        c.explored = true;
        c.visible = true;
        c.lastSeenMs = now;
    }
}

void VisionGrid::setVisible(const Vec2& world, bool visible, bool explored) {
    VisionCell& c = cellAt(world);
    if (visible) c.lastSeenMs = utils::monotonicMs();
    c.visible = visible;
    c.explored = explored || c.explored;
}

void VisionGrid::decay(int64_t nowMs, int64_t keepVisibleMs) {
    for (auto& c : cells) {
        if (c.visible && nowMs - c.lastSeenMs > keepVisibleMs) {
            c.visible = false;
        }
    }
}

bool VisionGrid::worldVisible(const Vec2& world) const {
    return cellAt(world).visible;
}

// ---------------------------------------------------------------------------
// FogBypass
// ---------------------------------------------------------------------------

FogBypass::FogBypass() {
    grid_.resize(64, 64, 100.0f, Vec2(0.0f, 0.0f));
}

void FogBypass::setTarget(int pid) {
    pid_ = pid;
}

size_t FogBypass::update(const std::vector<VisionSource>& sources, int64_t nowMs) {
    if (!enabled_) return 0;
    ++frames_;

    if (raw_) {
        grid_.revealAll();
        return grid_.cells.size();
    }

    size_t changed = recomputeFromSources(sources, nowMs);
    grid_.decay(nowMs, static_cast<int64_t>(1500));
    return changed;
}

void FogBypass::forceRevealAll() {
    raw_ = true;
    grid_.revealAll();
}

size_t FogBypass::recomputeFromSources(const std::vector<VisionSource>& sources,
                                       int64_t nowMs) {
    size_t changed = 0;
    for (const auto& src : sources) {
        if (!src.active) continue;
        float r2 = src.radius * src.radius;

        int x0 = static_cast<int>((src.position.x - src.radius - grid_.origin.x) / grid_.cellSize);
        int x1 = static_cast<int>((src.position.x + src.radius - grid_.origin.x) / grid_.cellSize);
        int y0 = static_cast<int>((src.position.y - src.radius - grid_.origin.y) / grid_.cellSize);
        int y1 = static_cast<int>((src.position.y + src.radius - grid_.origin.y) / grid_.cellSize);

        x0 = x0 < 0 ? 0 : x0;
        y0 = y0 < 0 ? 0 : y0;
        x1 = x1 >= grid_.cellsX ? grid_.cellsX - 1 : x1;
        y1 = y1 >= grid_.cellsY ? grid_.cellsY - 1 : y1;

        for (int cy = y0; cy <= y1; ++cy) {
            for (int cx = x0; cx <= x1; ++cx) {
                float wx = grid_.origin.x + (static_cast<float>(cx) + 0.5f) * grid_.cellSize;
                float wy = grid_.origin.y + (static_cast<float>(cy) + 0.5f) * grid_.cellSize;
                float dx = wx - src.position.x;
                float dy = wy - src.position.y;
                if (dx * dx + dy * dy > r2) continue;

                VisionCell& cell = grid_.cells[static_cast<size_t>(cy * grid_.cellsX + cx)];
                if (!cell.visible || !cell.explored) ++changed;
                cell.visible = true;
                cell.explored = true;
                cell.lastSeenMs = nowMs;
                if (src.team == 1) cell.allyMask = 1;
                if (src.team == 2) cell.enemyMask = 1;
            }
        }
    }
    return changed;
}

bool FogBypass::onFogCullBegin(uintptr_t enginePtr) {
    if (!enabled_ || !pid_ || enginePtr == 0) return false;
    // Hook site: the engine calls this right before applying fog culling.
    // We override the visibility flag in the engine's vision buffer so the
    // culler skips every entity.
    ProcessMemory mem;
    if (!mem.open(pid_)) return false;

    // Write visibility = visible to the engine's vision state (offset
    // template — real values come from RE). If offsets are 0, this is a
    // no-op and the recompute path covers the overlay.
    uint64_t visionFlag = 0;
    if (mem.read64(enginePtr, visionFlag)) {
        (void)visionFlag;
        // In real deployment: mem.write32(enginePtr + kVisionOffset, 1);
    }
    return true;
}

bool FogBypass::onFogCullEnd(uintptr_t enginePtr) {
    (void)enginePtr;
    if (!enabled_) return false;
    // Restore engine state after culling if we modified it.
    return true;
}

uint64_t FogBypass::cellsVisible() const {
    uint64_t n = 0;
    for (const auto& c : grid_.cells) {
        if (c.visible) ++n;
    }
    return n;
}

std::string FogBypass::diag() const {
    std::string out;
    out += "fog_bypass: enabled=" + std::string(enabled_ ? "yes" : "no") + "\n";
    out += "  grid=" + std::to_string(grid_.cellsX) + "x" +
           std::to_string(grid_.cellsY) + "\n";
    out += "  cells_visible=" + std::to_string(cellsVisible()) + "/" +
           std::to_string(grid_.cells.size()) + "\n";
    out += "  frames=" + std::to_string(frames_) + "\n";
    out += "  raw=" + std::string(raw_ ? "yes" : "no") + "\n";
    return out;
}

}  // namespace arift