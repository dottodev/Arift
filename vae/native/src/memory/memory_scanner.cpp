#include "memory_scanner.h"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <cstring>

#include "arift_log.h"
#include "arift_time.h"
#include "arift_utils.h"
#include "pattern_engine.h"

namespace arift {

// ---------------------------------------------------------------------------
// ProcessMemory
// ---------------------------------------------------------------------------

ProcessMemory::~ProcessMemory() {
    close();
}

bool ProcessMemory::open(int pid) {
    close();
    std::string path = "/proc/" + std::to_string(pid) + "/mem";
    fd_ = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd_ < 0) {
        last_errno_ = errno;
        ARIFT_WARN(kTagMemory, "open(%s) failed: %s", path.c_str(), strerror(errno));
        return false;
    }
    pid_ = pid;
    pageSize_ = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    if (pageSize_ == 0) pageSize_ = 4096;
    ARIFT_DEBUG(kTagMemory, "ProcessMemory open pid=%d", pid);
    return true;
}

void ProcessMemory::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    pid_ = -1;
}

bool ProcessMemory::read(uintptr_t addr, void* out, size_t len) {
    if (fd_ < 0) return false;
    ssize_t n = pread(fd_, out, len, static_cast<off_t>(addr));
    if (n != static_cast<ssize_t>(len)) {
        last_errno_ = errno;
        return false;
    }
    ++total_reads_;
    return true;
}

bool ProcessMemory::write(uintptr_t addr, const void* in, size_t len) {
    if (fd_ < 0) return false;
    ssize_t n = pwrite(fd_, in, len, static_cast<off_t>(addr));
    if (n != static_cast<ssize_t>(len)) {
        last_errno_ = errno;
        return false;
    }
    ++total_writes_;
    return true;
}

bool ProcessMemory::read8(uintptr_t addr, uint8_t& v) {
    return read(addr, &v, sizeof(v));
}

bool ProcessMemory::read16(uintptr_t addr, uint16_t& v) {
    return read(addr, &v, sizeof(v));
}

bool ProcessMemory::read32(uintptr_t addr, uint32_t& v) {
    return read(addr, &v, sizeof(v));
}

bool ProcessMemory::read64(uintptr_t addr, uint64_t& v) {
    return read(addr, &v, sizeof(v));
}

bool ProcessMemory::readFloat(uintptr_t addr, float& v) {
    return read(addr, &v, sizeof(v));
}

bool ProcessMemory::readDouble(uintptr_t addr, double& v) {
    return read(addr, &v, sizeof(v));
}

bool ProcessMemory::write8(uintptr_t addr, uint8_t v) {
    return write(addr, &v, sizeof(v));
}

bool ProcessMemory::write16(uintptr_t addr, uint16_t v) {
    return write(addr, &v, sizeof(v));
}

bool ProcessMemory::write32(uintptr_t addr, uint32_t v) {
    return write(addr, &v, sizeof(v));
}

bool ProcessMemory::write64(uintptr_t addr, uint64_t v) {
    return write(addr, &v, sizeof(v));
}

uintptr_t ProcessMemory::derefChain(uintptr_t base,
                                    const std::vector<int64_t>& offsets) {
    uintptr_t cur = base;
    for (size_t i = 0; i < offsets.size(); ++i) {
        uintptr_t target = cur + static_cast<uintptr_t>(offsets[i]);
        uint64_t next = 0;
        if (!read64(target, next)) return 0;
        cur = static_cast<uintptr_t>(next);
    }
    return cur;
}

std::string ProcessMemory::readCString(uintptr_t addr, size_t maxLen) {
    if (fd_ < 0 || maxLen == 0) return "";
    if (scratch_.size() < maxLen) scratch_.resize(maxLen);
    ssize_t n = pread(fd_, scratch_.data(), maxLen, static_cast<off_t>(addr));
    if (n <= 0) return "";
    ++total_reads_;
    size_t len = 0;
    while (len < static_cast<size_t>(n) && scratch_[len] != 0) ++len;
    return std::string(reinterpret_cast<const char*>(scratch_.data()), len);
}

bool ProcessMemory::readBytes(uintptr_t addr, std::vector<uint8_t>& out, size_t len) {
    out.resize(len);
    return read(addr, out.data(), len);
}

std::vector<ProcessMemory::BatchRead> ProcessMemory::batchRead(
    const std::vector<std::pair<uintptr_t, size_t>>& reads) {
    std::vector<BatchRead> out;
    out.reserve(reads.size());
    for (const auto& [addr, size] : reads) {
        BatchRead br;
        br.addr = addr;
        br.size = size;
        br.data.resize(size);
        br.ok = read(addr, br.data.data(), size);
        out.push_back(std::move(br));
    }
    return out;
}

size_t ProcessMemory::readRange(uintptr_t addr, uint8_t* out, size_t maxLen) {
    if (fd_ < 0 || maxLen == 0) return 0;
    size_t done = 0;
    while (done < maxLen) {
        size_t chunk = maxLen - done;
        if (chunk > pageSize_) chunk = pageSize_;
        ssize_t n = pread(fd_, out + done, chunk, static_cast<off_t>(addr + done));
        if (n <= 0) break;
        done += static_cast<size_t>(n);
        if (n < static_cast<ssize_t>(chunk)) break;
    }
    if (done) ++total_reads_;
    return done;
}

// ---------------------------------------------------------------------------
// MemoryScanner
// ---------------------------------------------------------------------------

MemoryScanner::MemoryScanner(const MemoryMap* map) : map_(map) {}

uint64_t MemoryScanner::scanRegions(
    const std::string& pathFilter,
    const std::function<bool(const MemRegion&, uintptr_t, const uint8_t*, size_t)>& cb,
    size_t pageSize) {
    if (!map_ || cb == nullptr) return 0;
    if (pageSize == 0) pageSize = 64 * 1024;

    Stopwatch sw;
    sw.start();
    uint64_t scanned = 0;
    std::vector<uint8_t> buffer(pageSize);

    for (const auto& region : map_->regions()) {
        if (!region.readable) continue;
        if (!pathFilter.empty() &&
            region.path.find(pathFilter) == std::string::npos) {
            continue;
        }
        uintptr_t addr = region.start;
        while (addr < region.end) {
            size_t chunk = static_cast<size_t>(region.end - addr);
            if (chunk > pageSize) chunk = pageSize;
            ProcessMemory mem;
            if (!mem.open(map_->pid())) return scanned;
            size_t got = mem.readRange(addr, buffer.data(), chunk);
            if (got == 0) {
                addr += chunk;
                continue;
            }
            if (!cb(region, addr, buffer.data(), got)) {
                return scanned;
            }
            scanned += got;
            addr += got;
        }
    }
    ARIFT_TRACE(kTagMemory, "scanRegions done: %llu bytes in %.1f ms",
                static_cast<unsigned long long>(scanned), sw.elapsedMs());
    return scanned;
}

std::vector<uintptr_t> MemoryScanner::findPatternAll(const std::string& pathFilter,
                                                     const Pattern& pattern,
                                                     size_t maxResults) {
    std::vector<uintptr_t> hits;
    if (pattern.size() == 0) return hits;
    scanRegions(pathFilter, [&](const MemRegion& region, uintptr_t addr,
                                const uint8_t* data, size_t len) {
        if (hits.size() >= maxResults) return false;
        auto found = PatternEngine::findAll(data, len, pattern,
                                            maxResults - hits.size());
        for (auto hit : found) {
            uintptr_t abs = addr + (hit - reinterpret_cast<uintptr_t>(data));
            hits.push_back(abs);
            if (hits.size() >= maxResults) return false;
        }
        return hits.size() < maxResults;
    });
    return hits;
}

uintptr_t MemoryScanner::findPatternInExec(const Pattern& pattern) {
    if (!map_) return 0;
    for (const auto& region : map_->regions()) {
        if (!region.executable) continue;
        ProcessMemory mem;
        if (!mem.open(map_->pid())) continue;
        std::vector<uint8_t> buffer(region.size > 8 * 1024 * 1024 ? 8 * 1024 * 1024
                                                                  : region.size);
        size_t read = mem.readRange(region.start, buffer.data(), buffer.size());
        if (read == 0) continue;
        uintptr_t hit = PatternEngine::findFirst(buffer.data(), read, pattern);
        if (hit) {
            return region.start + (hit - reinterpret_cast<uintptr_t>(buffer.data()));
        }
    }
    return 0;
}

std::vector<uintptr_t> MemoryScanner::findValue32(const std::string& pathFilter,
                                                  uint32_t value, size_t maxResults) {
    std::vector<uintptr_t> hits;
    scanRegions(pathFilter, [&](const MemRegion& region, uintptr_t addr,
                                const uint8_t* data, size_t len) {
        for (size_t i = 0; i + 4 <= len; ++i) {
            uint32_t v;
            memcpy(&v, data + i, 4);
            if (v == value) {
                hits.push_back(addr + i);
                if (hits.size() >= maxResults) return false;
            }
        }
        return hits.size() < maxResults;
    });
    return hits;
}

std::vector<uintptr_t> MemoryScanner::findValueFloat(const std::string& pathFilter,
                                                     float value, size_t maxResults) {
    uint32_t bits;
    memcpy(&bits, &value, 4);
    return findValue32(pathFilter, bits, maxResults);
}

std::vector<uintptr_t> MemoryScanner::findPointersTo(const std::string& pathFilter,
                                                     uintptr_t lo, uintptr_t hi,
                                                     size_t maxResults) {
    std::vector<uintptr_t> hits;
    scanRegions(pathFilter, [&](const MemRegion& region, uintptr_t addr,
                                const uint8_t* data, size_t len) {
        for (size_t i = 0; i + 8 <= len; i += 8) {
            uint64_t v;
            memcpy(&v, data + i, 8);
            if (v >= lo && v < hi) {
                hits.push_back(addr + i);
                if (hits.size() >= maxResults) return false;
            }
        }
        return hits.size() < maxResults;
    });
    return hits;
}

}  // namespace arift