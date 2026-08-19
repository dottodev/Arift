#include "minimap_override.h"

#include "arift_log.h"
#include "arift_utils.h"

namespace arift {

MinimapOverride::MinimapOverride() {
    params_.width = 160;
    params_.height = 160;
    params_.worldScale = 0.02f;
    params_.bounds.min = Vec2(0.0f, 0.0f);
    params_.bounds.max = Vec2(8000.0f, 8000.0f);
}

void MinimapOverride::updateEntities(const std::vector<PlayerSnapshot>& players,
                                     const std::vector<ObjectiveSnapshot>& objectives) {
    dots_.clear();
    if (!enabled_) return;

    for (const auto& p : players) {
        if (!p.alive) continue;
        addDot(p);
    }
    for (const auto& o : objectives) {
        if (!o.alive) continue;
        addObjective(o);
    }
}

Vec2 MinimapOverride::worldToMinimap(const Vec2& world) const {
    const MapBounds& b = params_.bounds;
    float nx = (world.x - b.min.x) / b.width();
    float ny = (world.y - b.min.y) / b.height();
    float px = params_.offset.x + nx * static_cast<float>(params_.width);
    float py = params_.offset.y + ny * static_cast<float>(params_.height);
    px = px < params_.offset.x ? params_.offset.x : px;
    py = py < params_.offset.y ? params_.offset.y : py;
    px = px > params_.offset.x + params_.width ? params_.offset.x + params_.width : px;
    py = py > params_.offset.y + params_.height ? params_.offset.y + params_.height : py;
    return Vec2(px, py);
}

Vec2 MinimapOverride::minimapToWorld(const Vec2& pixel) const {
    const MapBounds& b = params_.bounds;
    float nx = (pixel.x - params_.offset.x) / static_cast<float>(params_.width);
    float ny = (pixel.y - params_.offset.y) / static_cast<float>(params_.height);
    return Vec2(b.min.x + nx * b.width(), b.min.y + ny * b.height());
}

void MinimapOverride::addDot(const PlayerSnapshot& p) {
    MinimapDot dot;
    dot.pixel = worldToMinimap(Vec2(p.position.x, p.position.z));
    dot.team = p.team;
    dot.kind = p.kind;
    dot.objective = false;
    dot.lowHp = p.healthRatio() < 0.25f;
    dots_.push_back(dot);
}

void MinimapOverride::addObjective(const ObjectiveSnapshot& o) {
    MinimapDot dot;
    dot.pixel = worldToMinimap(Vec2(o.position.x, o.position.z));
    dot.team = EntityTeam::kNeutral;
    dot.kind = o.kind;
    dot.objective = true;
    dot.lowHp = o.maxHealth > 0.0f && (o.health / o.maxHealth) < 0.25f;
    dots_.push_back(dot);
}

void MinimapOverride::setVisionCircles(
    const std::vector<std::pair<Vec2, float>>& circles) {
    vision_circles_ = circles;
}

std::string MinimapOverride::diag() const {
    std::string out;
    out += "minimap_override: enabled=" + std::string(enabled_ ? "yes" : "no") + "\n";
    out += "  size=" + std::to_string(params_.width) + "x" +
           std::to_string(params_.height) + "\n";
    out += "  dots=" + std::to_string(dots_.size()) + "\n";
    out += "  vision_circles=" + std::to_string(vision_circles_.size()) + "\n";
    return out;
}

}  // namespace arift