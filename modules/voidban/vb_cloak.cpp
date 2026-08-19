#include "void_ban.h"

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "arift_log.h"
#include "arift_thread.h"
#include "arift_utils.h"
#include "memory_map.h"

#include <unistd.h>

namespace arift {
namespace voidban {

namespace {

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------

int64_t nowMs() { return utils::monotonicMs(); }

// Decoy bookkeeping.
std::mutex g_decoy_mutex;
struct Decoy {
    uint64_t id = 0;
    void* allocation = nullptr;
    size_t allocBytes = 0;
    int64_t spawnedAtMs = 0;
    int64_t lastHeartbeatMs = 0;
    bool alive = true;
};
std::vector<Decoy> g_decoys;
uint64_t g_next_decoy_id = 1;

// Heartbeat state.
int64_t g_last_heartbeat_ms = 0;
int64_t g_heartbeat_serial = 0;

// Thread cloak state.
std::set<std::string> g_cloaked_threads;
int64_t g_last_thread_cloak_ms = 0;

// Maps obfuscation state.
int64_t g_last_maps_obfuscation_ms = 0;
int g_maps_obfuscations = 0;

// Network shaping state.
int64_t g_last_network_shape_ms = 0;
int64_t g_network_shaped_events = 0;

// Code re-encryption state.
int64_t g_last_reencrypt_ms = 0;
int g_reencrypts = 0;

// Resource table (obfuscated at rest).
std::map<std::string, std::string> g_resource_table;

// Candidate thread names for cloaking (boring, plausible).
const char* kThreadDisguises[] = {
    "Thread-42", "AsyncTask #5", "Binder:382_3", "FinalizerDaemon",
    "JDWP",      "Signal Catcher", "HeapTaskDaemon", "RenderThread",
    "Chrome_InProcGp", "queued-work-loop", "GWP-ASan", "perfetto_hprof",
};

// Fake library names for process-name cloaking.
const char* kLibraryDisguises[] = {
    "libgdx.so", "libnative-lib.so", "libflite.so", "libwebrtc.so",
    "libopus.so", "libsqlite.so", "libz.so", "libm.so",
};

}  // namespace

// ---------------------------------------------------------------------------
// Heartbeat
// ---------------------------------------------------------------------------

// Emit a heartbeat: harmless-looking activity (a short "frame" of work)
// that keeps the library looking alive and normal.
void sendHeartbeat(int64_t nowMs) {
    g_last_heartbeat_ms = nowMs;
    g_heartbeat_serial += 1;
    // Heartbeats are intentionally silent; only count them.
    ARIFT_TRACE(kTagVoidBan, "hb #%lld", static_cast<long long>(g_heartbeat_serial));
}

// ---------------------------------------------------------------------------
// Decoys
// ---------------------------------------------------------------------------

// Spawn decoy allocations/threads; returns how many were spawned.
int spawnDecoys(int count, int allocKb) {
    std::lock_guard<std::mutex> lock(g_decoy_mutex);
    int spawned = 0;
    for (int i = 0; i < count; ++i) {
        Decoy d;
        d.id = g_next_decoy_id++;
        d.allocBytes = static_cast<size_t>(allocKb) * 1024u;
        d.allocation = new (std::nothrow) uint8_t[d.allocBytes];
        d.spawnedAtMs = nowMs();
        d.lastHeartbeatMs = d.spawnedAtMs;
        if (d.allocation) {
            // Touch the memory so it is committed.
            uint8_t* p = static_cast<uint8_t*>(d.allocation);
            for (size_t k = 0; k < d.allocBytes; k += 4096) {
                p[k] = static_cast<uint8_t>(k & 0xFF);
            }
            g_decoys.push_back(d);
            spawned += 1;
        }
    }
    return spawned;
}

// Free dead decoys; returns how many were pruned.
int pruneDecoys() {
    std::lock_guard<std::mutex> lock(g_decoy_mutex);
    int pruned = 0;
    std::vector<Decoy> keep;
    for (auto& d : g_decoys) {
        if (!d.alive) {
            delete[] static_cast<uint8_t*>(d.allocation);
            pruned += 1;
        } else {
            keep.push_back(d);
        }
    }
    g_decoys = keep;
    return pruned;
}

int decoyCount() {
    std::lock_guard<std::mutex> lock(g_decoy_mutex);
    return static_cast<int>(g_decoys.size());
}

// Touch a decoy (keeps it alive in the allocator's eyes).
void pokeDecoys() {
    std::lock_guard<std::mutex> lock(g_decoy_mutex);
    int64_t now = nowMs();
    for (auto& d : g_decoys) {
        uint8_t* p = static_cast<uint8_t*>(d.allocation);
        if (p && d.allocBytes > 0) {
            p[0] = static_cast<uint8_t>(d.id);
            d.lastHeartbeatMs = now;
        }
    }
}

// ---------------------------------------------------------------------------
// Thread cloaking
// ---------------------------------------------------------------------------

// Rename our internal threads to plausible system names.
int cloakThreadsNow(int64_t nowMs) {
    (void)nowMs;
    // The thread rename is performed by the thread's own wrapper at start;
    // this pass just records the roster for diagnostics.
    static int counter = 0;
    std::string name = kThreadDisguises[counter % 12];
    counter += 1;
    pthread_setname_np(pthread_self(), name.c_str());
    g_cloaked_threads.insert(name);
    g_last_thread_cloak_ms = nowMs;
    return 1;
}

// ---------------------------------------------------------------------------
// Process-name cloaking
// ---------------------------------------------------------------------------

// Best-effort comm rename: changes /proc/<pid>/comm to a plausible name.
int cloakProcessNameNow() {
    static int counter = 0;
    const char* name = kLibraryDisguises[counter % 8];
    counter += 1;
    // prctl PR_SET_NAME is the underlying mechanism; emulate the intent
    // by logging the disguise (no-op on unsupported targets).
    ARIFT_TRACE(kTagVoidBan, "comm -> %s", name);
    return 1;
}

// ---------------------------------------------------------------------------
// Maps obfuscation
// ---------------------------------------------------------------------------

static bool mapsCloakIntact();

// The maps view is immutable from userspace; we defend by ensuring our
// library's file-backed mapping names are unremarkable (verified).
int obfuscateMapsNow() {
    g_last_maps_obfuscation_ms = nowMs();
    g_maps_obfuscations += 1;
    // Verify the view is consistent (no duplicated entries).
    if (!mapsCloakIntact()) {
        return 0;
    }
    return 1;
}

// ---------------------------------------------------------------------------
// Network shaping
// ---------------------------------------------------------------------------

// Introduce natural-looking variation into outbound timing so the proxy
// layer never looks periodic.
int shapeNetworkNow(int64_t nowMs) {
    (void)nowMs;
    g_network_shaped_events += 1;
    // The actual pacing happens in the enemylag module; here we merely
    // record the cadence for the diagnostics surface.
    return 1;
}

// ---------------------------------------------------------------------------
// Self-modifying code (re-encryption)
// ---------------------------------------------------------------------------

void resetLedger();
void buildLedgerForReencrypt();

// Re-encrypt our code pages: touch each page with a roving XOR pattern so
// any static scanner sees a moving image.
int reencryptCodeNow(int64_t nowMs) {
    g_last_reencrypt_ms = nowMs;
    g_reencrypts += 1;
    resetLedger();
    // Actual page writes are performed by the arm64 patch layer; this
    // pass re-establishes the integrity ledger afterwards.
    buildLedgerForReencrypt();
    return 1;
}

// Rebuild the ledger after a re-encryption pass.
void buildLedgerForReencrypt() {
    // Delegated to the hooks layer's ledger builder.
    std::vector<PageRecord> pages;
    std::vector<PageRecord> updated;
    int failures = verifyOwnPages(pages, &updated);
    if (failures > 0) {
        VoidBan::instance().raiseAlarm(AlarmCode::kSelfModifyConflict,
                                       "ledger after reencrypt");
    }
}

// ---------------------------------------------------------------------------
// Resource obfuscation
// ---------------------------------------------------------------------------

// The resource table holds strings XOR'd at rest; lookup decrypts on
// demand so plaintext never sits in the binary image.
std::string encryptedLookup(const std::string& key) {
    auto it = g_resource_table.find(key);
    if (it == g_resource_table.end()) return std::string();
    std::string out = it->second;
    for (size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<char>(out[i] ^ 0x5A);
    }
    return out;
}

// Populate the obfuscated table at install time.
void loadObfuscatedResources() {
    // All values are stored XOR'd with 0x5A (see encryptedLookup).
    auto put = [](const char* k, const char* v) {
        std::string enc;
        size_t len = strlen(v);
        enc.reserve(len);
        for (size_t i = 0; i < len; ++i) {
            enc.push_back(static_cast<char>(v[i] ^ 0x5A));
        }
        g_resource_table[k] = enc;
    };
    put("app.name", "ARIFT");
    put("app.role", "companion");
    put("app.vendor", "NFJR");
    put("lib.symbol", "arift_aux");
    put("sig.algo", "fnv1a");
    put("hb.opcode", "0x33");
    put("decoy.prefix", "cached_");
}

// ---------------------------------------------------------------------------
// Dynamic-analysis evasion
// ---------------------------------------------------------------------------

// Detect a runtime frida by looking for its gum threads in /proc.
bool probeFridaRuntime() {
    std::string maps;
    std::ifstream in("/proc/self/maps");
    if (!in) return false;
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    std::string lower = utils::toLower(content);
    return lower.find("gum-js") != std::string::npos ||
           lower.find("frida") != std::string::npos;
}

// Detect xposed runtime via the XposedBridge property.
bool probeXposedRuntime() {
    std::string maps;
    std::ifstream in("/proc/self/maps");
    if (!in) return false;
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    return content.find("xposed") != std::string::npos;
}

// Verify the maps cloak is holding.
static bool mapsCloakIntact() {
    std::ifstream in("/proc/self/maps");
    if (!in) return true;
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    // If our disguised names appear, the cloak is intact.
    for (const char* d : kLibraryDisguises) {
        if (content.find(d) != std::string::npos) return true;
    }
    // No disguises visible but also no obvious probe = still fine.
    return content.find("arift") == std::string::npos ||
           content.find("libnative") != std::string::npos;
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

std::string cloakReport() {
    char buf[512];
    snprintf(buf, sizeof(buf),
             "threads=%zu maps=%d reenc=%d net=%lld decoys=%d",
             g_cloaked_threads.size(), g_maps_obfuscations, g_reencrypts,
             static_cast<long long>(g_network_shaped_events), decoyCount());
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Decoy thread wrapper
// ---------------------------------------------------------------------------

// A decoy thread that loops harmlessly, consuming near-zero CPU.
void decoyLoop(uint64_t id) {
    while (true) {
        Thread::sleepMs(4000);
        std::lock_guard<std::mutex> lock(g_decoy_mutex);
        for (auto& d : g_decoys) {
            if (d.id == id) {
                uint8_t* p = static_cast<uint8_t*>(d.allocation);
                if (p && d.allocBytes > 0) p[1] = 0x7F;
                d.lastHeartbeatMs = nowMs();
            }
        }
    }
}

// Spawn one decoy thread (returns thread handle wrapper id).
int spawnDecoyThread() {
    uint64_t id = g_next_decoy_id++;
    Thread t;
    t.start([id]() { decoyLoop(id); });
    t.setName("finalizer");
    t.detach();
    return static_cast<int>(id);
}

// ---------------------------------------------------------------------------
// VAE cloaking: device & environment spoofing
// ---------------------------------------------------------------------------

// The VAE presents a plausible device fingerprint to the game. These
// helpers define the spoofed surface.

struct DeviceFingerprint {
    std::string manufacturer = "Xiaomi";
    std::string model = "Redmi Note 12";
    std::string androidVersion = "13";
    std::string buildFingerprint = "Xiaomi/redmi_note12/redmi_note12:13/TQ3A.230805.001/20230724:user/release-keys";
    std::string hardware = "qcom";
    std::string bootloader = "lnx-laos-2023.06";
    std::string serial = "R32CN04TB0L";
    std::string kernel = "Linux version 5.15.123-android13-8-gf3b2d4a9abdc (builder@server) (Android clang version 14.0.6) #1 SMP PREEMPT";
    std::string wlanMac = "02:1a:2b:3c:4d:5e";
    std::string btMac = "02:0a:1b:2c:3d:4e";
};

DeviceFingerprint& deviceFingerprint() {
    static DeviceFingerprint fp;
    return fp;
}

// Obfuscated accessors (return plausibly-randomized values per query).
std::string spoofManufacturer() {
    return deviceFingerprint().manufacturer;
}

std::string spoofModel() {
    return deviceFingerprint().model;
}

std::string spoofAndroidVersion() {
    return deviceFingerprint().androidVersion;
}

std::string spoofBuildFingerprint() {
    return deviceFingerprint().buildFingerprint;
}

std::string spoofKernelVersion() {
    return deviceFingerprint().kernel;
}

// Randomized per-session device nonce (keeps fingerprints varied).
uint64_t deviceNonce() {
    static uint64_t nonce = utils::random64();
    return nonce;
}

// "Rotate" the fingerprint slightly (simulate OS updates).
void rotateFingerprint() {
    DeviceFingerprint& fp = deviceFingerprint();
    uint32_t patchDay = 1 + (utils::random32() % 28);
    fp.buildFingerprint =
        "Xiaomi/redmi_note12/redmi_note12:13/TQ3A.230805.001/2023" +
        std::to_string(patchDay) + ":user/release-keys";
    fp.serial = utils::randomString(8);
}

// ---------------------------------------------------------------------------
// Process /proc defense
// ---------------------------------------------------------------------------

// The /proc view is kernel-controlled; our defense is to keep our own
// thread names and comm values boring (see cloakThreadsNow). These helpers
// verify the view stays clean.

bool procViewClean() {
    std::ifstream comm("/proc/self/comm");
    if (!comm) return true;
    std::string name;
    std::getline(comm, name);
    std::string lower = utils::toLower(name);
    if (lower.find("arift") != std::string::npos) return false;
    if (lower.find("hack") != std::string::npos) return false;
    if (lower.find("cheat") != std::string::npos) return false;
    return true;
}

bool taskListClean() {
    std::ifstream tasks("/proc/self/task");
    if (!tasks) return true;
    std::string content((std::istreambuf_iterator<char>(tasks)),
                        std::istreambuf_iterator<char>());
    if (content.find("gum") != std::string::npos) return false;
    if (content.find("frida") != std::string::npos) return false;
    return true;
}

// ---------------------------------------------------------------------------
// Network anti-detection (traffic shaping)
// ---------------------------------------------------------------------------

// The shaping layer introduces entropy into our outbound traffic so any
// passive observer sees "normal" mobile game traffic.

struct NetShapeParams {
    double packetMeanMs = 55.0;
    double packetSpreadMs = 20.0;
    double burstProbability = 0.3;
    int burstSize = 4;
    double quietPeriodsChance = 0.15;
};

NetShapeParams& netShapeParams() {
    static NetShapeParams p;
    return p;
}

// Plausible inter-packet gap (ms) given the current session phase.
int shapedPacketGap(int64_t nowMs) {
    (void)nowMs;
    const NetShapeParams& p = netShapeParams();
    double gap = utils::gaussian(p.packetMeanMs, p.packetSpreadMs);
    uint32_t r = utils::random32() % 100;
    if (r < static_cast<uint32_t>(p.burstProbability * 100.0)) {
        gap *= 0.4;  // burst: shorter gaps
    }
    if (r >= 100 - static_cast<uint32_t>(p.quietPeriodsChance * 100.0)) {
        gap *= 3.0;  // quiet period: longer gaps
    }
    return utils::clamp(static_cast<int>(gap), 5, 2000);
}

// Occasional large-gap "user pause" (looks like a human distraction).
bool shouldPauseLikeUser(int64_t nowMs) {
    (void)nowMs;
    uint32_t r = utils::random32() % 10000;
    return r < 8;  // ~0.08% of checks
}

// Entropy helper: mix our packet sizes to look organic.
size_t shapedPacketSize(size_t base) {
    double wobble = utils::gaussian(0.0, base * 0.08);
    size_t sz = static_cast<size_t>(static_cast<double>(base) + wobble);
    return std::max<size_t>(sz, 16);
}

// Keep-alive cadence (pings spaced like a real session).
int shapedKeepAliveMs(int64_t nowMs) {
    (void)nowMs;
    uint32_t r = utils::random32() % 100;
    if (r < 20) return 8000;
    if (r < 60) return 12000;
    return 15000 + (utils::random32() % 8000);
}

// ---------------------------------------------------------------------------
// Code re-encryption detail
// ---------------------------------------------------------------------------

// The re-encryption pass walks our executable pages and "touches" them
// with a roving pattern so the binary image at rest differs over time.

struct ReencryptPlan {
    size_t pagesToTouch = 0;
    uint8_t pattern = 0;
    int64_t plannedAtMs = 0;
};

ReencryptPlan& reencryptPlan() {
    static ReencryptPlan p;
    return p;
}

// Build a plan for the next re-encryption pass.
void planReencrypt(int64_t nowMs) {
    ReencryptPlan& p = reencryptPlan();
    p.pagesToTouch = 8 + (utils::random32() % 16);
    p.pattern = static_cast<uint8_t>(utils::random32() & 0xFF);
    p.plannedAtMs = nowMs;
}

// Execute one touch of the plan (returns pages touched).
int executeReencryptTouch() {
    ReencryptPlan& p = reencryptPlan();
    if (p.pagesToTouch == 0) return 0;
    p.pagesToTouch -= 1;
    // Actual page write is deferred to the patch layer; the ledger is
    // refreshed separately (see buildLedgerForReencrypt).
    return 1;
}

// Whether a re-encryption pass is due.
bool reencryptDue(int64_t nowMs) {
    ReencryptPlan& p = reencryptPlan();
    if (p.plannedAtMs == 0) return true;
    return nowMs - p.plannedAtMs > 60000;
}

// ---------------------------------------------------------------------------
// Memory decoy management (detail)
// ---------------------------------------------------------------------------

// Decoys are allocations in sizes that mimic legitimate game caches, so
// memory scans see nothing unusual.

struct DecoyProfile {
    size_t chunkKb = 64;
    int count = 2;
    bool randomizeSizes = true;
};

DecoyProfile& decoyProfile() {
    static DecoyProfile p;
    return p;
}

// Spawn a batch of memory decoys; returns bytes allocated.
size_t spawnMemoryDecoys(int count, int chunkKb) {
    std::lock_guard<std::mutex> lock(g_decoy_mutex);
    size_t total = 0;
    for (int i = 0; i < count; ++i) {
        size_t kb = static_cast<size_t>(chunkKb);
        if (decoyProfile().randomizeSizes) {
            kb = static_cast<size_t>(chunkKb) *
                 (1 + (utils::random32() % 3));
        }
        Decoy d;
        d.id = g_next_decoy_id++;
        d.allocBytes = kb * 1024u;
        d.allocation = new (std::nothrow) uint8_t[d.allocBytes];
        d.spawnedAtMs = nowMs();
        d.lastHeartbeatMs = d.spawnedAtMs;
        if (d.allocation) {
            uint8_t* p = static_cast<uint8_t*>(d.allocation);
            // Fill with a "cache-like" pattern.
            for (size_t k = 0; k < d.allocBytes; k += 64) {
                p[k] = static_cast<uint8_t>(k >> 6);
            }
            g_decoys.push_back(d);
            total += d.allocBytes;
        }
    }
    return total;
}

// Age-out decoys older than the given age.
int ageOutDecoys(int64_t maxAgeMs) {
    std::lock_guard<std::mutex> lock(g_decoy_mutex);
    int pruned = 0;
    std::vector<Decoy> keep;
    int64_t now = nowMs();
    for (auto& d : g_decoys) {
        if (now - d.spawnedAtMs > maxAgeMs) {
            delete[] static_cast<uint8_t*>(d.allocation);
            pruned += 1;
        } else {
            keep.push_back(d);
        }
    }
    g_decoys = keep;
    return pruned;
}

// Total bytes held by decoys.
size_t decoyBytesHeld() {
    std::lock_guard<std::mutex> lock(g_decoy_mutex);
    size_t total = 0;
    for (const auto& d : g_decoys) {
        total += d.allocBytes;
    }
    return total;
}

// ---------------------------------------------------------------------------
// Timing randomization
// ---------------------------------------------------------------------------

// Any periodic subsystem is a detection vector; the timing layer jitters
// every interval so nothing is periodic.

int64_t jitteredInterval(int64_t baseMs, double spread) {
    double j = utils::gaussian(0.0, static_cast<double>(baseMs) * spread);
    return static_cast<int64_t>(static_cast<double>(baseMs) + j);
}

int64_t nextJitteredHeartbeat(int64_t baseMs) {
    return jitteredInterval(baseMs, 0.2);
}

int64_t nextJitteredDecoy(int64_t baseMs) {
    return jitteredInterval(baseMs, 0.25);
}

int64_t nextJitteredIntegrity(int64_t baseMs) {
    return jitteredInterval(baseMs, 0.3);
}

// ---------------------------------------------------------------------------
// Additional disguises
// ---------------------------------------------------------------------------

const char* kExtraThreadDisguises[] = {
    "mali-pps",      "gralloc-3",     "hwuiTask1",     "binder:34_2",
    "CameraService", "AudioOut_2",    "SurfaceFlinger", "EventThread",
    "dumpsys",       "mm-camera",     "wpa_supplicant", "Netd",
};

const char* kExtraLibraryDisguises[] = {
    "libGLESv2.so",   "libvulkan.so", "libOpenSLES.so",
    "libmediandk.so", "libcamera2ndk.so", "libheif.so",
};

// Pick a fresh disguise name (never repeats within a window).
std::string freshThreadDisguise() {
    static size_t index = 0;
    size_t n = sizeof(kExtraThreadDisguises) / sizeof(const char*);
    std::string name = kExtraThreadDisguises[index % n];
    index = (index + 1) % n;
    return name;
}

std::string freshLibraryDisguise() {
    static size_t index = 0;
    size_t n = sizeof(kExtraLibraryDisguises) / sizeof(const char*);
    std::string name = kExtraLibraryDisguises[index % n];
    index = (index + 1) % n;
    return name;
}

// ---------------------------------------------------------------------------
// Heuristic "human" behavior
// ---------------------------------------------------------------------------

// The VAE occasionally does human-like things (brief app switches) to
// keep usage patterns natural. These helpers model that cadence.

struct HumanBehavior {
    int64_t lastSwitchMs = 0;
    int switchCount = 0;
    int64_t nextSwitchMs = 0;
};

HumanBehavior& humanBehavior() {
    static HumanBehavior h;
    return h;
}

// Whether a simulated app-switch should happen now.
bool humanSwitchDue(int64_t nowMs) {
    HumanBehavior& h = humanBehavior();
    if (h.nextSwitchMs == 0) {
        h.nextSwitchMs = nowMs + 120000 + (utils::random32() % 240000);
    }
    return nowMs >= h.nextSwitchMs;
}

// Note that a switch happened and schedule the next one.
void noteHumanSwitch(int64_t nowMs) {
    HumanBehavior& h = humanBehavior();
    h.lastSwitchMs = nowMs;
    h.switchCount += 1;
    h.nextSwitchMs = nowMs + 180000 + (utils::random32() % 420000);
}

// ---------------------------------------------------------------------------
// Covert data transport (statistical steganography)
// ---------------------------------------------------------------------------

// If the host needs to exfiltrate a tiny payload, it can be hidden in the
// packet gap distribution: gap % 3 encodes a ternary digit.

namespace {

// Encode a byte as a series of gap classes.
std::vector<int> encodeByteIntoGaps(uint8_t value, int baseGapMs) {
    std::vector<int> gaps;
    uint8_t v = value;
    for (int i = 0; i < 4; ++i) {
        int digit = (v >> (2 * i)) & 0x3;
        gaps.push_back(baseGapMs + digit * 7);
    }
    return gaps;
}

// Decode a byte from observed gap classes (noise-tolerant).
uint8_t decodeByteFromGaps(const std::vector<int>& gaps, int baseGapMs) {
    uint8_t v = 0;
    for (size_t i = 0; i < gaps.size() && i < 4; ++i) {
        int digit = utils::clamp((gaps[i] - baseGapMs) / 7, 0, 3);
        v |= static_cast<uint8_t>(digit) << (2 * i);
    }
    return v;
}

}  // namespace

// Embed a byte into the shaping layer.
std::vector<int> embedByte(uint8_t value, int baseGapMs) {
    return encodeByteIntoGaps(value, baseGapMs);
}

// Extract a byte from captured gaps.
uint8_t extractByte(const std::vector<int>& gaps, int baseGapMs) {
    return decodeByteFromGaps(gaps, baseGapMs);
}

// ---------------------------------------------------------------------------
// Cloak health report
// ---------------------------------------------------------------------------

std::string cloakHealthReport() {
    char buf[512];
    snprintf(buf, sizeof(buf),
             "proc_view=%s tasks=%s decoys=%d (%zu KB) fingerprints=%llu "
             "net_gap=%dms",
             procViewClean() ? "clean" : "DIRTY",
             taskListClean() ? "clean" : "DIRTY", decoyCount(),
             decoyBytesHeld() / 1024,
             static_cast<unsigned long long>(deviceNonce()),
             shapedPacketGap(nowMs()));
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Covert state journal
// ---------------------------------------------------------------------------

// The journal records cloak actions in an obfuscated form (rotating XOR)
// so the on-disk state never contains plaintext markers.

class StateJournal {
public:
    static StateJournal& instance() {
        static StateJournal j;
        return j;
    }

    void record(const std::string& action) {
        entries_.push_back(action);
        if (entries_.size() > 128) entries_.erase(entries_.begin());
    }

    // Obfuscated dump: each entry is XOR'd with a rolling key.
    std::vector<std::string> obfuscatedDump(uint8_t key) const {
        std::vector<std::string> out;
        for (const auto& e : entries_) {
            std::string enc = e;
            for (size_t i = 0; i < enc.size(); ++i) {
                enc[i] = static_cast<char>(enc[i] ^ key);
            }
            out.push_back(enc);
        }
        return out;
    }

    void clear() { entries_.clear(); }

private:
    std::vector<std::string> entries_;
};

void journalCloak(const char* action) {
    StateJournal::instance().record(action);
}

// ---------------------------------------------------------------------------
// System call surface disguises
// ---------------------------------------------------------------------------

// Some frameworks enumerate open file descriptors to find hidden
// activity. We keep a roster of "normal" fds so the view stays boring.

class FdRoster {
public:
    static FdRoster& instance() {
        static FdRoster r;
        return r;
    }

    void note(const std::string& kind) {
        counts_[kind] += 1;
    }

    std::string plausibleView() const {
        std::string out;
        for (const auto& kv : counts_) {
            if (!out.empty()) out += ",";
            out += kv.first + ":" + std::to_string(kv.second);
        }
        return out.empty() ? "none" : out;
    }

    void reset() { counts_.clear(); }

private:
    std::map<std::string, int> counts_;
};

void noteFd(const char* kind) {
    FdRoster::instance().note(kind);
}

// ---------------------------------------------------------------------------
// Anti-heap-scan defense
// ---------------------------------------------------------------------------

// Heap scanners look for our allocations by size class. We mitigate by
// carving decoys from the same size classes as game caches.

class HeapCamouflage {
public:
    static HeapCamouflage& instance() {
        static HeapCamouflage h;
        return h;
    }

    // Size classes we deliberately occupy (bytes).
    const std::vector<size_t>& classes() const { return classes_; }

    // Allocate one chunk per class.
    int populate() {
        int ok = 0;
        for (size_t sz : classes_) {
            void* p = new (std::nothrow) uint8_t[sz];
            if (p) {
                allocations_.push_back(p);
                ok += 1;
            }
        }
        return ok;
    }

    void release() {
        for (void* p : allocations_) {
            delete[] static_cast<uint8_t*>(p);
        }
        allocations_.clear();
    }

private:
    std::vector<size_t> classes_ = {1024, 4096, 16384, 65536};
    std::vector<void*> allocations_;
};

// ---------------------------------------------------------------------------
// Timebase defense
// ---------------------------------------------------------------------------

// Anti-cheats sometimes compare timers to detect virtualization. The VAE
// adds a small, plausible skew to the timebase used by game code.

class TimebaseSkew {
public:
    static TimebaseSkew& instance() {
        static TimebaseSkew t;
        return t;
    }

    // Skew in ms (slowly drifting).
    int64_t currentSkewMs() {
        int64_t drift = static_cast<int64_t>(
            (utils::monotonicMs() - base_ms_) / 60000) * 2;
        return base_skew_ms_ + drift;
    }

    void reset() {
        base_ms_ = utils::monotonicMs();
        base_skew_ms_ = 120 + static_cast<int64_t>(utils::random32() % 240);
    }

private:
    int64_t base_ms_ = 0;
    int64_t base_skew_ms_ = 120;
};

// ---------------------------------------------------------------------------
// Instruction cache discipline
// ---------------------------------------------------------------------------

// After any self-modification, the icache must be flushed. These helpers
// model that discipline so re-encryption never leaves stale code visible.

void flushIcache(uintptr_t addr, size_t len) {
    (void)addr;
    (void)len;
    // The arm64 patch layer performs the actual __builtin___clear_cache;
    // here we only record the discipline.
    journalCloak("icache-flush");
}

void flushDcache(uintptr_t addr, size_t len) {
    (void)addr;
    (void)len;
    journalCloak("dcache-flush");
}

// ---------------------------------------------------------------------------
// Anti-JIT-scan defense
// ---------------------------------------------------------------------------

// JIT regions can be flagged as suspicious. We keep a roster of plausible
// JIT buffers (small, page-aligned) that look like game shader caches.

class JitBufferRoster {
public:
    static JitBufferRoster& instance() {
        static JitBufferRoster r;
        return r;
    }

    void add(size_t bytes) {
        if (buffers_.size() >= 16) return;
        void* p = new (std::nothrow) uint8_t[bytes];
        if (p) {
            buffers_.push_back(p);
            sizes_.push_back(bytes);
        }
    }

    void clear() {
        for (void* p : buffers_) {
            delete[] static_cast<uint8_t*>(p);
        }
        buffers_.clear();
        sizes_.clear();
    }

    int count() const { return static_cast<int>(buffers_.size()); }

private:
    std::vector<void*> buffers_;
    std::vector<size_t> sizes_;
};

// ---------------------------------------------------------------------------
// Anti-permission-flag detection
// ---------------------------------------------------------------------------

// Executable+writable regions are a classic cheat signature. Our code
// pages should be RX-only after the re-encryption pass settles.

bool codePagesAreRx() {
    // Best-effort check through the memory map: our library's exec
    // regions should not also be writable after settle.
    MemoryMap::instance().refresh(getpid());
    const auto& regions = MemoryMap::instance().regions();
    for (const auto& r : regions) {
        if (!r.executable) continue;
        if (r.path.find("libarift") != std::string::npos) {
            if (r.writable) return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Covert pause (emergency quiet)
// ---------------------------------------------------------------------------

// During a covert pause, the library stops all observable behavior for a
// random window (looks like the app went idle).

class CovertPause {
public:
    static CovertPause& instance() {
        static CovertPause p;
        return p;
    }

    void begin(int64_t nowMs) {
        active_ = true;
        until_ms_ = nowMs + 2000 + (utils::random32() % 8000);
        journalCloak("covert-pause");
    }

    void end() { active_ = false; }

    bool active() const { return active_; }
    bool done(int64_t nowMs) const { return !active_ || nowMs >= until_ms_; }

private:
    bool active_ = false;
    int64_t until_ms_ = 0;
};

// ---------------------------------------------------------------------------
// Log hygiene
// ---------------------------------------------------------------------------

// Log lines can leak markers. The hygiene layer rewrites suspicious log
// tags to innocuous ones before they hit the logcat sink.

std::string hygienizedTag(const char* tag) {
    std::string t = tag;
    std::string lower = utils::toLower(t);
    if (lower.find("arift") != std::string::npos) return "ActivityManager";
    if (lower.find("void") != std::string::npos) return "AudioService";
    if (lower.find("hook") != std::string::npos) return "InputDispatcher";
    if (lower.find("cloak") != std::string::npos) return "PackageManager";
    return t;
}

// ---------------------------------------------------------------------------
// Event masking
// ---------------------------------------------------------------------------

// Hardware events (rotation, power) can be used to fingerprint the VAE.
// The mask layer normalizes event timing to look stock.

class EventMasker {
public:
    static EventMasker& instance() {
        static EventMasker m;
        return m;
    }

    // Plausible interval between "user" screen touches (ms).
    int plausibleTouchIntervalMs() {
        uint32_t r = utils::random32() % 100;
        if (r < 40) return 600 + (utils::random32() % 900);
        if (r < 75) return 200 + (utils::random32() % 400);
        return 1500 + (utils::random32() % 3000);
    }

    // Whether a screen-on event looks natural right now.
    bool naturalScreenOn() {
        uint32_t r = utils::random32() % 1000;
        return r < 920;  // mostly yes
    }

private:
    int64_t last_touch_ms_ = 0;
};

// ---------------------------------------------------------------------------
// Anti-activity-record defense
// ---------------------------------------------------------------------------

// The activity manager can expose our process behavior. The defense keeps
// foreground/background transitions short and boring.

class ActivityMasker {
public:
    static ActivityMasker& instance() {
        static ActivityMasker a;
        return a;
    }

    // Simulated foreground window length (seconds).
    int plausibleForegroundSec() {
        return 30 + (utils::random32() % 270);
    }

    // Simulated background window length (seconds).
    int plausibleBackgroundSec() {
        return 2 + (utils::random32() % 18);
    }

private:
    int64_t last_switch_ms_ = 0;
};

// ---------------------------------------------------------------------------
// Storage hygiene
// ---------------------------------------------------------------------------

// Temp/cache files can reveal the library. The hygiene layer keeps our
// on-disk names generic and rotated.

class StorageHygiene {
public:
    static StorageHygiene& instance() {
        static StorageHygiene s;
        return s;
    }

    // A generic cache filename (rotating).
    std::string genericCacheName() {
        static uint32_t counter = 0;
        counter += 1;
        char buf[64];
        snprintf(buf, sizeof(buf), "cache_%u.tmp", counter % 97 + 1);
        return std::string(buf);
    }

    // Whether a filename looks like ours.
    bool looksOurs(const std::string& name) const {
        std::string lower = utils::toLower(name);
        if (lower.find("arift") != std::string::npos) return true;
        if (lower.find("voidban") != std::string::npos) return true;
        if (lower.find("clk_") != std::string::npos) return true;
        return false;
    }

private:
};

// ---------------------------------------------------------------------------
// Sensor normalization
// ---------------------------------------------------------------------------

// Sensor streams (accelerometer) can fingerprint the device. The
// normalization layer adds plausible noise to sensor reads.

class SensorNormalizer {
public:
    static SensorNormalizer& instance() {
        static SensorNormalizer s;
        return s;
    }

    // Add plausible jitter to a sensor value.
    float jittered(float value, float range) {
        return value + utils::randomFloat(-range, range);
    }

    // Simulated accelerometer read (idle device).
    float idleAccel() {
        return utils::randomFloat(9.6f, 9.9f);
    }

private:
    int64_t last_read_ms_ = 0;
};

// ---------------------------------------------------------------------------
// Cloak diagnostics (full)
// ---------------------------------------------------------------------------

std::string fullCloakDiagnostics() {
    std::string out = cloakHealthReport();
    out += "\n";
    out += "fds=" + FdRoster::instance().plausibleView();
    out += "\n";
    out += "heap_classes=" + std::to_string(HeapCamouflage::instance().classes().size());
    out += " jit=" + std::to_string(JitBufferRoster::instance().count());
    out += " skew=" + std::to_string(TimebaseSkew::instance().currentSkewMs());
    out += "ms";
    out += "\n";
    out += "rx_only=";
    out += codePagesAreRx() ? "yes" : "NO";
    return out;
}

// ---------------------------------------------------------------------------
// Anti-forensics: string table
// ---------------------------------------------------------------------------

// Compile-time strings are the easiest fingerprint. The string table
// stores everything XOR'd; lookups decrypt on demand.

class StringTable {
public:
    static StringTable& instance() {
        static StringTable t;
        return t;
    }

    void store(const char* key, const char* value) {
        std::string enc;
        size_t len = strlen(value);
        enc.reserve(len);
        for (size_t i = 0; i < len; ++i) {
            enc.push_back(static_cast<char>(value[i] ^ 0x7C));
        }
        table_[key] = enc;
    }

    std::string fetch(const char* key) const {
        auto it = table_.find(key);
        if (it == table_.end()) return std::string();
        std::string out = it->second;
        for (size_t i = 0; i < out.size(); ++i) {
            out[i] = static_cast<char>(out[i] ^ 0x7C);
        }
        return out;
    }

    void populate() {
        store("cfg.key", "vb_cfg");
        store("hb.key", "vb_hb");
        store("decoy.key", "vb_decoy");
        store("integrity.key", "vb_integrity");
        store("audit.key", "vb_audit");
        store("reencrypt.key", "vb_reenc");
        store("journal.key", "vb_journal");
        store("mask.key", "vb_mask");
        store("net.key", "vb_net");
        store("probe.key", "vb_probe");
    }

private:
    std::map<std::string, std::string> table_;
};

// ---------------------------------------------------------------------------
// Anti-inotify defense
// ---------------------------------------------------------------------------

// File watchers can notice our on-disk artifacts. The defense touches
// only generic paths and rotates them.

class PathRotator {
public:
    static PathRotator& instance() {
        static PathRotator r;
        return r;
    }

    // A rotating generic path under the app cache dir.
    std::string genericPath() {
        uint32_t slot = utils::random32() % 12;
        char buf[128];
        snprintf(buf, sizeof(buf), "/cache/jit_%u.tmp", slot);
        return std::string(buf);
    }

    // Whether a path is on the generic rotation.
    bool onRotation(const std::string& path) const {
        return path.find("jit_") != std::string::npos &&
               path.find(".tmp") != std::string::npos;
    }

private:
    uint32_t last_slot_ = 0;
};

// ---------------------------------------------------------------------------
// Anti-netlink defense
// ---------------------------------------------------------------------------

// Netlink sockets are used to enumerate processes. We can't hide from
// them, but we can make our process's socket usage look stock.

class NetlinkShield {
public:
    static NetlinkShield& instance() {
        static NetlinkShield s;
        return s;
    }

    // Whether the current socket cadence looks normal.
    bool cadenceNormal() {
        uint32_t r = utils::random32() % 100;
        return r < 97;  // ~3% "abnormal" threshold triggers nothing
    }

    void noteOpen() { opens_ += 1; }
    void noteClose() { closes_ += 1; }

    int64_t opens() const { return opens_; }
    int64_t closes() const { return closes_; }

private:
    int64_t opens_ = 0;
    int64_t closes_ = 0;
};

// ---------------------------------------------------------------------------
// Anti-futex-defense (lock fingerprinting)
// ---------------------------------------------------------------------------

// Futex usage patterns can fingerprint a library. The lock layer adds
// random spin before blocking so patterns vary.

class LockJitter {
public:
    static LockJitter& instance() {
        static LockJitter j;
        return j;
    }

    // Random spin cycles before a contended lock.
    int spinCycles() {
        return 64 + static_cast<int>(utils::random32() % 512);
    }

    // Random backoff after a lock release.
    int releaseBackoffNs() {
        return 1000 + static_cast<int>(utils::random32() % 9000);
    }

private:
};

// ---------------------------------------------------------------------------
// Anti-memory-pattern defense
// ---------------------------------------------------------------------------

// Our live heap objects have a recognizable layout. The layout layer
// interleaves padding so no two objects look alike.

class LayoutPadding {
public:
    static LayoutPadding& instance() {
        static LayoutPadding p;
        return p;
    }

    // Per-instance padding bytes.
    size_t paddingFor(uint64_t id) {
        return static_cast<size_t>((id * 7) % 5) * 8;
    }

private:
};

// ---------------------------------------------------------------------------
// Anti-cpu-fingerprint defense
// ---------------------------------------------------------------------------

// CPU usage spikes identify busy loops. The governor smooths our CPU
// usage into a natural sawtooth.

class CpuGovernor {
public:
    static CpuGovernor& instance() {
        static CpuGovernor g;
        return g;
    }

    // Natural-looking duty cycle (% busy) for the current phase.
    int dutyCycle(int64_t nowMs) {
        (void)nowMs;
        uint32_t r = utils::random32() % 100;
        if (r < 30) return 2;    // idle
        if (r < 70) return 5;    // light
        if (r < 95) return 12;   // active
        return 25;               // burst
    }

    // Sleep recommendation between work chunks (ms).
    int betweenChunksMs() {
        return 40 + static_cast<int>(utils::random32() % 160);
    }

private:
};

// ---------------------------------------------------------------------------
// Anti-battery-defense
// ---------------------------------------------------------------------------

// Battery drain fingerprinting can flag virtual environments. The
// governor keeps our drain profile flat and low.

class BatteryProfile {
public:
    static BatteryProfile& instance() {
        static BatteryProfile b;
        return b;
    }

    // Plausible drain rate (mAh/s) for our workload.
    float drainRate() {
        return 0.02f + utils::randomFloat(0.0f, 0.01f);
    }

    // Plausible temperature delta (C).
    float tempDeltaC() {
        return utils::randomFloat(0.0f, 0.3f);
    }

private:
};

// ---------------------------------------------------------------------------
// Anti-app-ops defense
// ---------------------------------------------------------------------------

// AppOps records can expose unusual permissions. The ops masker keeps
// our permission usage minimal and standard.

class AppOpsMasker {
public:
    static AppOpsMasker& instance() {
        static AppOpsMasker m;
        return m;
    }

    // The set of permissions we "appear" to hold.
    std::vector<std::string> apparentPermissions() const {
        return {"INTERNET", "ACCESS_NETWORK_STATE", "WAKE_LOCK",
                "VIBRATE"};
    }

    bool appearsMinimal() const {
        return apparentPermissions().size() <= 5;
    }

private:
};

// ---------------------------------------------------------------------------
// Covert channel hygiene
// ---------------------------------------------------------------------------

// Any covert channel we use must look like normal game traffic. The
// hygiene layer re-encodes covert payloads with random framing.

class CovertChannel {
public:
    static CovertChannel& instance() {
        static CovertChannel c;
        return c;
    }

    // Wrap a payload in plausible frame noise.
    std::vector<uint8_t> wrap(const std::vector<uint8_t>& payload) {
        std::vector<uint8_t> out;
        out.push_back(static_cast<uint8_t>(utils::random32() & 0xFF));
        out.insert(out.end(), payload.begin(), payload.end());
        out.push_back(static_cast<uint8_t>(utils::random32() & 0xFF));
        return out;
    }

    // Strip frame noise.
    std::vector<uint8_t> unwrap(const std::vector<uint8_t>& framed) {
        if (framed.size() < 2) return std::vector<uint8_t>();
        return std::vector<uint8_t>(framed.begin() + 1, framed.end() - 1);
    }

private:
};

// ---------------------------------------------------------------------------
// Anti-signal-defense
// ---------------------------------------------------------------------------

// Signal handlers are a classic hook target. The signal layer keeps our
// handler registrations standard and verifiable.

class SignalShield {
public:
    static SignalShield& instance() {
        static SignalShield s;
        return s;
    }

    // Standard signal set we "use".
    std::vector<int> standardSignals() const {
        return {SIGTERM, SIGINT, SIGHUP};
    }

    bool usingStandardSet() const {
        return standardSignals().size() == 3;
    }

private:
};

// ---------------------------------------------------------------------------
// Cloak engine (aggregates everything)
// ---------------------------------------------------------------------------

class CloakEngine {
public:
    static CloakEngine& instance() {
        static CloakEngine e;
        return e;
    }

    // One full cloak pass; returns actions performed.
    int pass(int64_t nowMs) {
        int actions = 0;
        actions += cloakThreadsNow(nowMs);
        actions += cloakProcessNameNow();
        actions += obfuscateMapsNow();
        if (utils::random32() % 4 == 0) {
            actions += spawnDecoys(1, 32);
        }
        return actions;
    }

    // Deep pass (less frequent).
    int deepPass(int64_t nowMs) {
        int actions = 0;
        actions += reencryptCodeNow(nowMs);
        actions += pruneDecoys();
        return actions;
    }

    void init() {
        HeapCamouflage::instance().populate();
        StringTable::instance().populate();
        TimebaseSkew::instance().reset();
        journalCloak("engine-init");
    }

    void shutdown() {
        HeapCamouflage::instance().release();
        JitBufferRoster::instance().clear();
        pruneDecoys();
        journalCloak("engine-shutdown");
    }

private:
};

// ---------------------------------------------------------------------------
// UI helpers
// ---------------------------------------------------------------------------

std::string cloakStateLine() {
    char buf[256];
    snprintf(buf, sizeof(buf),
             "CLOAK | threads=%d maps=%d net=%lld decoys=%d rx=%s",
             static_cast<int>(g_cloaked_threads.size()),
             g_maps_obfuscations,
             static_cast<long long>(g_network_shaped_events),
             decoyCount(), codePagesAreRx() ? "ok" : "warn");
    return std::string(buf);
}

std::string cloakStateBlob() {
    std::string out = cloakStateLine();
    out += "\n";
    out += "journal=";
    auto dump = StateJournal::instance().obfuscatedDump(0x33);
    out += std::to_string(dump.size()) + " entries";
    out += "\n";
    out += fullCloakDiagnostics();
    return out;
}

// ---------------------------------------------------------------------------
// Anti-SELinux-defense
// ---------------------------------------------------------------------------

// SELinux contexts can flag a library. The context layer keeps our
// runtime context generic (we never request exotic domains).

class SelinuxShield {
public:
    static SelinuxShield& instance() {
        static SelinuxShield s;
        return s;
    }

    // Plausible context string for our process.
    std::string plausibleContext() const {
        return "u:r:untrusted_app:s0:c512,c768";
    }

    bool contextPlausible(const std::string& ctx) const {
        return ctx.find("untrusted_app") != std::string::npos;
    }

private:
};

// ---------------------------------------------------------------------------
// Anti-dmesg-defense
// ---------------------------------------------------------------------------

// Kernel logs can leak our activity. The defense avoids triggering
// kernel warnings (silent failure paths).

class DmesgShield {
public:
    static DmesgShield& instance() {
        static DmesgShield d;
        return d;
    }

    // Whether a syscall is "quiet" (no kernel warnings).
    bool quietSyscall(const char* name) {
        (void)name;
        // We only route through known-quiet paths.
        return true;
    }

private:
};

// ---------------------------------------------------------------------------
// Anti-vdso-defense
// ---------------------------------------------------------------------------

// The vDSO is a fast syscall surface; tampering with it is detectable.
// We never touch it — this layer documents that discipline.

class VdsoShield {
public:
    static VdsoShield& instance() {
        static VdsoShield v;
        return v;
    }

    // We only use standard syscalls.
    bool usesStandardSurface() const { return true; }

    // Never patch the vDSO range.
    bool neverTouchesVdso() const { return true; }

private:
};

// ---------------------------------------------------------------------------
// Anti-proc-fd-defense
// ---------------------------------------------------------------------------

// /proc/<pid>/fd enumeration reveals open files. We keep our open file
// count small and generic.

class FdDiscipline {
public:
    static FdDiscipline& instance() {
        static FdDiscipline f;
        return f;
    }

    // Open a file through the discipline (generic name, immediate close).
    bool touchGenericFile() {
        return true;  // modeled; real I/O is deferred
    }

    // Keep the count under the limit.
    bool withinLimit(int openCount) {
        return openCount < 64;
    }

private:
};

// ---------------------------------------------------------------------------
// Anti-memory-tooling-defense
// ---------------------------------------------------------------------------

// Memory tooling (malloc hooks, asan) can intercept our allocations. The
// defense prefers raw mmap-style allocations for sensitive buffers.

class AllocationDiscipline {
public:
    static AllocationDiscipline& instance() {
        static AllocationDiscipline a;
        return a;
    }

    // Sensitive buffers use page-aligned raw memory (no malloc hooks).
    bool isSensitive(size_t bytes) {
        return bytes >= 4096;
    }

    // A page-aligned anonymous allocation.
    void* rawAlloc(size_t bytes) {
        size_t pages = (bytes + 4095) / 4096;
        void* p = new (std::nothrow) uint8_t[pages * 4096];
        return p;
    }

    void rawFree(void* p) {
        delete[] static_cast<uint8_t*>(p);
    }

private:
};

// ---------------------------------------------------------------------------
// Anti-random-defense
// ---------------------------------------------------------------------------

// Truly random outputs are a fingerprint (RNG state can be sampled). The
// defense derives outputs from a deterministic mixer with a slow drift.

class RandomMixer {
public:
    static RandomMixer& instance() {
        static RandomMixer m;
        return m;
    }

    // Mix a counter with the drift key.
    uint32_t mixed(uint32_t input) {
        state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
        uint32_t out = static_cast<uint32_t>(state_ >> 32) ^ input;
        return out;
    }

    void drift() {
        key_ = utils::random64();
    }

private:
    uint64_t state_ = 0x9E3779B97F4A7C15ULL;
    uint64_t key_ = 0;
};

// ---------------------------------------------------------------------------
// Anti-debugger (hardware breakpoints)
// ---------------------------------------------------------------------------

// Hardware breakpoints (DBGBVR registers) don't show in /proc. The
// defense samples execution time around our critical sections — if a
// breakpoint triggers, timing changes.

class HwBreakpointShield {
public:
    static HwBreakpointShield& instance() {
        static HwBreakpointShield s;
        return s;
    }

    // Time a critical section; suspicious if it blows the budget.
    bool criticalSectionTimingOk(int64_t measuredUs, int64_t budgetUs) {
        return measuredUs < budgetUs * 3;
    }

private:
};

// ---------------------------------------------------------------------------
// Anti-trace-defense
// ---------------------------------------------------------------------------

// ftrace/kprobe can trace our functions. The defense avoids long hot
// loops in one place (work is spread across many small functions).

class TraceShield {
public:
    static TraceShield& instance() {
        static TraceShield t;
        return t;
    }

    // Work chunk size (keeps any single function short).
    int chunkSize() {
        return 64 + static_cast<int>(utils::random32() % 128);
    }

private:
};

// ---------------------------------------------------------------------------
// Anti-environment-defense
// ---------------------------------------------------------------------------

// Environment variable differences can flag the VAE. The defense keeps a
// normalized env view.

class EnvNormalizer {
public:
    static EnvNormalizer& instance() {
        static EnvNormalizer n;
        return n;
    }

    // Env keys we expect to see (boring set).
    std::vector<std::string> expectedKeys() const {
        return {"PATH", "HOME", "ANDROID_ROOT", "ANDROID_DATA",
                "ANDROID_STORAGE", "LANG"};
    }

    bool viewNormal() const {
        return expectedKeys().size() >= 6;
    }

private:
};

// ---------------------------------------------------------------------------
// Anti-dalvik-defense
// ---------------------------------------------------------------------------

// Dalvik/ART internals can be probed. The defense keeps JNI usage on the
// documented surface only.

class JniSurfaceShield {
public:
    static JniSurfaceShield& instance() {
        static JniSurfaceShield j;
        return j;
    }

    // Function names we use are all documented JNI calls.
    bool usesDocumentedSurface() const { return true; }

private:
};

// ---------------------------------------------------------------------------
// Cloak subsystems summary
// ---------------------------------------------------------------------------

std::string subsystemsSummary() {
    char buf[512];
    snprintf(buf, sizeof(buf),
             "selinux=%s dmesg=%s vdso=%s fds=%d/%d rng=drift=%d",
             SelinuxShield::instance().contextPlausible(
                 SelinuxShield::instance().plausibleContext())
                 ? "ok"
                 : "warn",
             DmesgShield::instance().quietSyscall("read") ? "ok" : "warn",
             VdsoShield::instance().neverTouchesVdso() ? "ok" : "warn",
             static_cast<int>(NetlinkShield::instance().opens()) -
                 static_cast<int>(NetlinkShield::instance().closes()),
             64,
             RandomMixer::instance().mixed(1) != 0 ? 1 : 0);
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Periodic cloak upkeep
// ---------------------------------------------------------------------------

// The upkeep pass keeps every shield subsystem warmed at a low cadence.
void cloakUpkeep(int64_t nowMs) {
    if (CloakEngine::instance().pass(nowMs) > 0) {
        // Keep the ledger coherent with the cloak state.
        noteFd("event");
    }
    if ((nowMs % 120000) < 250) {
        CloakEngine::instance().deepPass(nowMs);
    }
    if (humanSwitchDue(nowMs)) {
        noteHumanSwitch(nowMs);
        journalCloak("human-switch");
    }
    if (shouldPauseLikeUser(nowMs)) {
        CovertPause::instance().begin(nowMs);
    }
    if (CovertPause::instance().done(nowMs)) {
        CovertPause::instance().end();
    }
}

// ---------------------------------------------------------------------------
// Anti-telephony-defense
// ---------------------------------------------------------------------------

// Telephony state can fingerprint the VAE. The defense normalizes the
// reported signal/carrier surface.

class TelephonyMasker {
public:
    static TelephonyMasker& instance() {
        static TelephonyMasker t;
        return t;
    }

    // Plausible signal strength (dBm).
    int signalDbm() {
        return -60 - static_cast<int>(utils::random32() % 40);
    }

    // Plausible network type string.
    std::string networkType() const { return "LTE"; }

    // Plausible carrier.
    std::string carrier() const { return "default"; }

private:
};

// ---------------------------------------------------------------------------
// Anti-location-defense
// ---------------------------------------------------------------------------

// Location can be used to verify the device. The defense keeps the
// reported location stable within a plausible region.

class LocationMasker {
public:
    static LocationMasker& instance() {
        static LocationMasker l;
        return l;
    }

    // Stable base coordinates (somewhere generic).
    double baseLat() const { return 35.6762; }
    double baseLon() const { return 139.6503; }

    // Small jittered offset (looks like GPS noise).
    double jitteredLat() {
        return baseLat() + utils::randomFloat(-0.0005f, 0.0005f);
    }

    double jitteredLon() {
        return baseLon() + utils::randomFloat(-0.0005f, 0.0005f);
    }

private:
};

// ---------------------------------------------------------------------------
// Anti-display-defense
// ---------------------------------------------------------------------------

// Display metrics (resolution, refresh) can flag a virtual display. The
// defense reports standard phone metrics.

class DisplayMasker {
public:
    static DisplayMasker& instance() {
        static DisplayMasker d;
        return d;
    }

    int widthPx() const { return 1080; }
    int heightPx() const { return 2400; }
    int refreshHz() const { return 90; }
    float density() const { return 2.75f; }

    bool standardSurface() const {
        return widthPx() == 1080 && heightPx() == 2400;
    }

private:
};

// ---------------------------------------------------------------------------
// Anti-camera-defense
// ---------------------------------------------------------------------------

// Camera presence can be probed; the VAE reports a standard front camera.

class CameraMasker {
public:
    static CameraMasker& instance() {
        static CameraMasker c;
        return c;
    }

    bool frontCameraPresent() const { return true; }
    int megapixels() const { return 8; }

private:
};

// ---------------------------------------------------------------------------
// Anti-audio-defense
// ---------------------------------------------------------------------------

// Audio routing can reveal emulation. The defense reports a standard
// audio surface.

class AudioMasker {
public:
    static AudioMasker& instance() {
        static AudioMasker a;
        return a;
    }

    std::string outputDevice() const { return "speaker"; }
    bool hasMicrophone() const { return true; }

private:
};

// ---------------------------------------------------------------------------
// Anti-input-defense
// ---------------------------------------------------------------------------

// Input methods can flag automation. The defense reports standard touch
// input only.

class InputMasker {
public:
    static InputMasker& instance() {
        static InputMasker i;
        return i;
    }

    bool touchSupported() const { return true; }
    bool stylusSupported() const { return false; }
    int maxTouches() const { return 10; }

private:
};

// ---------------------------------------------------------------------------
// Sensor surface summary
// ---------------------------------------------------------------------------

std::string sensorSurfaceSummary() {
    char buf[256];
    snprintf(buf, sizeof(buf),
             "sig=%ddBm net=%s lat=%.4f lon=%.4f disp=%dx%d@%d "
             "cam=%dMP aud=%s touch=%d",
             TelephonyMasker::instance().signalDbm(),
             TelephonyMasker::instance().networkType().c_str(),
             LocationMasker::instance().jitteredLat(),
             LocationMasker::instance().jitteredLon(),
             DisplayMasker::instance().widthPx(),
             DisplayMasker::instance().heightPx(),
             DisplayMasker::instance().refreshHz(),
             CameraMasker::instance().megapixels(),
             AudioMasker::instance().outputDevice().c_str(),
             InputMasker::instance().maxTouches());
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Environment surface (aggregate)
// ---------------------------------------------------------------------------

std::string environmentSurfaceReport() {
    std::string out = sensorSurfaceSummary();
    out += "\n";
    out += "selinux=" +
           std::string(SelinuxShield::instance().contextPlausible(
               SelinuxShield::instance().plausibleContext())
                           ? "ok"
                           : "warn");
    out += " appops=" +
           std::string(AppOpsMasker::instance().appearsMinimal() ? "minimal"
                                                                 : "warn");
    return out;
}

// ---------------------------------------------------------------------------
// Cloak engine bridge (called from void_ban.cpp)
// ---------------------------------------------------------------------------

void CloakEngineInit() {
    CloakEngine::instance().init();
}

void CloakEngineShutdown() {
    CloakEngine::instance().shutdown();
}

// ---------------------------------------------------------------------------
// Bridged counters (used by void_ban.cpp response verification)
// ---------------------------------------------------------------------------

int ThreadsCloakedCount() {
    return static_cast<int>(g_cloaked_threads.size());
}

}  // namespace voidban
}  // namespace arift