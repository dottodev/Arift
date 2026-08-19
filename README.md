# ARIFT — MLBB Cheat Injector (Project ARIFT)

Super-advanced Mobile Legends: Bang Bang cheat injector framework designed to
run inside a customized Virtual Android Environment (V.A.E) for isolation,
hooking, and device spoofing.

> Educational research / internal auditing / personal development framework.
> Using this against live servers violates MLBB ToS and can cause permanent bans.

## Modules

| Module | Language | Status | Phase |
|---|---|---|---|
| V.A.E Host Loader (injection manager, spawner) | Kotlin/Android | Implemented | 1 |
| Native injection bridge + memory/hook infra | C++ (NDK) | Implemented | 1 |
| ESP (boxes, health, cooldowns, objectives) | C++ | Implemented | 2 |
| Map Hack (fog-of-war bypass, minimap override) | C++ | Implemented | 2 |
| Rank Booster (matchmaking/MMR simulation engine) | C++ | Implemented | 2 |
| Auto Retri (auto pickup + shop logic) | C++ | Implemented | 3 |
| Auto Aim (targeted aim assist) | C++ | Implemented | 3 |
| Tank Defense (defensive automation) | C++ | Implemented | 3 |
| Physical Damage (damage rotation automation) | C++ | Implemented | 3 |
| Enemy Lag (targeted packet delay) | C++ | Implemented | 4 |
| Void Ban (anti-detection fortress) | C++ | Implemented | 5 |
| UI overlay ("ARIFT MENU") | Kotlin | Implemented | 6 (early) |

## Layout

```
Arift/
├── vae/android/     # Host loader + UI overlay (Kotlin app)
├── vae/native/      # JNI bridge, memory, hooks, loader (C++)
├── modules/         # Cheat modules
│   ├── esp/         # ESP
│   ├── maphack/     # Map hack
│   ├── rankbooster/ # Rank booster engine (largest module)
│   ├── enemylag/    # Enemy lag (phase 4)
│   ├── voidban/     # Void ban (phase 5)
│   ├── autoretri/   # Auto retri (phase 3)
│   ├── autoaim/     # Auto aim (phase 3)
│   ├── tankdefense/ # Tank defense (phase 3)
│   ├── physicaldamage/ # Physical damage (phase 3)
│   └── antidetect/  # Anti-detection framework (risk gate + shields)
├── scripts/         # Build + verification tooling
└── docs/            # Design specs
```

## Building

Requirements: Android NDK (r25+), CMake 3.22+, Android SDK, Gradle 8.

```powershell
# 1. Build native core (all .so for arm64-v8a)
./scripts/build.ps1 -Native

# 2. Build host loader APK
./scripts/build.ps1 -Apk

# 3. Verify module line-count targets
./scripts/verify_lines.ps1
```

## Line-Count Targets (enforced by scripts/verify_lines.ps1)

| Module | Target |
|---|---|
| ESP | >= 1000 |
| Map Hack | >= 1000 |
| Rank Booster | >= 9000 |
| Enemy Lag | >= 3000 (phase 4) |
| Void Ban | >= 7000 (phase 5) |
| Auto Retri | >= 1000 (phase 3) |
| Auto Aim | >= 1000 (phase 3) |
| Tank Defense | >= 1000 (phase 3) |
| Physical Damage | >= 1000 (phase 3) |
| Anti Detect | >= 1000 (core framework) |

## Phase Status

- Phase 1 — V.A.E foundation + basic injection: DONE
- Phase 2 — ESP, Map Hack, Rank Booster: DONE
- Phase 3 — Auto Retri, Auto Aim, Tank Defense, Physical Damage: DONE
- Phase 4 — Enemy Lag: DONE
- Phase 5 — Void Ban: DONE
- Phase 6 — UI + refinement: DONE (ARIFT MENU overlay, all features wired)
