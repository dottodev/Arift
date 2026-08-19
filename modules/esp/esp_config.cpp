#include "esp_config.h"

#include "arift_config.h"
#include "arift_log.h"

namespace arift {

void EspSettings::loadFromConfig() {
    Config& cfg = Config::instance();

    enabled = cfg.getBool("esp", "enabled", enabled);
    drawBoxes = cfg.getBool("esp", "draw_boxes", drawBoxes);
    drawHealthBars = cfg.getBool("esp", "draw_health", drawHealthBars);
    drawNames = cfg.getBool("esp", "draw_names", drawNames);
    drawCooldowns = cfg.getBool("esp", "draw_cooldowns", drawCooldowns);
    drawObjectives = cfg.getBool("esp", "draw_objectives", drawObjectives);
    drawDistance = cfg.getBool("esp", "draw_distance", drawDistance);
    drawLines = cfg.getBool("esp", "draw_lines", drawLines);
    drawCircleEnemies = cfg.getBool("esp", "draw_circle_enemies", drawCircleEnemies);
    drawItems = cfg.getBool("esp", "draw_items", drawItems);
    drawHeroLevel = cfg.getBool("esp", "draw_hero_level", drawHeroLevel);
    drawKda = cfg.getBool("esp", "draw_kda", drawKda);
    drawManaBar = cfg.getBool("esp", "draw_mana_bar", drawManaBar);
    drawLowHpAlert = cfg.getBool("esp", "draw_low_hp_alert", drawLowHpAlert);

    renderMode = static_cast<EspRenderMode>(
        cfg.getInt("esp", "render_mode", static_cast<int>(renderMode)));
    maxEntities = cfg.getInt("esp", "max_entities", maxEntities);
    maxRenderFps = cfg.getInt("esp", "max_render_fps", maxRenderFps);

    enemyBoxColor = static_cast<uint32_t>(
        cfg.getInt("esp", "enemy_box_color", static_cast<int>(enemyBoxColor)));
    allyBoxColor = static_cast<uint32_t>(
        cfg.getInt("esp", "ally_box_color", static_cast<int>(allyBoxColor)));
    neutralBoxColor = static_cast<uint32_t>(
        cfg.getInt("esp", "neutral_box_color", static_cast<int>(neutralBoxColor)));
    lowHpColor = static_cast<uint32_t>(
        cfg.getInt("esp", "low_hp_color", static_cast<int>(lowHpColor)));
    textColor = static_cast<uint32_t>(
        cfg.getInt("esp", "text_color", static_cast<int>(textColor)));
    healthColor = static_cast<uint32_t>(
        cfg.getInt("esp", "health_color", static_cast<int>(healthColor)));
    manaColor = static_cast<uint32_t>(
        cfg.getInt("esp", "mana_color", static_cast<int>(manaColor)));

    lowHpThreshold = cfg.getFloat("esp", "low_hp_threshold", lowHpThreshold);
    boxPadding = cfg.getFloat("esp", "box_padding", boxPadding);
    textSize = cfg.getFloat("esp", "text_size", textSize);
    healthBarWidth = cfg.getFloat("esp", "health_bar_width", healthBarWidth);
    healthBarHeight = cfg.getFloat("esp", "health_bar_height", healthBarHeight);
    objectiveTextSize = cfg.getFloat("esp", "objective_text_size", objectiveTextSize);

    visionScanMs = cfg.getInt("esp", "vision_scan_ms", visionScanMs);
    cooldownScanMs = cfg.getInt("esp", "cooldown_scan_ms", cooldownScanMs);

    trackLord = cfg.getBool("esp", "track_lord", trackLord);
    trackTurtle = cfg.getBool("esp", "track_turtle", trackTurtle);
    trackJungleTimers = cfg.getBool("esp", "track_jungle_timers", trackJungleTimers);

    fovDegrees = cfg.getFloat("esp", "fov_degrees", fovDegrees);
    nearPlane = cfg.getFloat("esp", "near_plane", nearPlane);
    farPlane = cfg.getFloat("esp", "far_plane", farPlane);
}

void EspSettings::saveToConfig() const {
    Config& cfg = Config::instance();

    cfg.setBool("esp", "enabled", enabled);
    cfg.setBool("esp", "draw_boxes", drawBoxes);
    cfg.setBool("esp", "draw_health", drawHealthBars);
    cfg.setBool("esp", "draw_names", drawNames);
    cfg.setBool("esp", "draw_cooldowns", drawCooldowns);
    cfg.setBool("esp", "draw_objectives", drawObjectives);
    cfg.setBool("esp", "draw_distance", drawDistance);
    cfg.setBool("esp", "draw_lines", drawLines);
    cfg.setBool("esp", "draw_circle_enemies", drawCircleEnemies);
    cfg.setBool("esp", "draw_items", drawItems);
    cfg.setBool("esp", "draw_hero_level", drawHeroLevel);
    cfg.setBool("esp", "draw_kda", drawKda);
    cfg.setBool("esp", "draw_mana_bar", drawManaBar);
    cfg.setBool("esp", "draw_low_hp_alert", drawLowHpAlert);

    cfg.setInt("esp", "render_mode", static_cast<int>(renderMode));
    cfg.setInt("esp", "max_entities", maxEntities);
    cfg.setInt("esp", "max_render_fps", maxRenderFps);

    cfg.setInt("esp", "enemy_box_color", static_cast<int>(enemyBoxColor));
    cfg.setInt("esp", "ally_box_color", static_cast<int>(allyBoxColor));
    cfg.setInt("esp", "neutral_box_color", static_cast<int>(neutralBoxColor));
    cfg.setInt("esp", "low_hp_color", static_cast<int>(lowHpColor));
    cfg.setInt("esp", "text_color", static_cast<int>(textColor));
    cfg.setInt("esp", "health_color", static_cast<int>(healthColor));
    cfg.setInt("esp", "mana_color", static_cast<int>(manaColor));

    cfg.setFloat("esp", "low_hp_threshold", lowHpThreshold);
    cfg.setFloat("esp", "box_padding", boxPadding);
    cfg.setFloat("esp", "text_size", textSize);
    cfg.setFloat("esp", "health_bar_width", healthBarWidth);
    cfg.setFloat("esp", "health_bar_height", healthBarHeight);
    cfg.setFloat("esp", "objective_text_size", objectiveTextSize);

    cfg.setInt("esp", "vision_scan_ms", visionScanMs);
    cfg.setInt("esp", "cooldown_scan_ms", cooldownScanMs);

    cfg.setBool("esp", "track_lord", trackLord);
    cfg.setBool("esp", "track_turtle", trackTurtle);
    cfg.setBool("esp", "track_jungle_timers", trackJungleTimers);

    cfg.setFloat("esp", "fov_degrees", fovDegrees);
    cfg.setFloat("esp", "near_plane", nearPlane);
    cfg.setFloat("esp", "far_plane", farPlane);
}

EspConfig& EspConfig::instance() {
    static EspConfig cfg;
    return cfg;
}

void EspConfig::load() {
    settings_.loadFromConfig();
    ARIFT_DEBUG(kTagEsp, "ESP config loaded");
}

void EspConfig::save() {
    settings_.saveToConfig();
    Config::instance().save();
}

}  // namespace arift