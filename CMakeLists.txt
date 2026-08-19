cmake_minimum_required(VERSION 3.22)

project(arift_core LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED ANDROID_ABI)
    set(ANDROID_ABI "arm64-v8a")
endif()

set(ARIFT_TARGET_ABI "${ANDROID_ABI}")

set(ARIFT_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vae/native/src")
set(ARIFT_MODULE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/modules")

add_compile_options(-fvisibility=hidden -fno-builtin -fno-stack-protector -O2 -flto)

include_directories(
    "${ARIFT_SOURCE_DIR}"
    "${ARIFT_SOURCE_DIR}/common"
    "${ARIFT_SOURCE_DIR}/core"
    "${ARIFT_SOURCE_DIR}/memory"
    "${ARIFT_SOURCE_DIR}/hooks"
    "${ARIFT_SOURCE_DIR}/loader"
    "${ARIFT_MODULE_DIR}/esp"
    "${ARIFT_MODULE_DIR}/maphack"
    "${ARIFT_MODULE_DIR}/rankbooster"
    "${ARIFT_MODULE_DIR}/enemylag"
    "${ARIFT_MODULE_DIR}/voidban"
    "${ARIFT_MODULE_DIR}/autoretri"
    "${ARIFT_MODULE_DIR}/autoaim"
    "${ARIFT_MODULE_DIR}/tankdefense"
    "${ARIFT_MODULE_DIR}/physicaldamage"
    "${ARIFT_MODULE_DIR}/antidetect"
)

# ---------------------------------------------------------------------------
# arift_common — shared infrastructure
# ---------------------------------------------------------------------------
add_library(arift_common STATIC
    "${ARIFT_SOURCE_DIR}/common/arift_log.cpp"
    "${ARIFT_SOURCE_DIR}/common/arift_config.cpp"
    "${ARIFT_SOURCE_DIR}/common/arift_utils.cpp"
    "${ARIFT_SOURCE_DIR}/common/arift_crypto.cpp"
    "${ARIFT_SOURCE_DIR}/common/arift_thread.cpp"
    "${ARIFT_SOURCE_DIR}/common/arift_time.cpp"
    "${ARIFT_SOURCE_DIR}/common/arift_fs.cpp"
)
target_include_directories(arift_common PUBLIC
    "${ARIFT_SOURCE_DIR}/common"
)
target_link_libraries(arift_common PUBLIC log)

# ---------------------------------------------------------------------------
# arift_memory — memory scanning, pattern engine, patching
# ---------------------------------------------------------------------------
add_library(arift_memory STATIC
    "${ARIFT_SOURCE_DIR}/memory/memory_scanner.cpp"
    "${ARIFT_SOURCE_DIR}/memory/pattern_engine.cpp"
    "${ARIFT_SOURCE_DIR}/memory/memory_map.cpp"
    "${ARIFT_SOURCE_DIR}/memory/arm64_patch.cpp"
)
target_include_directories(arift_memory PUBLIC
    "${ARIFT_SOURCE_DIR}/memory"
)
target_link_libraries(arift_memory PUBLIC arift_common)

# ---------------------------------------------------------------------------
# arift_hooks — inline ARM64 + PLT hook engines
# ---------------------------------------------------------------------------
add_library(arift_hooks STATIC
    "${ARIFT_SOURCE_DIR}/hooks/hook_engine.cpp"
    "${ARIFT_SOURCE_DIR}/hooks/inline_hook_arm64.cpp"
    "${ARIFT_SOURCE_DIR}/hooks/plt_hook.cpp"
)
target_include_directories(arift_hooks PUBLIC
    "${ARIFT_SOURCE_DIR}/hooks"
)
target_link_libraries(arift_hooks PUBLIC arift_memory arift_common)

# ---------------------------------------------------------------------------
# arift_loader — module loader, ELF parsing, hidden library loading
# ---------------------------------------------------------------------------
add_library(arift_loader STATIC
    "${ARIFT_SOURCE_DIR}/loader/elf_parser.cpp"
    "${ARIFT_SOURCE_DIR}/loader/dl_loader.cpp"
)
target_include_directories(arift_loader PUBLIC
    "${ARIFT_SOURCE_DIR}/loader"
)
target_link_libraries(arift_loader PUBLIC arift_hooks arift_memory arift_common)

# ---------------------------------------------------------------------------
# arift_core — JNI bridge + cheat registry (the injection entry point)
# ---------------------------------------------------------------------------
add_library(arift_core SHARED
    "${ARIFT_SOURCE_DIR}/jni_bridge.cpp"
    "${ARIFT_SOURCE_DIR}/core/cheat_registry.cpp"
    "${ARIFT_SOURCE_DIR}/core/arift_core_api.cpp"
    "${ARIFT_SOURCE_DIR}/core/injection_flow.cpp"
    "${ARIFT_SOURCE_DIR}/core/guard_thread.cpp"
    "${ARIFT_SOURCE_DIR}/core/feature_switch.cpp"
    "${ARIFT_MODULE_DIR}/esp/esp_manager.cpp"
    "${ARIFT_MODULE_DIR}/esp/esp_renderer.cpp"
    "${ARIFT_MODULE_DIR}/esp/esp_entities.cpp"
    "${ARIFT_MODULE_DIR}/esp/esp_config.cpp"
    "${ARIFT_MODULE_DIR}/maphack/maphack_manager.cpp"
    "${ARIFT_MODULE_DIR}/maphack/fog_bypass.cpp"
    "${ARIFT_MODULE_DIR}/maphack/minimap_override.cpp"
    "${ARIFT_MODULE_DIR}/rankbooster/rb_engine.cpp"
    "${ARIFT_MODULE_DIR}/rankbooster/rb_core.cpp"
    "${ARIFT_MODULE_DIR}/rankbooster/rb_config.cpp"
    "${ARIFT_MODULE_DIR}/rankbooster/rb_types.cpp"
    "${ARIFT_MODULE_DIR}/rankbooster/rb_elo.cpp"
    "${ARIFT_MODULE_DIR}/rankbooster/rb_mmr.cpp"
    "${ARIFT_MODULE_DIR}/rankbooster/rb_matchmaking.cpp"
    "${ARIFT_MODULE_DIR}/rankbooster/rb_lobby.cpp"
    "${ARIFT_MODULE_DIR}/rankbooster/rb_network.cpp"
    "${ARIFT_MODULE_DIR}/rankbooster/rb_protocol.cpp"
    "${ARIFT_MODULE_DIR}/rankbooster/rb_telemetry.cpp"
    "${ARIFT_MODULE_DIR}/rankbooster/rb_ai.cpp"
    "${ARIFT_MODULE_DIR}/rankbooster/rb_cache.cpp"
    "${ARIFT_MODULE_DIR}/rankbooster/rb_guard.cpp"
    "${ARIFT_MODULE_DIR}/rankbooster/rb_tuner.cpp"
    "${ARIFT_MODULE_DIR}/rankbooster/rb_statistics.cpp"
    "${ARIFT_MODULE_DIR}/rankbooster/rb_profile.cpp"
    "${ARIFT_MODULE_DIR}/rankbooster/rb_utils.cpp"
    "${ARIFT_MODULE_DIR}/rankbooster/rb_scheduler.cpp"
    "${ARIFT_MODULE_DIR}/enemylag/enemy_lag.cpp"
    "${ARIFT_MODULE_DIR}/enemylag/packet_proxy.cpp"
    "${ARIFT_MODULE_DIR}/voidban/void_ban.cpp"
    "${ARIFT_MODULE_DIR}/voidban/vb_hooks.cpp"
    "${ARIFT_MODULE_DIR}/voidban/vb_cloak.cpp"
    "${ARIFT_MODULE_DIR}/autoretri/autoretri.cpp"
    "${ARIFT_MODULE_DIR}/autoaim/autoaim.cpp"
    "${ARIFT_MODULE_DIR}/tankdefense/tankdefense.cpp"
    "${ARIFT_MODULE_DIR}/physicaldamage/physicaldamage.cpp"
    "${ARIFT_MODULE_DIR}/antidetect/antidetect.cpp"
)
target_include_directories(arift_core PRIVATE
    "${ARIFT_SOURCE_DIR}/core"
    "${ARIFT_MODULE_DIR}/esp"
    "${ARIFT_MODULE_DIR}/maphack"
    "${ARIFT_MODULE_DIR}/rankbooster"
    "${ARIFT_MODULE_DIR}/enemylag"
    "${ARIFT_MODULE_DIR}/voidban"
    "${ARIFT_MODULE_DIR}/autoretri"
    "${ARIFT_MODULE_DIR}/autoaim"
    "${ARIFT_MODULE_DIR}/tankdefense"
    "${ARIFT_MODULE_DIR}/physicaldamage"
    "${ARIFT_MODULE_DIR}/antidetect"
)
target_link_libraries(arift_core PUBLIC
    arift_loader
    arift_hooks
    arift_memory
    arift_common
    log
    dl
)

# ---------------------------------------------------------------------------
# Install rules — single packed .so for the host app
# ---------------------------------------------------------------------------
install(TARGETS arift_core LIBRARY DESTINATION lib/${ANDROID_ABI})
