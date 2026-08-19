#include "esp_renderer.h"

#include <cmath>
#include <cstdio>

#include "arift_log.h"
#include "arift_utils.h"

namespace arift {

namespace {

constexpr uint32_t kAlpha(uint32_t argb) { return (argb >> 24) & 0xFF; }
constexpr uint32_t kRed(uint32_t argb) { return (argb >> 16) & 0xFF; }
constexpr uint32_t kGreen(uint32_t argb) { return (argb >> 8) & 0xFF; }
constexpr uint32_t kBlue(uint32_t argb) { return argb & 0xFF; }

uint32_t mixColor(uint32_t a, uint32_t b, float t) {
    uint32_t r = static_cast<uint32_t>(kRed(a) * (1.0f - t) + kRed(b) * t);
    uint32_t g = static_cast<uint32_t>(kGreen(a) * (1.0f - t) + kGreen(b) * t);
    uint32_t bl = static_cast<uint32_t>(kBlue(a) * (1.0f - t) + kBlue(b) * t);
    uint32_t al = static_cast<uint32_t>(kAlpha(a) * (1.0f - t) + kAlpha(b) * t);
    return (al << 24) | (r << 16) | (g << 8) | bl;
}

std::string fmtText(const char* fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    return std::string(buf);
}

}  // namespace

EspRenderer::EspRenderer(const EspEntityReader* reader) : reader_(reader) {}

void EspRenderer::setScreen(int width, int height) {
    width_ = width > 0 ? width : 1080;
    height_ = height > 0 ? height : 2340;
}

RenderFrame EspRenderer::render() {
    RenderFrame frame;
    frame.width = width_;
    frame.height = height_;
    frame.frameMs = utils::monotonicMs();

    if (!reader_) return frame;
    const EspSettings& cfg = EspConfig::instance().settings();
    if (!cfg.enabled) return frame;

    const auto& players = reader_->players();
    for (const auto& p : players) {
        if (!p.alive) continue;
        if (p.team == EntityTeam::kAlly && p.kind == EntityKind::kHero) continue;

        ScreenProjection proj = reader_->projectToScreen(p.position);
        if (!proj.valid) continue;

        if (cfg.drawBoxes) drawPlayerBox(frame, p, proj);
        if (cfg.drawHealthBars) drawHealthBar(frame, p, proj);
        if (cfg.drawNames) drawNameTag(frame, p, proj);
        if (cfg.drawCooldowns) drawCooldowns(frame, p, proj);
        if (cfg.drawDistance) drawDistanceTag(frame, p, proj);
        if (cfg.drawKda) drawKdaTag(frame, p, proj);
        if (cfg.drawLowHpAlert) drawLowHpAlert(frame, p, proj);
        if (cfg.drawLines) {
            DrawLine line;
            line.a = Vec2(static_cast<float>(width_) * 0.5f,
                          static_cast<float>(height_) * 0.5f);
            line.b = proj.screenPos;
            line.color = colorFor(p);
            line.thickness = 1.5f;
            frame.lines.push_back(line);
        }
    }

    if (cfg.drawObjectives) {
        const auto& objs = reader_->objectives();
        for (const auto& o : objs) {
            if (!o.alive) continue;
            ScreenProjection proj = reader_->projectToScreen(o.position);
            if (proj.valid) drawObjectives(frame, o, proj);
        }
    }

    ++frames_;
    primitives_last_ = frame.lines.size() + frame.rects.size() +
                       frame.circles.size() + frame.texts.size() +
                       frame.bars.size();
    return frame;
}

Rect2D EspRenderer::estimateBox(const PlayerSnapshot& p,
                                const ScreenProjection& proj) const {
    Rect2D box;
    float depth = proj.depth > 1.0f ? proj.depth : 1.0f;
    float baseHeight = 260.0f;   // ~screen px at 1 unit distance
    float h = baseHeight * (1.0f / depth) * 4.0f;
    float w = h * 0.55f;
    if (h < 8.0f) h = 8.0f;
    if (w < 5.0f) w = 5.0f;
    box.left = proj.screenPos.x - w * 0.5f;
    box.right = proj.screenPos.x + w * 0.5f;
    box.top = proj.screenPos.y - h;
    box.bottom = proj.screenPos.y;
    return box;
}

float EspRenderer::projectHeight(const ScreenProjection& proj) const {
    Rect2D box = estimateBox(PlayerSnapshot(), proj);
    return box.height();
}

void EspRenderer::drawPlayerBox(RenderFrame& frame, const PlayerSnapshot& p,
                                const ScreenProjection& proj) {
    Rect2D box = estimateBox(p, proj);
    const EspSettings& cfg = EspConfig::instance().settings();
    box.left -= cfg.boxPadding;
    box.right += cfg.boxPadding;
    box.top -= cfg.boxPadding;
    box.bottom += cfg.boxPadding;

    DrawRect r;
    r.rect = box;
    r.color = colorFor(p);
    r.thickness = 1.6f;
    r.filled = false;
    frame.rects.push_back(r);
}

void EspRenderer::drawHealthBar(RenderFrame& frame, const PlayerSnapshot& p,
                                const ScreenProjection& proj) {
    const EspSettings& cfg = EspConfig::instance().settings();
    Rect2D box = estimateBox(p, proj);

    DrawBar bar;
    bar.rect.left = box.left;
    bar.rect.right = box.right;
    bar.rect.top = box.top - cfg.healthBarHeight - 2.0f;
    bar.rect.bottom = bar.rect.top + cfg.healthBarHeight;
    bar.ratio = p.healthRatio();
    bar.color = healthColorFor(p);
    bar.bgColor = 0x99000000;
    frame.bars.push_back(bar);

    if (cfg.drawManaBar && p.maxMana > 0.0f) {
        DrawBar mana;
        mana.rect.left = bar.rect.left;
        mana.rect.right = bar.rect.right;
        mana.rect.top = bar.rect.top - cfg.healthBarHeight - 2.0f;
        mana.rect.bottom = mana.rect.top + cfg.healthBarHeight * 0.6f;
        mana.ratio = p.maxMana > 0.0f ? p.mana / p.maxMana : 0.0f;
        mana.color = cfg.manaColor;
        mana.bgColor = 0x99000000;
        frame.bars.push_back(mana);
    }
}

void EspRenderer::drawNameTag(RenderFrame& frame, const PlayerSnapshot& p,
                              const ScreenProjection& proj) {
    const EspSettings& cfg = EspConfig::instance().settings();
    Rect2D box = estimateBox(p, proj);

    std::string label;
    if (cfg.drawHeroLevel) {
        label = fmtText("%s (Lv%.0f)", p.heroName.c_str(), p.level);
    } else {
        label = p.heroName;
    }

    DrawText t;
    t.pos = Vec2(box.left, box.bottom + 6.0f);
    t.text = label;
    t.color = cfg.textColor;
    t.size = cfg.textSize;
    frame.texts.push_back(t);

    DrawText name;
    name.pos = Vec2(box.left, box.bottom + 6.0f + cfg.textSize + 2.0f);
    name.text = p.name;
    name.color = mixColor(cfg.textColor, colorFor(p), 0.5f);
    name.size = cfg.textSize - 2.0f;
    frame.texts.push_back(name);
}

void EspRenderer::drawCooldowns(RenderFrame& frame, const PlayerSnapshot& p,
                                const ScreenProjection& proj) {
    const EspSettings& cfg = EspConfig::instance().settings();
    Rect2D box = estimateBox(p, proj);

    float x = box.left;
    float y = box.bottom + 6.0f;
    if (cfg.drawNames) y += cfg.textSize * 2.0f + 4.0f;

    float slotW = (box.width()) / 5.0f;
    if (slotW < 8.0f) slotW = 8.0f;
    float slotH = 5.0f;

    for (int s = 0; s < 5; ++s) {
        const AbilityInfo& ab = p.abilities[s];
        DrawBar cd;
        cd.rect.left = x + static_cast<float>(s) * slotW;
        cd.rect.right = cd.rect.left + slotW - 1.0f;
        cd.rect.top = y;
        cd.rect.bottom = y + slotH;
        cd.ratio = ab.readyRatio();
        cd.color = ab.isReady() ? cfg.healthColor : mixColor(0xFFFFFFFF, 0xFFFF0000, 0.4f);
        cd.bgColor = 0x66000000;
        frame.bars.push_back(cd);

        if (!ab.isReady()) {
            DrawText t;
            t.pos = Vec2(cd.rect.left, y + slotH + 9.0f);
            t.text = fmtText("%.0f", ab.cooldown);
            t.color = 0xFFFFA500;
            t.size = 9.0f;
            frame.texts.push_back(t);
        }
    }
}

void EspRenderer::drawDistanceTag(RenderFrame& frame, const PlayerSnapshot& p,
                                  const ScreenProjection& proj) {
    float dist = p.position.length() / 100.0f;  // game units -> meters-ish
    DrawText t;
    Rect2D box = estimateBox(p, proj);
    t.pos = Vec2(box.right + 4.0f, proj.screenPos.y - 10.0f);
    t.text = fmtText("%.0fm", dist);
    t.color = 0xFFBBBBBB;
    t.size = 10.0f;
    frame.texts.push_back(t);
}

void EspRenderer::drawKdaTag(RenderFrame& frame, const PlayerSnapshot& p,
                             const ScreenProjection& proj) {
    DrawText t;
    Rect2D box = estimateBox(p, proj);
    t.pos = Vec2(box.left, box.top - 16.0f);
    t.text = fmtText("%u/%u/%u", p.kills, p.deaths, p.assists);
    t.color = 0xFFCCAAFF;
    t.size = 10.0f;
    frame.texts.push_back(t);
}

void EspRenderer::drawLowHpAlert(RenderFrame& frame, const PlayerSnapshot& p,
                                 const ScreenProjection& proj) {
    const EspSettings& cfg = EspConfig::instance().settings();
    if (p.healthRatio() > cfg.lowHpThreshold) return;
    if (p.maxHealth <= 0.0f) return;

    Rect2D box = estimateBox(p, proj);
    DrawCircle c;
    c.center = box.center();
    c.radius = box.width() * 0.9f;
    c.color = cfg.lowHpColor;
    c.thickness = 2.0f;
    c.filled = false;
    frame.circles.push_back(c);
}

void EspRenderer::drawObjectives(RenderFrame& frame, const ObjectiveSnapshot& obj,
                                 const ScreenProjection& proj) {
    const EspSettings& cfg = EspConfig::instance().settings();
    Rect2D box;
    box.left = proj.screenPos.x - 30.0f;
    box.right = proj.screenPos.x + 30.0f;
    box.top = proj.screenPos.y - 30.0f;
    box.bottom = proj.screenPos.y + 30.0f;

    DrawCircle c;
    c.center = proj.screenPos;
    c.radius = 26.0f;
    c.color = obj.kind == EntityKind::kLord ? 0xFFFFD200 : 0xFF00E5FF;
    c.thickness = 2.0f;
    c.filled = false;
    frame.circles.push_back(c);

    DrawText t;
    t.pos = Vec2(proj.screenPos.x - 20.0f, proj.screenPos.y + 44.0f);
    t.text = obj.name;
    t.color = 0xFFFFFFFF;
    t.size = cfg.objectiveTextSize;
    frame.texts.push_back(t);

    DrawText hp;
    hp.pos = Vec2(proj.screenPos.x - 20.0f, proj.screenPos.y + 62.0f);
    hp.text = fmtText("%.0f%%", obj.maxHealth > 0.0f ? obj.health / obj.maxHealth * 100.0f : 0.0f);
    hp.color = healthColorFor(PlayerSnapshot());
    hp.size = cfg.objectiveTextSize - 2.0f;
    frame.texts.push_back(hp);
}

uint32_t EspRenderer::colorFor(const PlayerSnapshot& p) const {
    const EspSettings& cfg = EspConfig::instance().settings();
    switch (p.team) {
        case EntityTeam::kAlly: return cfg.allyBoxColor;
        case EntityTeam::kEnemy: return cfg.enemyBoxColor;
        case EntityTeam::kNeutral: return cfg.neutralBoxColor;
        default: return cfg.neutralBoxColor;
    }
}

uint32_t EspRenderer::healthColorFor(const PlayerSnapshot& p) const {
    const EspSettings& cfg = EspConfig::instance().settings();
    float ratio = p.healthRatio();
    if (ratio <= cfg.lowHpThreshold) return cfg.lowHpColor;
    if (ratio <= 0.5f) return mixColor(cfg.lowHpColor, 0xFFFFA500, ratio * 2.0f);
    return mixColor(0xFFFFA500, cfg.healthColor, (ratio - 0.5f) * 2.0f);
}

}  // namespace arift