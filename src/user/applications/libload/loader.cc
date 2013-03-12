#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

#include <string>
#include <list>
#include <map>
#include <set>

#define PACKED __attribute__((packed))

#define _NO_ELF_CLASS
#include <Elf.h>

#define STB_LOCAL       0
#define STB_GLOBAL      1
#define STB_WEAK        2
#define STB_LOPROC     13
#define STB_HIPROC     15

typedef void (*entry_point_t)(char*[], char **);

typedef struct _object_meta {
    std::string filename;
    std::string path;
    entry_point_t entry;

    void *mapped_file;
    size_t mapped_file_sz;

    bool relocated;
    uintptr_t load_base;

    bool running;

    std::list<std::pair<void*, size_t> > memory_regions;

    ElfProgramHeader_t *phdrs;
    ElfSectionHeader_t *shdrs;

    ElfSectionHeader_t *sh_symtab;
    ElfSectionHeader_t *sh_strtab;

    ElfSymbol_t *symtab;
    const char *strtab;

    ElfSectionHeader_t *sh_shstrtab;
    const char *shstrtab;

    ElfProgramHeader_t *ph_dynamic;

    std::list<std::string> needed;

    ElfSymbol_t *dyn_symtab;
    const char *dyn_strtab;
    size_t dyn_strtab_sz;

    ElfRela_t *rela;
    ElfRel_t *rel;
    size_t rela_sz;
    size_t rel_sz;

    bool uses_rela;

    uintptr_t *got;

    ElfRela_t *plt_rela;
    ElfRel_t *plt_rel;

    uintptr_t init_func;
    uintptr_t fini_func;

    size_t plt_sz;

    ElfHash_t *hash;
    Elf_Word *hash_buckets;
    Elf_Word *hash_chains;

    std::list<struct _object_meta*> preloads;
    std::list<struct _object_meta*> objects;

    struct _object_meta *parent;
} object_meta_t;

#define IS_NOT_PAGE_ALIGNED(x) (((x) & (getpagesize() - 1)) != 0)

extern "C" void *pedigree_sys_request_mem(size_t len);

bool loadObject(const char *filename, object_meta_t *meta);

bool loadSharedObjectHelper(const char *filename, object_meta_t *parent);

bool findSymbol(const char *symbol, object_meta_t *meta, ElfSymbol_t &sym);

bool lookupSymbol(const char *symbol, object_meta_t *meta, ElfSymbol_t &sym, bool bWeak);

void doRelocation(object_meta_t *meta);

uintptr_t doThisRelocation(ElfRel_t rel, object_meta_t *meta);
uintptr_t doThisRelocation(ElfRela_t rel, object_meta_t *meta);

std::string symbolName(const ElfSymbol_t &sym, object_meta_t *meta);

std::string findObject(std::string name);

extern "C" uintptr_t _libload_resolve_symbol();

std::list<std::string> g_lSearchPaths;

std::set<std::string> g_LoadedObjects;

size_t elfhash(const char *name) {
    size_t h = 0, g = 0;
    while(*name) {
        h = (h << 4) + *name++;
        g = h & 0xF0000000;
        h ^= g;
        h ^= g >> 24;
    }

    return h;
}

extern char **environ;

#include <syslog.h>

extern "C" int main(int argc, char *argv[])
{
    // Sanity check: do we actually have a program to load?
    if(argc == 0) {
        return 0;
    }

    syslog(LOG_INFO, "libload.so starting...");

    char *ld_libpath = getenv("LD_LIBRARY_PATH");
    char *ld_preload = getenv("LD_PRELOAD");
    char *ld_debug = getenv("LD_DEBUG");

    g_lSearchPaths.push_back(std::string("/libraries"));
    g_lSearchPaths.push_back(std::string("."));

    if(ld_libpath) {
        // Parse, write.
        const char *entry;
        while((entry = strtok(ld_libpath, ":"))) {
            g_lSearchPaths.push_back(std::string(entry));
        }
    }

    if(ld_debug) {
        fprintf(stderr, "libload.so: search path is\n");
        for(std::list<std::string>::iterator it = g_lSearchPaths.begin();
            it != g_lSearchPaths.end();
            ++it) {
            printf(" -> %s\n", it->c_str());
        }
    }

    /// \todo Implement dlopen etc in here.

    syslog(LOG_INFO, "libload.so loading main object");

    // Load the main object passed on the command line.
    object_meta_t *meta = new object_meta_t;
    meta->running = false;
    if(!loadObject(argv[0], meta)) {
        delete meta;
        return ENOEXEC;
    }

    g_LoadedObjects.insert(meta->filename);

    syslog(LOG_INFO, "libload.so loading preload, if one exists");

    // Preload?
    if(ld_preload) {
        object_meta_t *preload = new object_meta_t;
        if(!loadObject(ld_preload, preload)) {
            printf("Loading preload '%s' failed.\n", ld_preload);
        } else {
            preload->parent = meta;
            meta->preloads.push_back(preload);

            g_LoadedObjects.insert(preload->filename);
        }
    }

    syslog(LOG_INFO, "libload.so loading dependencies");

    // Any libraries to load?
    if(meta->needed.size()) {
        for(std::list<std::string>::iterator it = meta->needed.begin();
            it != meta->needed.end();
            ++it) {
            if(g_LoadedObjects.find(*it) == g_LoadedObjects.end())
                loadSharedObjectHelper(it->c_str(), meta);
        }
    }

    syslog(LOG_INFO, "libload.so relocating dependencies");

    // Relocate preloads.
    for(std::list<struct _object_meta *>::iterator it = meta->preloads.begin();
        it != meta->preloads.end();
        ++it) {
        doRelocation(*it);
    }

    // Relocate all other loaded objects.
    for(std::list<struct _object_meta *>::iterator it = meta->objects.begin();
        it != meta->objects.end();
        ++it) {
        doRelocation(*it);
    }

    syslog(LOG_INFO, "libload.so relocating main object");

    // Do initial relocation of the binary (non-GOT entries)
    doRelocation(meta);

    syslog(LOG_INFO, "libload.so running entry point");

    // All done - run the program!
    meta->running = true;
    meta->entry(argv, environ);

    return 0;
}

std::string findObject(std::string name) {
    std::string fixed_path = name;
    std::list<std::string>::iterator it = g_lSearchPaths.begin();
    do {
        struct stat st;
        int l = stat(fixed_path.c_str(), &st);
        if(l == 0) {
            return fixed_path;
        }

        fixed_path = *it;
        fixed_path += "/";
        fixed_path += name;
    } while(++it != g_lSearchPaths.end());

    return std::string("<not found>");
}

bool loadSharedObjectHelper(const char *filename, object_meta_t *parent) {
    object_meta_t *object = new object_meta_t;
    if(!loadObject(filename, object)) {
        printf("Loading '%s' failed.\n", filename);
    } else {
        object->parent = parent;
        parent->objects.push_back(object);
        g_LoadedObjects.insert(object->filename);

        if(object->needed.size()) {
            for(std::list<std::string>::iterator it = object->needed.begin();
                it != object->needed.end();
                ++it) {
                if(g_LoadedObjects.find(*it) == g_LoadedObjects.end())
                    return loadSharedObjectHelper(it->c_str(), parent);
            }
        }
    }

    return true;
}

#include <syslog.h>
bool loadObject(const char *filename, object_meta_t *meta) {
    meta->filename = filename;
    meta->path = findObject(meta->filename);

    // Okay, let's open up the file for reading...
    int fd = open(meta->path.c_str(), O_RDONLY);
    if(fd < 0) {
        fprintf(stderr, "libload.so: couldn't load object '%s' (%s) (%s)\n", filename, meta->path.c_str(), strerror(errno));
        return false;
    }

    // Check header.
    ElfHeader_t header;
    int n = read(fd, &header, sizeof(header));
    if(n < 0) {
        close(fd);
        return false;
    } else if(n != sizeof(header)) {
        close(fd);
        errno = ENOEXEC;
        return false;
    }

    if(header.ident[1] != 'E' ||
       header.ident[2] != 'L' ||
       header.ident[3] != 'F' ||
       header.ident[0] != 127) {
        close(fd);
        errno = ENOEXEC;
        return false;
    }

    // Fairly confident we have an ELF binary now. Valid class?
    if(!(header.ident[4] == 1 || header.ident[4] == 2)) {
        close(fd);
        errno = ENOEXEC;
        return false;
    }

    meta->entry = (entry_point_t) header.entry;

    // Grab the size of the file - we'll mmap the entire thing, and pull out what we want.
    struct stat st;
    fstat(fd, &st);

    meta->mapped_file_sz = st.st_size;

    const char *pBuffer = (const char *) mmap(0, meta->mapped_file_sz, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    if(pBuffer == MAP_FAILED) {
        close(fd);
        errno = ENOEXEC;
        return false;
    }
    meta->mapped_file = (void *) pBuffer;

    meta->phdrs = (ElfProgramHeader_t *) &pBuffer[header.phoff];
    meta->shdrs = (ElfSectionHeader_t *) &pBuffer[header.shoff];

    if(header.type == ET_REL) {
        meta->relocated = true;
    } else if(header.type == ET_EXEC || header.type == ET_DYN) {
        // First program header zero?
        if(header.phnum) {
            meta->relocated = (meta->phdrs[0].vaddr & ~(getpagesize() - 1)) == 0;
        }
    }

    meta->sh_shstrtab = &meta->shdrs[header.shstrndx];
    meta->shstrtab = (const char *) &pBuffer[meta->sh_shstrtab->offset];

    // Find the symbol and string tables (these are not the dynamic ones).
    meta->sh_symtab = 0;
    meta->sh_strtab = 0;
    for(int i = 0; i < header.shnum; i++) {
        const char *name = meta->shstrtab + meta->shdrs[i].name;
        if(meta->shdrs[i].type == SHT_SYMTAB && !strcmp(name, ".symtab")) {
            meta->sh_symtab = &meta->shdrs[i];
        } else if(meta->shdrs[i].type == SHT_STRTAB && !strcmp(name, ".strtab")) {
            meta->sh_strtab = &meta->shdrs[i];
        }
    }

    meta->symtab = 0;
    meta->strtab = 0;

    if(meta->sh_symtab != 0) {
        meta->symtab = (ElfSymbol_t *) &pBuffer[meta->sh_symtab->offset];
    }

    if(meta->sh_strtab != 0) {
        meta->strtab = (const char *) &pBuffer[meta->sh_strtab->offset];
    }

    // Load program headers.
    meta->load_base = 0;
    if(header.phnum) {
        if(meta->relocated) {
            // Reserve space for the full loaded virtual address space of the process.
            meta->load_base = meta->phdrs[0].vaddr & ~(getpagesize() - 1);
            uintptr_t finalAddress = 0;
            for(size_t i = 0; i < header.phnum; ++i) {
                uintptr_t endAddr = meta->phdrs[i].vaddr + meta->phdrs[i].memsz;
                finalAddress = std::max(finalAddress, endAddr);
            }
            size_t mapSize = finalAddress - meta->load_base;
            if(mapSize & (getpagesize() - 1)) {
                mapSize = (mapSize + getpagesize()) & ~(getpagesize() - 1);
            }

            void *p = pedigree_sys_request_mem(mapSize);
            if(!p) {
                munmap((void *) pBuffer, meta->mapped_file_sz);
                errno = ENOEXEC;
                return false;
            }

            meta->load_base = (uintptr_t) p;
            if(meta->load_base & (getpagesize() - 1)) {
                meta->load_base = (meta->load_base + getpagesize()) & ~(getpagesize() - 1);
            }

            // Patch up section headers, quickly.
            for(size_t shdx = 0; shdx < header.shnum; ++shdx) {
                meta->shdrs[shdx].addr += meta->load_base;
            }
        }

        // NEEDED libraries are stored as offsets into the dynamic string table.
        std::list<uintptr_t> tmp_needed;

        for(size_t i = 0; i < header.phnum; i++) {
            if(meta->phdrs[i].type == PT_DYNAMIC) {
                meta->ph_dynamic = &meta->phdrs[i];
                if(meta->relocated) {
                    meta->ph_dynamic->vaddr += meta->load_base;
                }

                ElfDyn_t *dyn = (ElfDyn_t *) &pBuffer[meta->phdrs[i].offset];

                while(dyn->tag != DT_NULL) {
                    switch(dyn->tag) {
                        case DT_NEEDED:
                            tmp_needed.push_back(dyn->un.ptr);
                            break;
                        case DT_SYMTAB:
                            meta->dyn_symtab = (ElfSymbol_t *) dyn->un.ptr;
                            break;
                        case DT_STRTAB:
                            meta->dyn_strtab = (const char *) dyn->un.ptr;
                            break;
                        case DT_STRSZ:
                            meta->dyn_strtab_sz = dyn->un.val;
                            break;
                        case DT_RELA:
                            meta->rela = (ElfRela_t *) dyn->un.ptr;
                            break;
                        case DT_REL:
                            meta->rel = (ElfRel_t *) dyn->un.ptr;
                            break;
                        case DT_RELASZ:
                            meta->rela_sz = dyn->un.val;
                            break;
                        case DT_RELSZ:
                            meta->rel_sz = dyn->un.val;
                            break;
                        case DT_PLTGOT:
                            meta->got = (uintptr_t *) dyn->un.ptr;
                            break;
                        case DT_JMPREL:
                            if(meta->uses_rela) {
                                meta->plt_rela = (ElfRela_t *) dyn->un.ptr;
                            } else {
                                meta->plt_rel = (ElfRel_t *) dyn->un.ptr;
                            }
                            break;
                        case DT_PLTREL:
                            meta->uses_rela = dyn->un.val == DT_RELA;
                            break;
                        case DT_PLTRELSZ:
                            meta->plt_sz = dyn->un.val;
                            break;
                        case DT_INIT:
                            meta->init_func = dyn->un.val;
                            break;
                        case DT_FINI:
                            meta->fini_func = dyn->un.val;
                            break;
                    }
                    dyn++;
                }
            } else if(meta->phdrs[i].type == PT_LOAD) {
                // Loadable data - use the flags to determine how we'll mmap.
                int flags = PROT_READ;
                if(meta->phdrs[i].flags & PF_X)
                    flags |= PROT_EXEC;
                if(meta->phdrs[i].flags & PF_W)
                    flags |= PROT_WRITE;
                if((meta->phdrs[i].flags & PF_R) == 0)
                    flags &= ~PROT_READ;

                if(meta->relocated) {
                    meta->phdrs[i].vaddr += meta->load_base;
                }

                uintptr_t phdr_base = meta->phdrs[i].vaddr;

                size_t pagesz = getpagesize();

                size_t base_addend = phdr_base & (pagesz - 1);
                phdr_base &= ~(pagesz - 1);

                size_t offset = meta->phdrs[i].offset & ~(pagesz - 1);
                size_t offset_addend = meta->phdrs[i].offset & (pagesz - 1);

                size_t mapsz = offset_addend + meta->phdrs[i].memsz;

                // Already mapped?
                if((msync((void *) phdr_base, mapsz, MS_SYNC) != 0) && (errno == ENOMEM)) {
                    int mapflags = MAP_ANON | MAP_FIXED;
                    if(meta->relocated) {
                        mapflags |= MAP_USERSVD;
                    }
                    void *p = mmap((void *) phdr_base, mapsz, PROT_READ | PROT_WRITE, mapflags, 0, 0);
                    if(p == MAP_FAILED) {
                        /// \todo cleanup.
                        errno = ENOEXEC;
                        return false;
                    }

                    // It'd be nice to fully mmap the file, but Pedigree's mmap is not very good.
                    memcpy((void *) meta->phdrs[i].vaddr, &pBuffer[meta->phdrs[i].offset], meta->phdrs[i].filesz);
                    meta->memory_regions.push_back(std::pair<void *, size_t>(p, mapsz));
                }

                if(meta->phdrs[i].memsz > meta->phdrs[i].filesz) {
                    uintptr_t vaddr = meta->phdrs[i].vaddr + meta->phdrs[i].filesz;
                    memset((void *) vaddr, 0, meta->phdrs[i].memsz - meta->phdrs[i].filesz);
                }

                // mprotect accordingly.
                size_t alignExtra = meta->phdrs[i].vaddr & (getpagesize() - 1);
                uintptr_t protectaddr = meta->phdrs[i].vaddr & ~(getpagesize() - 1);
                mprotect((void *) protectaddr, meta->phdrs[i].memsz + alignExtra, flags);
            }
        }

        if(meta->relocated) {
            uintptr_t base_vaddr = meta->load_base;

            // Patch up references.
            if(meta->dyn_strtab)
                meta->dyn_strtab += base_vaddr;
            if(meta->dyn_symtab)
                meta->dyn_symtab = (ElfSymbol_t *) (((uintptr_t) meta->dyn_symtab) + base_vaddr);
            if(meta->got)
                meta->got = (uintptr_t *) (((uintptr_t) meta->got) + base_vaddr);
            if(meta->rela)
                meta->rela = (ElfRela_t *) (((uintptr_t) meta->rela) + base_vaddr);
            if(meta->rel)
                meta->rel = (ElfRel_t *) (((uintptr_t) meta->rel) + base_vaddr);
            if(meta->plt_rela)
                meta->plt_rela = (ElfRela_t *) (((uintptr_t) meta->plt_rela) + base_vaddr);
            if(meta->plt_rel)
                meta->plt_rel = (ElfRel_t *) (((uintptr_t) meta->plt_rel) + base_vaddr);
        }

        if(meta->dyn_strtab) {
            for(std::list<uintptr_t>::iterator it = tmp_needed.begin();
                it != tmp_needed.end();
                ++it) {
                std::string s(meta->dyn_strtab + *it);
                meta->needed.push_back(s);
                it = tmp_needed.erase(it);
            }
        }
    } else {
        meta->phdrs = 0;
    }

    // Do another pass over section headers to try and get the hash table.
    meta->hash = 0;
    meta->hash_buckets = 0;
    meta->hash_chains = 0;
    for(size_t i = 0; i < header.shnum; i++) {
        if(meta->shdrs[i].type == SHT_HASH) {
            uintptr_t vaddr = meta->shdrs[meta->shdrs[i].link].addr;
            if(((uintptr_t) meta->dyn_symtab) == vaddr) {
                meta->hash = (ElfHash_t *) &pBuffer[meta->shdrs[i].offset];
                meta->hash_buckets = (Elf_Word *) &pBuffer[meta->shdrs[i].offset + sizeof(ElfHash_t)];
                meta->hash_chains = (Elf_Word *) &pBuffer[meta->shdrs[i].offset + sizeof(ElfHash_t) + (sizeof(Elf_Word) * meta->hash->nbucket)];
            }
        }
    }

    // Patch up the GOT so we can start resolving symbols when needed.
    if(meta->got) {
        meta->got[1] = (uintptr_t) meta;
        meta->got[2] = (uintptr_t) _libload_resolve_symbol;
    }

    // mmap complete - don't need the file descriptors open any longer.
    close(fd);

    return true;
}

bool lookupSymbol(const char *symbol, object_meta_t *meta, ElfSymbol_t &sym, bool bWeak) {
    if(!meta) {
        return false;
    }

    // Allow preloads to override the main object symbol table, as well as any others.
    for(std::list<object_meta_t*>::iterator it = meta->preloads.begin();
        it != meta->preloads.end();
        ++it) {
        if(lookupSymbol(symbol, *it, sym, false))
            return true;
    }

    std::string sname(symbol);

    size_t hash = elfhash(symbol);
    size_t y = meta->hash_buckets[hash % meta->hash->nbucket];
    if(y > meta->hash->nchain) {
        return false;
    }

    do {
        sym = meta->dyn_symtab[y];
        if(symbolName(sym, meta) == sname) {
            if(ST_BIND(sym.info) == STB_GLOBAL || ST_BIND(sym.info) == STB_LOCAL) {
                if(sym.shndx) {
                    break;
                }
            }
            if(bWeak) {
                if(ST_BIND(sym.info) == STB_WEAK) {
                    sym.value = (uintptr_t) ~0UL;
                    break;
                }
            }
        }
        y = meta->hash_chains[y];
    } while(y != 0);

    if(y != 0) {
        // Patch up the value.
        if(ST_TYPE(sym.info) < 3 && ST_BIND(sym.info) != STB_WEAK) {
            if(sym.shndx && meta->relocated) {
                ElfSectionHeader_t *sh = &meta->shdrs[sym.shndx];
                sym.value += meta->load_base;
            }
        }
    }

    return y != 0;
}

bool findSymbol(const char *symbol, object_meta_t *meta, ElfSymbol_t &sym) {
    if(!meta) {
        return false;
    }

    for(std::list<object_meta_t*>::iterator it = meta->preloads.begin();
        it != meta->preloads.end();
        ++it) {
        if(lookupSymbol(symbol, *it, sym, false))
            return true;
    }

    object_meta_t *ext_meta = meta;
    while(ext_meta->parent) {
        ext_meta = ext_meta->parent;
    }

    for(std::list<object_meta_t*>::iterator it = ext_meta->objects.begin();
        it != ext_meta->objects.end();
        ++it) {
        if(lookupSymbol(symbol, *it, sym, false))
            return true;
    }

    // No luck? Try weak symbols in the main object.
    if(lookupSymbol(symbol, meta, sym, true))
        return true;

    return false;
}

std::string symbolName(const ElfSymbol_t &sym, object_meta_t *meta) {
    if(!meta) {
        return std::string("");
    } else if(sym.name == 0) {
        return std::string("");
    }

    ElfSymbol_t *symtab = meta->symtab;
    const char *strtab = meta->strtab;
    if(meta->dyn_symtab) {
        symtab = meta->dyn_symtab;
    }

    if(ST_TYPE(sym.info) == 3) {
        strtab = meta->shstrtab;
    } else if(meta->dyn_strtab) {
        strtab = meta->dyn_strtab;
    }

    const char *name = strtab + sym.name;
    return std::string(name);
}

void doRelocation(object_meta_t *meta) {
    if(meta->rel) {
        for(size_t i = 0; i < (meta->rel_sz / sizeof(ElfRel_t)); i++) {
            doThisRelocation(meta->rel[i], meta);
        }
    }

    if(meta->rela) {
        for(size_t i = 0; i < (meta->rela_sz / sizeof(ElfRela_t)); i++) {
            doThisRelocation(meta->rela[i], meta);
        }
    }

    // Relocated binaries need to have the GOTPLT fixed up, as each entry points to
    // a non-relocated address (that is also not relative).
    if(meta->relocated) {
        uintptr_t base = meta->load_base;

        if(meta->plt_rel) {
            for(size_t i = 0; i < (meta->plt_sz / sizeof(ElfRel_t)); i++) {
                uintptr_t *addr = (uintptr_t *) (base + meta->plt_rel[i].offset);
                *addr += base;
            }
        }

        if(meta->plt_rela) {
            for(size_t i = 0; i < (meta->plt_sz / sizeof(ElfRela_t)); i++) {
                uintptr_t *addr = (uintptr_t *) (base + meta->plt_rel[i].offset);
                *addr += base;
            }
        }
    }
}

#define R_X86_64_NONE       0
#define R_X86_64_64         1
#define R_X86_64_PC32       2
#define R_X86_64_GOT32      3
#define R_X86_64_PLT32      4
#define R_X86_64_COPY       5
#define R_X86_64_GLOB_DAT   6
#define R_X86_64_JUMP_SLOT  7
#define R_X86_64_RELATIVE   8
#define R_X86_64_GOTPCREL   9
#define R_X86_64_32         10
#define R_X86_64_32S        11
#define R_X86_64_PC64       24
#define R_X86_64_GOTOFF64   25
#define R_X86_64_GOTPC32    26
#define R_X86_64_GOT64      27
#define R_X86_64_GOTPCREL64 28
#define R_X86_64_GOTPC64    29
#define R_X86_64_GOTPLT64   30
#define R_X86_64_PLTOFF64   31

#define R_386_NONE     0
#define R_386_32       1
#define R_386_PC32     2
#define R_386_GOT32    3
#define R_386_PLT32    4
#define R_386_COPY     5
#define R_386_GLOB_DAT 6
#define R_386_JMP_SLOT 7
#define R_386_RELATIVE 8
#define R_386_GOTOFF   9
#define R_386_GOTPC    10

uintptr_t doThisRelocation(ElfRel_t rel, object_meta_t *meta) {
    ElfSymbol_t *symtab = meta->symtab;
    if(meta->dyn_symtab) {
        symtab = meta->dyn_symtab;
    }

    ElfSymbol_t *sym = &symtab[R_SYM(rel.info)];
    ElfSectionHeader_t *sh = 0;
    if(sym->shndx) {
        sh = &meta->shdrs[sym->shndx];
    }

    uintptr_t B = meta->load_base;
    uintptr_t P = rel.offset;
    if(meta->relocated) {
        P += B;
    }

    uintptr_t A = *((uintptr_t*) P);
    uintptr_t S = 0;

    std::string symbolname = symbolName(*sym, meta);

    // Patch in section header?
    if(symtab && ST_TYPE(sym->info) == 3) {
        S = sh->addr;
    } else if(R_TYPE(rel.info) != R_386_RELATIVE) {
        if(sym->name == 0) {
            S = sym->value;
        } else {
            ElfSymbol_t lookupsym;
            if(R_TYPE(rel.info) == R_386_COPY) {
                // Search anything except the current object.
                if(!findSymbol(symbolname.c_str(), meta, lookupsym)) {
                    printf("symbol lookup for '%s' failed.\n", symbolname.c_str());
                    lookupsym.value = (uintptr_t) ~0UL;
                }
            } else {
                // Attempt a local lookup first.
                if(!lookupSymbol(symbolname.c_str(), meta, lookupsym, false)) {
                    // No local symbol of that name - search other objects.
                    if(!findSymbol(symbolname.c_str(), meta, lookupsym)) {
                        printf("symbol lookup for '%s' (needed in '%s') failed.\n", symbolname.c_str(), meta->path.c_str());
                        lookupsym.value = (uintptr_t) ~0UL;
                    }
                }
            }

            S = lookupsym.value;
        }
    }

    if(S == (uintptr_t) ~0UL)
        S = 0;

    uint32_t result = A;
    switch(R_TYPE(rel.info)) {
        case R_386_NONE:
            break;
        case R_386_32:
            result = S + A;
            break;
        case R_386_PC32:
            result = S + A - P;
            break;
        case R_386_JMP_SLOT:
        case R_386_GLOB_DAT:
            result = S;
            break;
        case R_386_COPY:
            result = *((uint32_t *) S);
            break;
        case R_386_RELATIVE:
            result = B + A;
            break;
    }

    *((uint32_t *) P) = result;
    return result;
}

uintptr_t doThisRelocation(ElfRela_t rel, object_meta_t *meta) {
    ElfSymbol_t *symtab = meta->symtab;
    const char *strtab = meta->strtab;
    if(meta->dyn_symtab) {
        symtab = meta->dyn_symtab;
    }
    if(meta->dyn_strtab) {
        strtab = meta->dyn_strtab;
    }

    ElfSymbol_t *sym = &symtab[R_SYM(rel.info)];
    ElfSectionHeader_t *sh = 0;
    if(sym->shndx) {
        sh = &meta->shdrs[sym->shndx];
    }

    uintptr_t A = rel.addend;
    uintptr_t S = 0;
    uintptr_t B = 0;
    uintptr_t P = rel.offset;
    if(meta->relocated) {
        P += (sh ? sh->addr : B);
    }

    std::string symbolname = symbolName(*sym, meta);

    // Patch in section header?
    if(symtab && ST_TYPE(sym->info) == 3) {
        S = sh->addr;
    } else if(R_TYPE(rel.info) != R_X86_64_RELATIVE) {
        if(sym->name == 0) {
            S = sym->value;
        } else {
            ElfSymbol_t lookupsym;
            lookupsym.value = 0;
            if(R_TYPE(rel.info) == R_X86_64_COPY) {
                // Search anything except the current object.
                if(!findSymbol(symbolname.c_str(), meta, lookupsym)) {
                    printf("symbol lookup for '%s' failed.\n", symbolname.c_str());
                    lookupsym.value = (uintptr_t) ~0UL;
                }
            } else {
                // Attempt a local lookup first.
                if(!lookupSymbol(symbolname.c_str(), meta, lookupsym, false)) {
                    // No local symbol of that name - search other objects.
                    if(!findSymbol(symbolname.c_str(), meta, lookupsym)) {
                        printf("symbol lookup for '%s' (needed in '%s') failed.\n", symbolname.c_str(), meta->path.c_str());
                        lookupsym.value = (uintptr_t) ~0UL;
                    }
                }
            }

            S = lookupsym.value;
        }
    }

    // Valid S?
    if((S == 0) && (R_TYPE(rel.info) != R_X86_64_RELATIVE)) {
        return (uintptr_t) ~0UL;
    }

    // Weak symbol.
    if(S == (uintptr_t) ~0UL) {
        S = 0;
    }

    uintptr_t result = *((uintptr_t *) P);
    switch(R_TYPE(rel.info)) {
        case R_X86_64_NONE:
            break;
        case R_X86_64_64:
            result = S + A;
            break;
        case R_X86_64_PC32:
            result = (result & 0xFFFFFFFF00000000) | ((S + A - P) & 0xFFFFFFFF);
            break;
        case R_X86_64_COPY:
            result = *((uintptr_t *) S);
            break;
        case R_X86_64_JUMP_SLOT:
        case R_X86_64_GLOB_DAT:
            result = S;
            break;
        case R_X86_64_RELATIVE:
            result = B + A;
            break;
        case R_X86_64_32:
        case R_X86_64_32S:
            result = (result & 0xFFFFFFFF00000000) | ((S + A) & 0xFFFFFFFF);
            break;
    }
    *((uintptr_t *) P) = result;
    return result;
}

extern "C" uintptr_t _libload_dofixup(uintptr_t id, uintptr_t symbol) {
    object_meta_t *meta = (object_meta_t *) id;
    uintptr_t returnaddr = 0;

#ifdef BITS_32
    ElfRel_t rel = meta->plt_rel[symbol / sizeof(ElfRel_t)];
#else
    ElfRela_t rel = meta->plt_rela[symbol / sizeof(ElfRela_t)];
#endif
    // Verify the symbol is sane.
    if(meta->hash && (R_SYM(rel.info) > meta->hash->nchain)) {
        fprintf(stderr, "symbol lookup failed (symbol not in hash table)\n");
        abort();
    }

    ElfSymbol_t *sym = &meta->dyn_symtab[R_SYM(rel.info)];
    std::string symbolname = symbolName(*sym, meta);

    uintptr_t result = doThisRelocation(rel, meta);
    if(result == (uintptr_t) ~0UL) {
        fprintf(stderr, "symbol lookup failed (couldn't relocate)\n");
        abort();
    }

    return result;
}