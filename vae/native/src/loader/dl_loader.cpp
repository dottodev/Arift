#include "dl_loader.h"

#include <dlfcn.h>
#include <sys/mman.h>

#include <cstring>

#include "arift_log.h"
#include "arift_utils.h"

namespace arift {

namespace {

struct LoadedModuleStorage {
    DlLoader::LoadedModule mod;
};

bool mapSegments(const uint8_t* data, const ElfW(Ehdr)* ehdr,
                 uintptr_t& baseOut, size_t& sizeOut) {
    // Conservative approach: allocate a big RWX region and copy loadable
    // segments. A production build would do proper PT_LOAD mapping.
    const auto* phdr = reinterpret_cast<const ElfW(Phdr)*>(data + ehdr->e_phoff);
    uint64_t maxVaddr = 0;
    uint64_t minVaddr = UINT64_MAX;
    for (uint16_t i = 0; i < ehdr->e_phnum; ++i) {
        if (phdr[i].p_type != PT_LOAD) continue;
        if (phdr[i].p_vaddr < minVaddr) minVaddr = phdr[i].p_vaddr;
        uint64_t end = phdr[i].p_vaddr + phdr[i].p_memsz;
        if (end > maxVaddr) maxVaddr = end;
    }
    if (maxVaddr == 0) return false;

    size_t total = static_cast<size_t>(maxVaddr - minVaddr + 4095) & ~size_t(4095);
    void* mapped = mmap(nullptr, total + 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapped == MAP_FAILED) {
        ARIFT_ERROR(kTagLoader, "mmap failed for hidden module");
        return false;
    }
    uintptr_t base = reinterpret_cast<uintptr_t>(mapped) - minVaddr;

    for (uint16_t i = 0; i < ehdr->e_phnum; ++i) {
        if (phdr[i].p_type != PT_LOAD) continue;
        uintptr_t dst = base + phdr[i].p_vaddr;
        size_t filesz = static_cast<size_t>(phdr[i].p_filesz);
        memcpy(reinterpret_cast<void*>(dst), data + phdr[i].p_offset, filesz);
        size_t memsz = static_cast<size_t>(phdr[i].p_memsz);
        if (memsz > filesz) {
            memset(reinterpret_cast<void*>(dst + filesz), 0, memsz - filesz);
        }
    }

    baseOut = base;
    sizeOut = total;
    return true;
}

}  // namespace

std::map<std::string, DlLoader::LoadedModule>& DlLoader::registry() {
    static std::map<std::string, DlLoader::LoadedModule> reg;
    return reg;
}

DlLoader::LoadedModule* DlLoader::loadFromBuffer(const std::string& name,
                                                 const uint8_t* data, size_t size,
                                                 uintptr_t preferBase) {
    (void)preferBase;
    if (registry().count(name)) {
        ARIFT_WARN(kTagLoader, "Module %s already loaded", name.c_str());
        return &registry()[name];
    }

    const auto* ehdr = reinterpret_cast<const ElfW(Ehdr)*>(data);
    if (size < sizeof(ElfW(Ehdr)) || memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0) {
        ARIFT_ERROR(kTagLoader, "Invalid ELF buffer for %s", name.c_str());
        return nullptr;
    }

    uintptr_t base = 0;
    size_t mappedSize = 0;
    if (!mapSegments(data, ehdr, base, mappedSize)) return nullptr;

    LoadedModule mod;
    mod.name = name;
    mod.base = base;
    mod.size = mappedSize;
    if (!mod.parser.parse(data, size)) {
        ARIFT_WARN(kTagLoader, "Parser failed for %s", name.c_str());
    }
    mod.valid = true;
    registry()[name] = mod;

    ARIFT_INFO(kTagLoader, "Hidden module %s loaded at %s (size 0x%zx)",
               name.c_str(), utils::hex(base).c_str(), mappedSize);
    return &registry()[name];
}

DlLoader::LoadedModule* DlLoader::loadFromFile(const std::string& path,
                                               uintptr_t preferBase) {
    // Read the file into memory, then delegate.
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) {
        fclose(f);
        return nullptr;
    }
    std::vector<uint8_t> buf(static_cast<size_t>(len));
    size_t got = fread(buf.data(), 1, buf.size(), f);
    fclose(f);
    if (got != buf.size()) return nullptr;
    std::string name = path;
    size_t slash = name.find_last_of('/');
    if (slash != std::string::npos) name = name.substr(slash + 1);
    return loadFromBuffer(name, buf.data(), buf.size(), preferBase);
}

uintptr_t DlLoader::resolve(const LoadedModule* mod, const std::string& name) {
    if (!mod || !mod->valid) return 0;
    return mod->parser.resolveExport(name, mod->base);
}

DlLoader::LoadedModule* DlLoader::find(const std::string& name) {
    auto it = registry().find(name);
    return it == registry().end() ? nullptr : &it->second;
}

bool DlLoader::unload(LoadedModule* mod) {
    if (!mod || !mod->base) return false;
    registry().erase(mod->name);
    return munmap(reinterpret_cast<void*>(mod->base), mod->size) == 0;
}

size_t DlLoader::loadedCount() {
    return registry().size();
}

}  // namespace arift