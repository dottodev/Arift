# NewFJR Mod Menu — Reverse-Engineering Notes & Application Plan

Source: `C:\Users\mitra\Downloads\Newf Jr.apk.zip` (Newf Jr v2.2, package `com.newf.jr`).
Analyzed: 2026-08-20. Purpose: learn how a working MLBB mod menu really "injects",
then apply the same mechanics to Arift.

---

## 1. What NewFJR actually is

An **AndLua** app (`com.androlua.LuaApplication`, `LuaActivity`, `LuaService`,
`LuaAccessibilityService`) — i.e. a Lua runtime on Android. The menu UI and all
menu logic are **Lua scripts** (encrypted, AndLua `=`-prefixed), running on
`libluajava.so`, with `libsocket.so` (= **LuaSocket**, TCP) and `libmime.so`.

- targetSdk **26** (deliberately old: no background-FGS restrictions, no scoped
  storage pain)
- Permissions: `SYSTEM_ALERT_WINDOW`, `WRITE_EXTERNAL_STORAGE`, `WRITE_SETTINGS`,
  `REQUEST_INSTALL_PACKAGES`, accessibility service
- `android:persistent="true"`, `largeHeap`

## 2. The payloads in assets (the real story)

### 2a. Game-side asset drops — `assets/rankbooster/com.mobile.legends/files/dragon2017/assets/Document/android/`
Files that the mod menu copies into MLBB's own writable data directory
(`/data/data/com.mobile.legends/files/dragon2017/assets/Document/android/`).
The old MLBB "dragon2017" engine **loads files from that folder at boot**:

| File | What it really is | Effect |
|---|---|---|
| `Booster_Damage.So` (2 KB) | Plain **Lua text script** (reads `/sys/devices/system/cpu/cpu*/cpufreq/...`) | "Rank booster" / device speed |
| `Damage_Brutal_Skill.so` (2.8 MB) | **UnityFS asset bundle** (`BuildPlayer-PVP_032_extlow_add.sharedAssets8`) | Modded game assets (damage/skins) |

**No ptrace. No root. No "injection".** The game loads them itself on next
launch because they live inside its trusted data folder.

### 2b. Compiled memory-editor library — `assets/cpp/*.ini`
The `.ini` files are NOT text — they contain **compiled ARM64 machine code**
(dlopen'd / loaded as a library). Symbols extracted from `EspOn.ini`:

```
pread64 / pwrite64  on  /proc/%d/mem        <- DIRECT /proc/<pid>/mem file I/O
readmaps_* (code_app, java_heap, c_heap, c_bss, ashmem, stack)
getRoot / su -c / rebootsystem / pm install / pm uninstall / kill %d
killGG / killXs / com.mobile.legends:UnityKillsMe   <- kills anti-cheat process
BypassGameSafe
RangeMemorySearch_DWORD / _FLOAT, MemoryWrite_DWORD / _FLOAT
FreezeThread / AddFreezeItem_DWORD / SetFreezeDelay  <- GG-style value freezing
GetProcessState / GetDate / /proc/%d/status
```

### 2c. The channel
Menu (Lua) ↔ memory library talk over **LuaSocket (TCP, localhost)**:
payload opens a socket, menu connects and sends feature commands.

## 3. The three-channel injection model (summary)

1. **Memory hacks** — root + **direct `open("/proc/<pid>/mem", O_RDWR)` + `pread`/`pwrite`**.
   NO ptrace, NO process_vm_readv. Plus: kill the anti-cheat subprocess
   (`com.mobile.legends:UnityKillsMe`), bypass game safety checks.
2. **Asset drop** — copy `.so` / Lua / UnityFS into the game's
   `files/dragon2017/assets/Document/android/` folder; the game's own loader
   executes them on the next boot (no cross-process access at all).
3. **Command channel** — TCP socket on localhost for feature toggles.

## 4. How to apply this to Arift

### Already aligned (no change needed)
- Our native `ProcessMemory` (`vae/native/src/memory/memory_scanner.cpp`)
  already does exactly channel 1: `open("/proc/<pid>/mem", O_RDWR)` + `pread`/`pwrite`.
- Our menu↔core communication is JNI calls (fine — core lives in our process).

### What's blocking us (the "says it can't" problem) — verify, then fix
1. **Surface the real errno.** `ProcessMemory` captures `last_errno_` but the
   Kotlin layer never sees it. Add it to `nativeDiagDump()` and the status card:
   - `EACCES` → uid/SELinux blocks cross-process mem access
   - `EIO` → game process died/restarted
   - `ENOENT` → wrong pid
2. **Same-uid check inside the VAE.** Read `/proc/<pid>/status` `Uid:` line and
   compare with our own uid. If the VAE runs both apps under one uid,
   `/proc/<pid>/mem` MUST open — if it doesn't, the failure is SELinux domain,
   not uid.
3. **Root fallback (NewFJR's answer).** If `open()` fails: detect root
   (`su -c id` / `getuid()==0`) and, when available, open via a root helper
   (e.g. `su -c` + `chmod`/setuid trick or `exec` a tiny root opener). VAE
   containers frequently grant root.
4. **Kill the anti-cheat child (biggest single win).** Extend `ProcessManager`
   target discovery to find MLBB's anti-cheat subprocess
   (`com.mobile.legends:UnityKillsMe`, plus newer variants) and `SIGKILL` it
   right before attach/scan — exactly what NewFJR's `killGG`/`killXs` do.
   Our app is likely being interfered with by that process right now.
5. **O_RDONLY fallback.** If `O_RDWR` is denied but the target only needs
   reads (ESP etc.), open read-only first; upgrade to read-write only for
   features that write.

### Optional future work (channel 2 + 3, only if the VAE's MLBB version still loads `Document/android/`)
- Build a tiny `libarift_payload.so` that the app drops into the clone's
  `files/dragon2017/assets/Document/android/`; on game boot the engine loads it,
  it connects back to our menu over a localhost TCP socket, and our menu sends
  feature commands in-game. This is how modules that must live inside the game
  (render hooks) would work.
- Add GG-style value **freezing** (periodic rewrite of locked addresses) to the
  freeze-capable features (enemy lag, damage) — NewFJR proves the pattern.

## 5. Bottom line
NewFJR "injects" by: (a) direct `/proc/<pid>/mem` I/O (we already have this),
(b) killing the game's anti-cheat process, (c) root fallback, (d) asset drops
into the game's own folder. Our immediate fixes: surface the errno, check the
uid, add the root fallback, kill the anti-cheat child, O_RDONLY fallback.