#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "memory_map.h"

namespace arift {

// Wraps the /proc/<pid>/mem file with buffered I/O and safe address checks.
class ProcessMemory {
public:
    ProcessMemory() = default;
    ~ProcessMemory();

    bool open(int pid);
    void close();
    bool isOpen() const { return fd_ >= 0; }
    bool isReadOnly() const { return read_only_; }
    int pid() const { return pid_; }

    // Raw reads — return false on fault (unmapped page, EPERM, etc).
    bool read(uintptr_t addr, void* out, size_t len);
    bool write(uintptr_t addr, const void* in, size_t len);

    bool read8(uintptr_t addr, uint8_t& v);
    bool read16(uintptr_t addr, uint16_t& v);
    bool read32(uintptr_t addr, uint32_t& v);
    bool read64(uintptr_t addr, uint64_t& v);
    bool readFloat(uintptr_t addr, float& v);
    bool readDouble(uintptr_t addr, double& v);

    bool write8(uintptr_t addr, uint8_t v);
    bool write16(uintptr_t addr, uint16_t v);
    bool write32(uintptr_t addr, uint32_t v);
    bool write64(uintptr_t addr, uint64_t v);

    // Dereference pointer chains: read [base + off1] -> ptr -> [ptr + off2]...
    uintptr_t derefChain(uintptr_t base, const std::vector<int64_t>& offsets);

    // Read a NUL-terminated string up to maxLen.
    std::string readCString(uintptr_t addr, size_t maxLen = 128);

    // Read a fixed-size byte blob.
    bool readBytes(uintptr_t addr, std::vector<uint8_t>& out, size_t len);

    // Batch read of many addresses (best-effort).
    struct BatchRead {
        uintptr_t addr = 0;
        size_t size = 0;
        bool ok = false;
        std::vector<uint8_t> data;
    };
    std::vector<BatchRead> batchRead(const std::vector<std::pair<uintptr_t, size_t>>& reads);

    // Region-aware reads: auto-skip unmapped pages.
    size_t readRange(uintptr_t addr, uint8_t* out, size_t maxLen);

    int lastErrno() const { return last_errno_; }
    uint64_t totalReads() const { return total_reads_; }
    uint64_t totalWrites() const { return total_writes_; }

private:
    int fd_ = -1;
    int pid_ = -1;
    int last_errno_ = 0;
    bool read_only_ = false;
    uint64_t total_reads_ = 0;
    uint64_t total_writes_ = 0;
    size_t pageSize_ = 4096;
    std::vector<uint8_t> scratch_;
};

// High-level scanner: walks readable regions, applies a callback per page.
class MemoryScanner {
public:
    explicit MemoryScanner(const MemoryMap* map = nullptr);
    void setMap(const MemoryMap* map) { map_ = map; }

    // Scan all readable regions (or regions whose path matches `pathFilter`).
    // The callback receives (region, offsetInRegion, data, len) and returns
    // true to continue scanning this page.
    uint64_t scanRegions(
        const std::string& pathFilter,
        const std::function<bool(const MemRegion&, uintptr_t, const uint8_t*, size_t)>& cb,
        size_t pageSize = 64 * 1024);

    // Convenience: find all hits of a pattern across the whole address space.
    std::vector<uintptr_t> findPatternAll(const std::string& pathFilter,
                                          const class Pattern& pattern,
                                          size_t maxResults = 128);

    // Find the first hit within executable regions only.
    uintptr_t findPatternInExec(const class Pattern& pattern);

    // Scan for a 4-byte value (useful for object hunting).
    std::vector<uintptr_t> findValue32(const std::string& pathFilter,
                                       uint32_t value, size_t maxResults = 64);

    // Scan for a float value.
    std::vector<uintptr_t> findValueFloat(const std::string& pathFilter,
                                          float value, size_t maxResults = 64);

    // Pointer scan: find addresses whose first dereference lands in [lo, hi].
    std::vector<uintptr_t> findPointersTo(const std::string& pathFilter,
                                          uintptr_t lo, uintptr_t hi,
                                          size_t maxResults = 64);

    void setVerbose(bool v) { verbose_ = v; }

private:
    const MemoryMap* map_;
    bool verbose_ = false;
};

}  // namespace arift