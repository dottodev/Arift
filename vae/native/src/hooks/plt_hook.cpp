#include "plt_hook.h"

#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <sys/mman.h>

#ifndef RTLD_DI_LINKMAP
#define RTLD_DI_LINKMAP 2
#endif

#include <cstring>

#include "arift_log.h"
#include "arift_utils.h"

namespace arift {

namespace {

uintptr_t alignDown(uintptr_t v, uintptr_t align) {
    return v & ~(align - 1);
}

}  // namespace

PltHook::PltEntry* PltHook::findSlot(const std::string& libPath,
                                     const std::string& symbol) {
    static std::map<std::string, PltEntry> cache;

    std::string key = libPath + "::" + symbol;
    auto it = cache.find(key);
    if (it != cache.end()) return &it->second;

    void* handle = dlopen(libPath.c_str(), RTLD_LAZY | RTLD_NOLOAD);
    if (!handle) {
        ARIFT_WARN(kTagHooks, "dlopen(%s) failed: %s", libPath.c_str(), dlerror());
        return nullptr;
    }
    dlclose(handle);

    // Locate the dynamic section of the loaded image via dl_iterate_phdr.
    struct DynCtx {
        const char* want;
        const ElfW(Dyn)* dyn = nullptr;
    };
    DynCtx dctx;
    dctx.want = libPath.c_str();
    dl_iterate_phdr(
        [](dl_phdr_info* info, size_t size, void* data) -> int {
            (void)size;
            auto* ctx = static_cast<DynCtx*>(data);
            if (!info->dlpi_name) return 0;
            const char* got = strrchr(info->dlpi_name, '/');
            got = got ? got + 1 : info->dlpi_name;
            const char* want = strrchr(ctx->want, '/');
            want = want ? want + 1 : ctx->want;
            if (strcmp(got, want) != 0) return 0;
            for (uint16_t i = 0; i < info->dlpi_phnum; ++i) {
                const ElfW(Phdr)* ph = info->dlpi_phdr + i;
                if (ph->p_type == PT_DYNAMIC) {
                    ctx->dyn = reinterpret_cast<const ElfW(Dyn)*>(
                        info->dlpi_addr + ph->p_vaddr);
                    break;
                }
            }
            return 1;
        },
        &dctx);
    const ElfW(Dyn)* dyn = dctx.dyn;
    if (!dyn) return nullptr;

    const char* dynstr = nullptr;
    const ElfW(Sym)* dynsym = nullptr;
    ElfW(Xword) dynsymCount = 0;
    const ElfW(Rel)* jmprel = nullptr;
    ElfW(Xword) jmprelSize = 0;
    const ElfW(Rela)* jmprela = nullptr;
    ElfW(Xword) jmprelaSize = 0;

    for (const ElfW(Dyn)* d = dyn; d->d_tag != DT_NULL; ++d) {
        switch (d->d_tag) {
            case DT_STRTAB: dynstr = reinterpret_cast<const char*>(d->d_un.d_ptr); break;
            case DT_SYMTAB: dynsym = reinterpret_cast<const ElfW(Sym)*>(d->d_un.d_ptr); break;
            case DT_SYMENT: break;
            case DT_JMPREL: jmprel = reinterpret_cast<const ElfW(Rel)*>(d->d_un.d_ptr); break;
            case DT_PLTRELSZ: jmprelSize = d->d_un.d_val; break;
            case DT_RELA: jmprela = reinterpret_cast<const ElfW(Rela)*>(d->d_un.d_ptr); break;
            case DT_RELASZ: jmprelaSize = d->d_un.d_val; break;
            case DT_PLTREL:
                // value == DT_REL or DT_RELA
                if (d->d_un.d_val == DT_RELA) {
                    jmprel = reinterpret_cast<const ElfW(Rel)*>(d->d_un.d_ptr);
                }
                break;
            default: break;
        }
    }

    if (!dynsym || !dynstr || !jmprel) {
        ARIFT_WARN(kTagHooks, "No dynsym/jmprel in %s", libPath.c_str());
        return nullptr;
    }

    size_t relCount = jmprelSize / sizeof(ElfW(Rel));
    for (size_t i = 0; i < relCount; ++i) {
        const ElfW(Rel)& rel = jmprel[i];
        if (ELF64_R_TYPE(rel.r_info) != R_AARCH64_JUMP_SLOT) continue;
        uint32_t symIdx = ELF64_R_SYM(rel.r_info);
        const char* symName = dynstr + dynsym[symIdx].st_name;
        if (strcmp(symName, symbol.c_str()) != 0) continue;
        PltEntry entry;
        entry.symbol = symbol;
        entry.gotAddr = static_cast<uintptr_t>(rel.r_offset);
        entry.original = *reinterpret_cast<uintptr_t*>(entry.gotAddr);
        cache[key] = entry;
        ARIFT_DEBUG(kTagHooks, "PLT slot for %s at %s (orig %s)",
                    symbol.c_str(), utils::hex(entry.gotAddr).c_str(),
                    utils::hex(entry.original).c_str());
        return &cache[key];
    }

    ARIFT_WARN(kTagHooks, "Symbol %s not found in %s", symbol.c_str(), libPath.c_str());
    return nullptr;
}

bool PltHook::hook(uintptr_t gotAddr, uintptr_t replacement,
                   uintptr_t* outOriginal) {
    uintptr_t page = alignDown(gotAddr, 4096);
    if (mprotect(reinterpret_cast<void*>(page), 4096, PROT_READ | PROT_WRITE) != 0) {
        ARIFT_ERROR(kTagHooks, "mprotect failed for GOT slot %s",
                    utils::hex(gotAddr).c_str());
        return false;
    }
    auto* slot = reinterpret_cast<uintptr_t*>(gotAddr);
    uintptr_t orig = *slot;
    *slot = replacement;
    if (outOriginal) *outOriginal = orig;
    return true;
}

bool PltHook::unhook(uintptr_t gotAddr, uintptr_t original) {
    auto* slot = reinterpret_cast<uintptr_t*>(gotAddr);
    *slot = original;
    return true;
}

uintptr_t PltHook::findGotSlotInElf(const uint8_t* elfData, size_t elfSize,
                                    uintptr_t moduleBase,
                                    const std::string& symbol) {
    if (!elfData || elfSize < sizeof(ElfW(Ehdr))) return 0;
    const auto* ehdr = reinterpret_cast<const ElfW(Ehdr)*>(elfData);
    if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0) return 0;

    const auto* shdr = reinterpret_cast<const ElfW(Shdr)*>(
        elfData + ehdr->e_shoff);
    size_t shnum = ehdr->e_shnum;
    const char* shstr = nullptr;
    for (size_t i = 0; i < shnum; ++i) {
        if (shdr[i].sh_type == SHT_STRTAB && i != ehdr->e_shstrndx) continue;
        if (shdr[i].sh_type == SHT_STRTAB) {
            shstr = reinterpret_cast<const char*>(elfData + shdr[i].sh_offset);
        }
    }
    if (!shstr) return 0;

    for (size_t i = 0; i < shnum; ++i) {
        const char* name = shstr + shdr[i].sh_name;
        if (strcmp(name, ".dynsym") != 0) continue;
        const auto* dynsym = reinterpret_cast<const ElfW(Sym)*>(
            elfData + shdr[i].sh_offset);
        size_t symCount = shdr[i].sh_size / sizeof(ElfW(Sym));
        // Find .dynstr
        const char* dynstr = nullptr;
        for (size_t j = 0; j < shnum; ++j) {
            const char* n2 = shstr + shdr[j].sh_name;
            if (strcmp(n2, ".dynstr") == 0) {
                dynstr = reinterpret_cast<const char*>(elfData + shdr[j].sh_offset);
                break;
            }
        }
        if (!dynstr) return 0;
        // Find .got
        uintptr_t gotAddr = 0;
        for (size_t j = 0; j < shnum; ++j) {
            const char* n3 = shstr + shdr[j].sh_name;
            if (strcmp(n3, ".got") == 0 || strcmp(n3, ".got.plt") == 0) {
                gotAddr = moduleBase + shdr[j].sh_addr;
            }
        }
        if (!gotAddr) return 0;
        // Find .rela.plt
        for (size_t j = 0; j < shnum; ++j) {
            const char* n4 = shstr + shdr[j].sh_name;
            if (strcmp(n4, ".rela.plt") != 0) continue;
            const auto* rela = reinterpret_cast<const ElfW(Rela)*>(
                elfData + shdr[j].sh_offset);
            size_t relCount = shdr[j].sh_size / sizeof(ElfW(Rela));
            for (size_t r = 0; r < relCount; ++r) {
                uint32_t symIdx = ELF64_R_SYM(rela[r].r_info);
                if (symIdx >= symCount) continue;
                const char* symName = dynstr + dynsym[symIdx].st_name;
                if (strcmp(symName, symbol.c_str()) == 0) {
                    return moduleBase + rela[r].r_offset;
                }
            }
        }
    }
    return 0;
}

std::map<uintptr_t, PltHook::PltEntry>& PltHook::activeHooks() {
    static std::map<uintptr_t, PltEntry> hooks;
    return hooks;
}

}  // namespace arift