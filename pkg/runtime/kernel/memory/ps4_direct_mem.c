#include "ps4_direct_mem.h"
#include "ps4_hitch.h"
#include "ps4_kernel_sys.h"
#include "nps5_thunk.h"
#include "nps5_libkernel_hle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <sys/mman.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif

typedef struct DirectMemBlock {
    off_t phys_offset;
    size_t size;
    int is_free;
    uint64_t mapped_guest_vaddr;
    struct DirectMemBlock *next;
    struct DirectMemBlock *prev;
} DirectMemBlock;

static DirectMemBlock *g_direct_blocks = NULL;
static pthread_mutex_t g_direct_mutex = PTHREAD_MUTEX_INITIALIZER;

// ---------------------------------------------------------------------------------
// Physical backing for direct memory. Every direct-memory page lives at
// g_phys_base + physAddr (a lazily committed host region); sceKernelMapDirectMemory
// aliases guest virtual ranges onto it (mach_vm_remap, shared), so the same physical
// pages can be mapped at several guest addresses, unmapped and re-mapped elsewhere
// with their contents intact (Naughty Dog's memory linearizer relies on this).
// ---------------------------------------------------------------------------------
typedef struct DirectMapping {
    uint64_t vaddr;
    uint64_t size;
    uint64_t phys;
    struct DirectMapping *next;
} DirectMapping;

static DirectMapping *g_dmaps = NULL; // protected by g_direct_mutex
// Set while this thread holds g_direct_mutex around a call into the VM allocator (map calls). A
// MAP_FIXED replace unmaps what it replaces and reaches ps4_direct_mem_on_unmap on the same
// thread; it must not take the (non-recursive) mutex again. Lock order is always
// g_direct_mutex -> vm_mutex: the allocator never calls the hook with vm_mutex held.
static _Thread_local bool t_direct_held;
static uint8_t *g_phys_base = NULL;
static size_t g_phys_size = 0;

void ps4_gnm_guest_remapped(uint64_t addr, uint64_t size) __attribute__((weak));

static void dm_phys_init(void) {
    if (g_phys_base || getenv("PS4_NO_PHYS_ALIAS")) return;
    size_t sz = 128ULL << 30; // virtual only; pages are committed on first touch
    void *p = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (p == MAP_FAILED) {
        PS4_LOGF(NPS5_S_MEM, NPS5_L_WARN, "ps4-mem", "WARNING: cannot reserve physical backing (%d); direct memory aliasing disabled\n", errno);
        return;
    }
    g_phys_base = (uint8_t *)p;
    g_phys_size = sz;
}

// Map guest [vaddr, vaddr+len) onto physical [phys, phys+len). Caller holds g_direct_mutex.
static int dm_alias(GuestContext *ctx, uint64_t vaddr, uint64_t phys, size_t len) {
#if defined(__APPLE__)
    GuestContext *proc = ctx->process_ctx ? ctx->process_ctx : ctx;
    if (!g_phys_base || ((vaddr | phys | len) & 0x3fffULL) || phys + len > g_phys_size ||
        vaddr + len > proc->mem_size)
        return -1;
    mach_vm_address_t target = (mach_vm_address_t)(uintptr_t)(proc->mem_base + vaddr);
    vm_prot_t cur = 0, max = 0;
    kern_return_t kr;
    PS4_HITCH_TIMED(PS4_HITCH_VM, kr = mach_vm_remap(mach_task_self(), &target, len, 0,
                                                     VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(),
                                                     (mach_vm_address_t)(uintptr_t)(g_phys_base + phys), FALSE,
                                                     &cur, &max, VM_INHERIT_SHARE));
    if (kr != KERN_SUCCESS) {
        PS4_LOGF(NPS5_S_MEM, NPS5_L_WARN, "ps4-mem", "mach_vm_remap(va=0x%llx phys=0x%llx len=0x%zx) failed: %d\n",
                (unsigned long long)vaddr, (unsigned long long)phys, len, kr);
        return -1;
    }
    DirectMapping *m = (DirectMapping *)calloc(1, sizeof(*m));
    if (m) {
        m->vaddr = vaddr;
        m->size = len;
        m->phys = phys;
        m->next = g_dmaps;
        g_dmaps = m;
    }
    if (ps4_gnm_guest_remapped) ps4_gnm_guest_remapped(vaddr, len);
    return 0;
#else
    (void)ctx; (void)vaddr; (void)phys; (void)len;
    return -1;
#endif
}

// Reports every other mapping whose physical range overlaps [phys, phys+len) of the new mapping at vaddr. Caller holds
// g_direct_mutex.
static void dm_report_aliases(uint64_t vaddr, uint64_t phys, uint64_t len) {
    for (const DirectMapping *m = g_dmaps; m; m = m->next) {
        if (m->vaddr == vaddr && m->phys == phys) continue; // the new mapping itself
        const uint64_t lo = m->phys > phys ? m->phys : phys;
        const uint64_t hi = m->phys + m->size < phys + len ? m->phys + m->size : phys + len;
        if (lo >= hi) continue;
        recomp_vm_hooks_report_alias(vaddr + (lo - phys), m->vaddr + (lo - m->phys), hi - lo);
    }
}

// Called by recomp_vm_free (without vm_mutex held): detach guest [addr, addr+size) from
// physical memory so the virtual range no longer aliases it.
void ps4_direct_mem_on_unmap(GuestContext *ctx, uint64_t addr, size_t size) {
    if (!ctx || size == 0) return;
    GuestContext *proc = ctx->process_ctx ? ctx->process_ctx : ctx;
    uint64_t end = addr + size;
    const bool held = t_direct_held;
    if (!held) pthread_mutex_lock(&g_direct_mutex);
    DirectMapping **pp = &g_dmaps;
    while (*pp) {
        DirectMapping *m = *pp;
        uint64_t mend = m->vaddr + m->size;
        if (m->vaddr >= end || mend <= addr) { pp = &m->next; continue; }
        uint64_t ostart = m->vaddr > addr ? m->vaddr : addr;
        uint64_t oend = mend < end ? mend : end;
        // Replace the overlap with fresh private zero pages.
        // (mach_vm_map, not mmap(MAP_FIXED): the maximum protection stays read-write and the pages
        // keep the arena's VM tag; review 1 n1)
        mach_vm_address_t fix = (mach_vm_address_t)(uintptr_t)(proc->mem_base + ostart);
        PS4_HITCH_TIMED(PS4_HITCH_VM, mach_vm_map(mach_task_self(), &fix, oend - ostart, 0,
                                                  VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE | VM_MAKE_TAG(240), MEMORY_OBJECT_NULL, 0,
                                                  FALSE, VM_PROT_READ | VM_PROT_WRITE, VM_PROT_READ | VM_PROT_WRITE,
                                                  VM_INHERIT_NONE));
        if (ps4_gnm_guest_remapped) ps4_gnm_guest_remapped(ostart, oend - ostart);
        if (ostart > m->vaddr && oend < mend) {
            DirectMapping *tail = (DirectMapping *)calloc(1, sizeof(*tail));
            if (tail) {
                tail->vaddr = oend;
                tail->size = mend - oend;
                tail->phys = m->phys + (oend - m->vaddr);
                tail->next = m->next;
                m->next = tail;
            }
            m->size = ostart - m->vaddr;
            pp = &m->next;
        } else if (ostart > m->vaddr) {
            m->size = ostart - m->vaddr;
            pp = &m->next;
        } else if (oend < mend) {
            m->phys += oend - m->vaddr;
            m->size = mend - oend;
            m->vaddr = oend;
            pp = &m->next;
        } else {
            *pp = m->next;
            free(m);
        }
    }
    if (!held) pthread_mutex_unlock(&g_direct_mutex);
}

// Physical address backing guest address `addr`, if it is mapped direct memory.
static bool dm_lookup(uint64_t addr, uint64_t *start, uint64_t *end, uint64_t *phys) {
    bool ok = false;
    pthread_mutex_lock(&g_direct_mutex);
    for (DirectMapping *m = g_dmaps; m; m = m->next) {
        if (addr >= m->vaddr && addr < m->vaddr + m->size) {
            *start = m->vaddr;
            *end = m->vaddr + m->size;
            *phys = m->phys;
            ok = true;
            break;
        }
    }
    pthread_mutex_unlock(&g_direct_mutex);
    return ok;
}
static int g_direct_inited = 0;
// SceProcParam sdk_version (+0x10, 64 bits): a PS4 image has the PS4 SDK version in the low word
// and 0 above; a PS5 image has its PS5 SDK version in the high word (ASTRO BOT: 0x09000040_11590001).
static uint64_t g_guest_sdk64;
int nps5_guest_is_ps5(void) { return (g_guest_sdk64 >> 32) != 0; }
static size_t g_direct_size;       // set by ps4_direct_mem_configure
static bool g_direct_configured;  // ps4_direct_mem_configure ran

// Flexible memory of a process: its flexible_memory_size plus the base, or the default.
static uint64_t flexible_mem_size(int has_flex, uint64_t flex) {
    return has_flex ? flex + (PS4_MEM_FLEXIBLE_BASE_MIB << 20) : (PS4_MEM_FLEXIBLE_DEFAULT_MIB << 20);
}

uint64_t ps4_direct_mem_size_rule(uint32_t sdk, int neo, int has_flex, uint64_t flex, int ext1, int ext2) {
    const uint64_t mib = 1ULL << 20;
    uint64_t total = (neo ? PS4_MEM_TOTAL_NEO_MIB : PS4_MEM_TOTAL_MIB) * mib;
    const uint64_t flexible = flexible_mem_size(has_flex, flex);
    if (sdk >= PS4_MEM_SDK_EXTENDED_ALWAYS) {
        ext1 = 1;
        ext2 = 1;
    }
    if (neo && !ext1) total -= PS4_MEM_NO_EXTENDED_1_NEO_MIB * mib;
    if (!neo && !ext2) total -= PS4_MEM_NO_EXTENDED_2_BASE_MIB * mib;
    return flexible < total ? total - flexible : 0;
}

// PS4_DIRECT_MEM_SIZE=<n>[k|m|g]: an explicit size in place of the process's (not console behaviour).
static size_t direct_mem_size_override(void) {
    const char *env = getenv("PS4_DIRECT_MEM_SIZE");
    if (!env || !*env) return 0;
    char *end = NULL;
    unsigned long long val = strtoull(env, &end, 0);
    if (end && *end) {
        if (*end == 'g' || *end == 'G') val *= 1024ULL * 1024 * 1024;
        else if (*end == 'm' || *end == 'M') val *= 1024ULL * 1024;
        else if (*end == 'k' || *end == 'K') val *= 1024ULL;
    }
    return (size_t)val;
}

// The size before ps4_direct_mem_configure ran is the one of a process without parameters.
static size_t get_total_direct_mem_size(void) {
    if (g_direct_configured) return g_direct_size;
    size_t over = direct_mem_size_override();
    if (over) return over;
    return (size_t)ps4_direct_mem_size_rule(0, sceKernelIsNeoMode() != 0, 0, 0, 0, 0);
}

// Reads `n` bytes of guest memory at `addr` into `out`; false when the range is outside it.
static bool guest_read(const GuestContext *proc, uint64_t addr, void *out, size_t n) {
    if (!proc || !RECOMP_MEM_OK(proc) || addr == 0 || addr > proc->mem_size || n > proc->mem_size - addr) return false;
    memcpy(out, proc->mem_base + addr, n);
    return true;
}

void ps4_direct_mem_configure(GuestContext *ctx, uint64_t proc_param_addr) {
    const GuestContext *proc = ctx ? (ctx->process_ctx ? ctx->process_ctx : ctx) : NULL;
    uint64_t pp_size = 0, sdk = 0, mp_addr = 0, mp_size = 0, flex_ptr = 0, flex = 0, ext1_ptr = 0, ext2_ptr = 0;
    uint8_t ext1 = 0, ext2 = 0;
    int has_flex = 0;
    if (guest_read(proc, proc_param_addr, &pp_size, 8)) {
        if (pp_size >= 0x18) guest_read(proc, proc_param_addr + 0x10, &sdk, 8);
        if (pp_size >= 0x48) guest_read(proc, proc_param_addr + 0x40, &mp_addr, 8);
    }
    if (guest_read(proc, mp_addr, &mp_size, 8)) {
        if (mp_size >= 0x18 && guest_read(proc, mp_addr + 0x10, &flex_ptr, 8))
            has_flex = guest_read(proc, flex_ptr, &flex, 8);
        if (mp_size >= 0x20 && guest_read(proc, mp_addr + 0x18, &ext1_ptr, 8)) guest_read(proc, ext1_ptr, &ext1, 1);
        if (mp_size >= 0x30 && guest_read(proc, mp_addr + 0x28, &ext2_ptr, 8)) guest_read(proc, ext2_ptr, &ext2, 1);
    }
    g_guest_sdk64 = sdk;
    const int neo = sceKernelIsNeoMode() != 0;
    const int sdk_ext = (uint32_t)sdk >= PS4_MEM_SDK_EXTENDED_ALWAYS;
    const int use_ext1 = sdk_ext || ext1 != 0, use_ext2 = sdk_ext || ext2 != 0;
    const uint64_t size = ps4_direct_mem_size_rule((uint32_t)sdk, neo, has_flex, flex, ext1 != 0, ext2 != 0);
    const uint64_t flexible = flexible_mem_size(has_flex, flex);
    PS4_LOGF(NPS5_S_MEM, NPS5_L_DEBUG, "ps4-mem", "direct memory %llu MiB (sdk 0x%llx, flexible %llu MiB, ext1 %d, ext2 %d)\n",
           (unsigned long long)(size >> 20), (unsigned long long)sdk, (unsigned long long)(flexible >> 20), use_ext1,
           use_ext2);
    size_t total = (size_t)size;
    const size_t over = direct_mem_size_override();
    if (over) {
        PS4_LOGF(NPS5_S_MEM, NPS5_L_DEBUG, "ps4-mem", "PS4_DIRECT_MEM_SIZE: direct memory %zu MiB instead (not console behaviour)\n", over >> 20);
        total = over;
    }
    fflush(stdout);

    pthread_mutex_lock(&g_direct_mutex);
    const size_t old_total = get_total_direct_mem_size();
    if (g_direct_inited && total != old_total) {
        // Direct memory already exists: resize its free end, as the kernel does when it sets up
        // the process's regions. When the end is in use, the size stays what the blocks cover.
        DirectMemBlock *last = g_direct_blocks;
        while (last && last->next) last = last->next;
        if (last && last->is_free && (size_t)last->phys_offset + last->size == old_total &&
            (total > old_total || last->size >= old_total - total)) {
            last->size = total - (size_t)last->phys_offset;
        } else {
            PS4_LOGF(NPS5_S_MEM, NPS5_L_WARN, "ps4-mem", "ERROR: cannot resize direct memory from %zu to %zu MiB: its end is in use\n",
                    old_total >> 20, total >> 20);
            total = old_total;
        }
    }
    g_direct_size = total;
    g_direct_configured = true;
    pthread_mutex_unlock(&g_direct_mutex);
}

int ps4_direct_mem_init(void) {
    pthread_mutex_lock(&g_direct_mutex);
    if (!g_direct_inited) {
        g_direct_blocks = (DirectMemBlock *)calloc(1, sizeof(DirectMemBlock));
        if (!g_direct_blocks) {
            pthread_mutex_unlock(&g_direct_mutex);
            return -1;
        }
        g_direct_blocks->phys_offset = 0;
        g_direct_blocks->size = get_total_direct_mem_size();
        g_direct_blocks->is_free = 1;
        g_direct_blocks->mapped_guest_vaddr = 0;
        g_direct_blocks->next = NULL;
        g_direct_blocks->prev = NULL;
        g_direct_inited = 1;
        dm_phys_init();
    }
    pthread_mutex_unlock(&g_direct_mutex);
    return 0;
}

void ps4_direct_mem_destroy(void) {
    pthread_mutex_lock(&g_direct_mutex);
    DirectMemBlock *curr = g_direct_blocks;
    while (curr) {
        DirectMemBlock *next = curr->next;
        free(curr);
        curr = next;
    }
    g_direct_blocks = NULL;
    g_direct_inited = 0;
    pthread_mutex_unlock(&g_direct_mutex);
}

size_t sceKernelGetDirectMemorySize(void) {
    return get_total_direct_mem_size();
}

size_t sceKernelAvailableDirectMemorySize(void) {
    ps4_direct_mem_init();
    pthread_mutex_lock(&g_direct_mutex);
    size_t free_total = 0;
    DirectMemBlock *curr = g_direct_blocks;
    while (curr) {
        if (curr->is_free) {
            free_total += curr->size;
        }
        curr = curr->next;
    }
    pthread_mutex_unlock(&g_direct_mutex);
    return free_total;
}

// Direct memory covers [0, sceKernelGetDirectMemorySize()) and never grows. An allocation is the
// lowest aligned range of `length` bytes in a free block within [searchStart, searchEnd), as in
// shadPS4's MemoryManager::Allocate. Errors as shadPS4's sceKernelAllocateDirectMemory: EINVAL for
// a negative bound, EAGAIN for a range smaller than the length or when nothing fits.
int sceKernelAllocateDirectMemory(off_t searchStart, off_t searchEnd, size_t length, size_t alignment, int type, off_t *physAddrOut) {
    (void)type;
    PS4_LOGF(NPS5_S_MEM, NPS5_L_DEBUG, "ps4-mem", "sceKernelAllocateDirectMemory: start=0x%llx end=0x%llx len=%zu (%.2f MB) align=%zu type=%d\n",
           (unsigned long long)searchStart, (unsigned long long)searchEnd, length, (double)length / (1024.0 * 1024.0), alignment, type);
    fflush(stdout);

    if (!physAddrOut || length == 0 || searchStart < 0 || searchEnd < 0) return 0x80020016; // ORBIS_KERNEL_ERROR_EINVAL
    if (alignment == 0) alignment = 4096;
    // An alignment is a power of two no larger than the memory, and the rounded length must not wrap
    // or exceed the memory: ~0 used to round to 0 and "succeed" with an empty block.
    if ((alignment & (alignment - 1)) != 0 || alignment > get_total_direct_mem_size() ||
        length > get_total_direct_mem_size())
        return 0x80020016; // ORBIS_KERNEL_ERROR_EINVAL
    const size_t aligned_len = (length + alignment - 1) & ~(alignment - 1);
    if (aligned_len == 0 || aligned_len > get_total_direct_mem_size()) return 0x80020016;
    if (searchEnd <= searchStart || (size_t)(searchEnd - searchStart) < aligned_len)
        return 0x80020023; // ORBIS_KERNEL_ERROR_EAGAIN

    ps4_direct_mem_init();

    pthread_mutex_lock(&g_direct_mutex);
    DirectMemBlock *best = NULL;
    size_t alloc_start = 0;
    for (DirectMemBlock *curr = g_direct_blocks; curr; curr = curr->next) {
        if (!curr->is_free) continue;
        const size_t block_start = (size_t)curr->phys_offset, block_end = block_start + curr->size;
        const size_t from = block_start > (size_t)searchStart ? block_start : (size_t)searchStart;
        const size_t start = (from + alignment - 1) & ~(alignment - 1);
        if (start < from || start + aligned_len > block_end) continue;
        if (start + aligned_len > (size_t)searchEnd) break; // blocks are in address order
        best = curr;
        alloc_start = start;
        break;
    }

    // Bookkeeping for the split: a leading free block and a trailing one, made before any change.
    const size_t front_pad = best ? alloc_start - (size_t)best->phys_offset : 0;
    DirectMemBlock *pad_block = best && front_pad ? (DirectMemBlock *)calloc(1, sizeof(DirectMemBlock)) : NULL;
    DirectMemBlock *tail = best && best->size - front_pad > aligned_len ? (DirectMemBlock *)calloc(1, sizeof(DirectMemBlock)) : NULL;
    if (!best || (front_pad && !pad_block) || (best->size - front_pad > aligned_len && !tail)) {
        pthread_mutex_unlock(&g_direct_mutex);
        free(pad_block);
        free(tail);
        PS4_LOGF(NPS5_S_MEM, NPS5_L_WARN, "ps4-mem", "sceKernelAllocateDirectMemory: no free %zu MiB in [0x%llx, 0x%llx) of %zu MiB\n",
                aligned_len >> 20, (unsigned long long)searchStart, (unsigned long long)searchEnd,
                get_total_direct_mem_size() >> 20);
        return 0x80020023; // ORBIS_KERNEL_ERROR_EAGAIN
    }

    if (pad_block) {
        pad_block->phys_offset = best->phys_offset;
        pad_block->size = front_pad;
        pad_block->is_free = 1;
        pad_block->prev = best->prev;
        pad_block->next = best;
        if (best->prev) best->prev->next = pad_block;
        else g_direct_blocks = pad_block;
        best->prev = pad_block;
        best->phys_offset = (off_t)alloc_start;
        best->size -= front_pad;
    }
    if (tail) {
        tail->phys_offset = best->phys_offset + (off_t)aligned_len;
        tail->size = best->size - aligned_len;
        tail->is_free = 1;
        tail->prev = best;
        tail->next = best->next;
        if (best->next) best->next->prev = tail;
        best->next = tail;
        best->size = aligned_len;
    }

    best->is_free = 0;
    *physAddrOut = best->phys_offset;
    pthread_mutex_unlock(&g_direct_mutex);

    return 0;
}

// Orbis error for the errno recomp_vm_map_ex reports.
static int vm_err_to_orbis(int err) {
    return err == EINVAL ? 0x80020016 /* EINVAL */ : 0x8002000c /* ENOMEM */;
}

int sceKernelMapDirectMemory(GuestContext *ctx, void **addrInOut, size_t length, int prot, int flags, off_t physAddr, size_t alignment) {
    PS4_LOGF(NPS5_S_MEM, NPS5_L_DEBUG, "ps4-mem", "sceKernelMapDirectMemory: in=%p len=%zu (%.2f MB) prot=0x%x flags=0x%x phys=0x%llx align=%zu\n",
           addrInOut ? *addrInOut : NULL, length, (double)length / (1024.0 * 1024.0), prot, flags, (unsigned long long)physAddr, alignment);
    fflush(stdout);

    if (!ctx || !addrInOut || length == 0) return 0x80020016; // ORBIS_KERNEL_ERROR_EINVAL
    if (alignment == 0) alignment = 4096;

    ps4_direct_mem_init();

    pthread_mutex_lock(&g_direct_mutex);
    t_direct_held = true; // see t_direct_held: a fixed map below may unmap a direct mapping
    DirectMemBlock *curr = g_direct_blocks;
    DirectMemBlock *found = NULL;
    while (curr) {
        // Any sub-range of an allocated block may be mapped (not only its start).
        if (!curr->is_free && physAddr >= curr->phys_offset &&
            (uint64_t)physAddr + length <= (uint64_t)curr->phys_offset + curr->size) {
            found = curr;
            break;
        }
        curr = curr->next;
    }

    if (!found) {
        PS4_LOGF(NPS5_S_MEM, NPS5_L_WARN, "ps4-mem", "ERROR: sceKernelMapDirectMemory: physAddr 0x%llx (len 0x%zx) not in an allocated block!\n", (unsigned long long)physAddr, length);
        t_direct_held = false;
        pthread_mutex_unlock(&g_direct_mutex);
        return 0x80020016; // ORBIS_KERNEL_ERROR_EINVAL
    }

    // The length is the caller's; recomp_vm_map_ex rounds it (pages for a fixed address, the
    // alignment otherwise), so a fixed map never reaches into its neighbours.
    int merr = 0;
    uint64_t vaddr = recomp_vm_map_ex(ctx, (uint64_t)*addrInOut, length, alignment, prot, flags, "direct_mem",
                                      NPS5_EXT_DIRECT, &merr);

    if (vaddr == (uint64_t)-1) {
        PS4_LOGF(NPS5_S_MEM, NPS5_L_WARN, "ps4-mem", "ERROR: sceKernelMapDirectMemory failed to allocate VM extent! len=%zu align=%zu\n", length, alignment);
        t_direct_held = false;
        pthread_mutex_unlock(&g_direct_mutex);
        return vm_err_to_orbis(merr);
    }

    found->mapped_guest_vaddr = vaddr;
    *addrInOut = (void *)vaddr;
    if (dm_alias(ctx, vaddr, (uint64_t)physAddr, (length + 0x3fffULL) & ~0x3fffULL) != 0 && g_phys_base) {
        // Private zero-fill memory that does not alias the physical block would make two mappings of
        // one block silently diverge: fail the call and undo the mapping.
        PS4_LOGF(NPS5_S_MEM, NPS5_L_WARN, "ps4-mem", "ERROR: direct mapping 0x%llx (phys 0x%llx) could not be physically aliased\n",
                (unsigned long long)vaddr, (unsigned long long)physAddr);
        found->mapped_guest_vaddr = 0;
        GuestVMExtent mapped;
        size_t undo = (length + 0x3fffULL) & ~0x3fffULL;
        if (recomp_vm_query(ctx, vaddr, &mapped) && mapped.addr == vaddr && mapped.size > undo) undo = mapped.size;
        recomp_vm_free(ctx, vaddr, undo);
        *addrInOut = NULL;
        t_direct_held = false;
        pthread_mutex_unlock(&g_direct_mutex);
        return 0x8002000c; // ORBIS_KERNEL_ERROR_ENOMEM
    }

    // Other mappings of the same physical pages first (see Nps5VmHooks.on_alias), then the mapping itself: its pages
    // are final only now (dm_alias replaced them).
    dm_report_aliases(vaddr, (uint64_t)physAddr, (length + 0x3fffULL) & ~0x3fffULL);
    recomp_vm_hooks_report_map(ctx, vaddr, (length + 0x3fffULL) & ~0x3fffULL);

    PS4_LOGF(NPS5_S_MEM, NPS5_L_DEBUG, "ps4-mem", "sceKernelMapDirectMemory SUCCESS: out=%p\n", (void *)vaddr);
    fflush(stdout);

    t_direct_held = false;
    pthread_mutex_unlock(&g_direct_mutex);
    return 0;
}

extern GuestContext *g_nps5_vm_root; // nps5_vm.c

int sceKernelReleaseDirectMemory(off_t physAddr, size_t length) {
    (void)length;
    ps4_direct_mem_init();

    pthread_mutex_lock(&g_direct_mutex);
    DirectMemBlock *curr = g_direct_blocks;
    while (curr) {
        if (!curr->is_free && curr->phys_offset == physAddr) {
            // The guest mappings of the released memory go with it, as on the console: a later
            // access is a classified fault, not a silent use of memory another allocation may now own.
            // Collected first: the unmap hook edits g_dmaps. t_direct_held lets that hook run under
            // the lock this call already holds.
            if (g_nps5_vm_root) {
                const uint64_t plo = (uint64_t)curr->phys_offset, phi = plo + curr->size;
                size_t n = 0;
                for (DirectMapping *m = g_dmaps; m; m = m->next)
                    if (m->phys < phi && m->phys + m->size > plo) n++;
                uint64_t (*rng)[2] = n ? malloc(n * sizeof(*rng)) : NULL;
                size_t k = 0;
                if (rng) {
                    for (DirectMapping *m = g_dmaps; m && k < n; m = m->next) {
                        if (m->phys >= phi || m->phys + m->size <= plo) continue;
                        const uint64_t lo = m->phys > plo ? m->phys : plo, hi = m->phys + m->size < phi ? m->phys + m->size : phi;
                        rng[k][0] = m->vaddr + (lo - m->phys);
                        rng[k][1] = hi - lo;
                        k++;
                    }
                    t_direct_held = true;
                    for (size_t i = 0; i < k; i++) recomp_vm_free(g_nps5_vm_root, rng[i][0], rng[i][1]);
                    t_direct_held = false;
                    free(rng);
                }
            }
            curr->is_free = 1;
            curr->mapped_guest_vaddr = 0;

            // Merge with next if free
            if (curr->next && curr->next->is_free) {
                DirectMemBlock *next = curr->next;
                curr->size += next->size;
                curr->next = next->next;
                if (next->next) next->next->prev = curr;
                free(next);
            }
            // Merge with prev if free
            if (curr->prev && curr->prev->is_free) {
                DirectMemBlock *prev = curr->prev;
                prev->size += curr->size;
                prev->next = curr->next;
                if (curr->next) curr->next->prev = prev;
                free(curr);
            }
            pthread_mutex_unlock(&g_direct_mutex);
            return 0;
        }
        curr = curr->next;
    }
    pthread_mutex_unlock(&g_direct_mutex);
    return 0x80020016; // ORBIS_KERNEL_ERROR_EINVAL
}

int sceKernelMapNamedFlexibleMemory(GuestContext *ctx, void **addrInOut, size_t length, int prot, int flags, const char *name) {
    PS4_LOGF(NPS5_S_MEM, NPS5_L_DEBUG, "ps4-mem", "sceKernelMapNamedFlexibleMemory (%s): in=%p len=%zu (%.2f MB) prot=0x%x flags=0x%x\n",
           name ? name : "null", addrInOut ? *addrInOut : NULL, length, (double)length / (1024.0 * 1024.0), prot, flags);
    fflush(stdout);

    if (!ctx || !addrInOut || length == 0) return 0x80020016; // ORBIS_KERNEL_ERROR_EINVAL
    const char *tag = (name && *name) ? name : "flexible_mem";

    // In Orbis OS, flexible memory allocations are aligned to 64KB (0x10000)
    size_t alignment = 65536;

    int merr = 0;
    uint64_t vaddr = recomp_vm_map_ex(ctx, (uint64_t)*addrInOut, length, alignment, prot, flags, tag,
                                      NPS5_EXT_FLEXIBLE, &merr);

    if (vaddr == (uint64_t)-1) {
        fprintf(stderr, "[vmm] sceKernelMapNamedFlexibleMemory FAILED: tag=%s, len=%zu (0x%zx), in_addr=%p\n",
                tag, length, length, *addrInOut);
        return vm_err_to_orbis(merr);
    }

    *addrInOut = (void *)vaddr;
    PS4_LOGF(NPS5_S_MEM, NPS5_L_DEBUG, "ps4-mem", "sceKernelMapNamedFlexibleMemory SUCCESS: out=%p\n", (void *)vaddr);
    fflush(stdout);
    return 0;
}

int sceKernelMapFlexibleMemory(GuestContext *ctx, void **addrInOut, size_t length, int prot, int flags) {
    return sceKernelMapNamedFlexibleMemory(ctx, addrInOut, length, prot, flags, "flexible_mem");
}

int sceKernelReserveVirtualRange(GuestContext *ctx, void **addrInOut, size_t length, int flags, size_t alignment) {
    PS4_LOGF(NPS5_S_MEM, NPS5_L_DEBUG, "ps4-mem", "sceKernelReserveVirtualRange: in=%p len=%zu (%.2f MB) flags=0x%x align=%zu\n",
           addrInOut ? *addrInOut : NULL, length, (double)length / (1024.0 * 1024.0), flags, alignment);
    fflush(stdout);

    if (!ctx || !addrInOut || length == 0) return 0x80020016; // ORBIS_KERNEL_ERROR_EINVAL
    if (alignment == 0) alignment = 65536;

    int merr = 0;
    uint64_t vaddr = recomp_vm_map_ex(ctx, (uint64_t)*addrInOut, length, alignment, PROT_NONE, flags, "reserved",
                                      NPS5_EXT_RESERVED, &merr);

    if (vaddr == (uint64_t)-1) {
        PS4_LOGF(NPS5_S_MEM, NPS5_L_WARN, "ps4-mem", "ERROR: sceKernelReserveVirtualRange FAILED! in=%p len=%zu align=%zu\n",
                addrInOut ? *addrInOut : NULL, length, alignment);
        return vm_err_to_orbis(merr);
    }

    *addrInOut = (void *)vaddr;
    PS4_LOGF(NPS5_S_MEM, NPS5_L_DEBUG, "ps4-mem", "sceKernelReserveVirtualRange SUCCESS: out=%p\n", (void *)vaddr);
    fflush(stdout);
    return 0;
}

int sceKernelConfiguredFlexibleMemorySize(GuestContext *ctx, uint64_t *sizeOut) {
    if (!sizeOut) return -EINVAL;
    uint64_t min_fmem = 4ULL * 1024 * 1024 * 1024; // 4 GB minimum flexible memory budget for 64-bit recompiled titles
    uint64_t from_param = 0;
    if (ctx && RECOMP_MEM_OK(ctx)) {
        GuestContext *proc = ctx->process_ctx ? ctx->process_ctx : ctx;
        if (proc->proc_param_addr != 0 && proc->proc_param_addr + 0x48 <= proc->mem_size) {
            // OrbisProcParam has mem_param pointer at offset +0x40
            uint64_t mem_param_addr = *(uint64_t *)(proc->mem_base + proc->proc_param_addr + 0x40ULL);
            if (mem_param_addr != 0 && mem_param_addr + 0x18 <= proc->mem_size) {
                // OrbisKernelMemParam has flexible_memory_size pointer at offset +0x10
                uint64_t flex_ptr = *(uint64_t *)(proc->mem_base + mem_param_addr + 0x10ULL);
                if (flex_ptr != 0 && flex_ptr + 8 <= proc->mem_size) {
                    uint64_t flex_extra = *(uint64_t *)(proc->mem_base + flex_ptr);
                    if (flex_extra > 0 && flex_extra < 32ULL * 1024 * 1024 * 1024) {
                        // Base flexible memory is 272MB (0x11000000)
                        from_param = flex_extra + (272ULL * 1024 * 1024);
                    }
                }
            }
        }
    }
    if (from_param > min_fmem) {
        *sizeOut = from_param;
    } else {
        *sizeOut = min_fmem;
    }
    PS4_LOGF(NPS5_S_MEM, NPS5_L_DEBUG, "ps4-mem", "sceKernelConfiguredFlexibleMemorySize: %llu (%.2f MB)\n",
           (unsigned long long)*sizeOut, (double)*sizeOut / (1024.0 * 1024.0));
    fflush(stdout);
    return 0;
}

size_t sceKernelAvailableFlexibleMemorySize(GuestContext *ctx) {
    if (!ctx) return 0;
    GuestContext *proc = ctx->process_ctx ? ctx->process_ctx : ctx;
    uint64_t total_fmem = 4ULL * 1024 * 1024 * 1024;
    sceKernelConfiguredFlexibleMemorySize(ctx, &total_fmem);

    pthread_mutex_lock(&proc->vm_mutex);
    size_t used = 0;
    GuestVMExtent *curr = proc->vm_extents;
    while (curr) {
        if (!curr->is_free && curr->kind == NPS5_EXT_FLEXIBLE) {
            used += curr->size;
        }
        curr = curr->next;
    }
    pthread_mutex_unlock(&proc->vm_mutex);

    if (used >= total_fmem) return 0;
    return (size_t)(total_fmem - used);
}

int sceKernelVirtualQuery(GuestContext *ctx, const void *addr, int flags, OrbisKernelVirtualQueryInfo *info, size_t infoSize) {
    (void)flags;
    if (!ctx || !info || infoSize < sizeof(OrbisKernelVirtualQueryInfo)) {
        return -EINVAL;
    }
    uint64_t vaddr = (uint64_t)addr;
    GuestVMExtent ext_copy;
    if (!recomp_vm_query(ctx, vaddr, &ext_copy)) {
        return -EFAULT;
    }
    const GuestVMExtent *extent = &ext_copy;

    memset(info, 0, infoSize);
    info->start = (void *)extent->addr;
    info->end = (void *)(extent->addr + extent->size);
    info->prot = extent->prot ? extent->prot : (PROT_READ | PROT_WRITE);
    info->flags = extent->flags;
    // A reservation (sceKernelReserveVirtualRange) holds address space only: no pages, not committed
    // (shadPS4 VirtualQuery: is_committed = vma.IsMapped(), false for Free and Reserved). Allocators
    // read this bit to choose between mprotect (committed) and allocate + MAP_FIXED map (not committed);
    // the Unity PS5 player's virtual-memory commit does exactly that on its reserved ranges.
    // NPS5_VQ_RESERVED_UNCOMMITTED=1 (profiles/engines/unity-ps5.json) until the titles that ran with
    // the old answer (reservations reported committed) have been regression-tested with it.
    static int reserved_uncommitted = -1;
    if (reserved_uncommitted < 0) {
        const char *e = getenv("NPS5_VQ_RESERVED_UNCOMMITTED");
        reserved_uncommitted = (e && e[0] == '1') ? 1 : 0;
    }
    info->isCommitted = (extent->is_free || (reserved_uncommitted && extent->kind == NPS5_EXT_RESERVED)) ? 0 : 1;
    info->isFlexibleMemory = (extent->kind == NPS5_EXT_FLEXIBLE) ? 1 : 0;
    info->isDirectMemory = (extent->kind == NPS5_EXT_DIRECT) ? 1 : 0;
    info->isPooledMemory = (extent->kind == NPS5_EXT_POOL) ? 1 : 0;
    strncpy(info->name, extent->name, sizeof(info->name) - 1);
    uint64_t ds, de, dp;
    if (dm_lookup(vaddr, &ds, &de, &dp)) {
        info->start = (void *)ds;
        info->end = (void *)de;
        info->offset = (off_t)dp;
        info->isDirectMemory = 1;
        info->isFlexibleMemory = 0;
        info->isCommitted = 1;
    }

    return 0;
}

int sceKernelQueryMemoryProtection(GuestContext *ctx, const void *addr, void **start, void **end, int *prot) {
    if (!ctx) return -EINVAL;
    uint64_t vaddr = (uint64_t)addr;
    GuestVMExtent ext_copy;
    if (!recomp_vm_query(ctx, vaddr, &ext_copy)) {
        return -EFAULT;
    }
    const GuestVMExtent *extent = &ext_copy;
    if (start) *start = (void *)extent->addr;
    if (end) *end = (void *)(extent->addr + extent->size);
    if (prot) *prot = extent->prot ? extent->prot : (PROT_READ | PROT_WRITE);
    return 0;
}

int sceKernelAllocateMainDirectMemory(size_t length, size_t alignment, int type, off_t *physAddrOut) {
    PS4_LOGF(NPS5_S_MEM, NPS5_L_DEBUG, "ps4-mem", "sceKernelAllocateMainDirectMemory: len=%zu (%.2f MB) align=%zu type=%d\n",
           length, (double)length / (1024.0 * 1024.0), alignment, type);
    fflush(stdout);
    // shadPS4 (and the SDK's description): the whole direct memory is the search range.
    return sceKernelAllocateDirectMemory(0, (off_t)sceKernelGetDirectMemorySize(), length, alignment, type, physAddrOut);
}

// Memory Pool implementation
int sceKernelMemoryPoolReserve(GuestContext *ctx, void *addrIn, size_t len, size_t alignment, int flags, void **addrOut) {
    PS4_LOGF(NPS5_S_MEM, NPS5_L_DEBUG, "ps4-mem", "sceKernelMemoryPoolReserve: in=%p len=%zu (%.2f MB) align=%zu flags=0x%x\n",
           addrIn, len, (double)len / (1024.0 * 1024.0), alignment, flags);
    fflush(stdout);

    if (!ctx || !addrOut || len == 0) return 0x80020016; // ORBIS_KERNEL_ERROR_EINVAL
    if (alignment == 0) alignment = 2ULL * 1024 * 1024; // 2MB default alignment

    // Through the same path as every other mapping: a requested address is honoured (MAP_FIXED
    // in flags) or treated as a hint, and a failure is reported, never papered over.
    int merr = 0;
    uint64_t vaddr = recomp_vm_map_ex(ctx, (uint64_t)addrIn, len, alignment, PROT_READ | PROT_WRITE, flags,
                                      "pool_reserved", NPS5_EXT_POOL, &merr);

    if (vaddr == (uint64_t)-1) {
        PS4_LOGF(NPS5_S_MEM, NPS5_L_WARN, "ps4-mem", "ERROR: sceKernelMemoryPoolReserve FAILED: in=%p len=%zu align=%zu\n",
                addrIn, len, alignment);
        return vm_err_to_orbis(merr);
    }

    *addrOut = (void *)vaddr;
    PS4_LOGF(NPS5_S_MEM, NPS5_L_DEBUG, "ps4-mem", "sceKernelMemoryPoolReserve SUCCESS: out=%p\n", (void *)vaddr);
    fflush(stdout);
    return 0; // ORBIS_OK
}

int sceKernelMemoryPoolExpand(GuestContext *ctx, off_t searchStart, off_t searchEnd, size_t len, size_t alignment, off_t *physAddrOut) {
    (void)ctx;
    PS4_LOGF(NPS5_S_MEM, NPS5_L_DEBUG, "ps4-mem", "sceKernelMemoryPoolExpand: start=0x%llx end=0x%llx len=%zu (%.2f MB) align=%zu\n",
           (unsigned long long)searchStart, (unsigned long long)searchEnd, len, (double)len / (1024.0 * 1024.0), alignment);
    fflush(stdout);
    // Errors as shadPS4's sceKernelMemoryPoolExpand: EINVAL for a bad range, ENOMEM when nothing fits.
    if (len == 0 || !physAddrOut || searchStart < 0 || searchEnd <= searchStart) return 0x80020016;
    if (alignment == 0) alignment = 64ULL * 1024; // 64KB default alignment
    const int rc = sceKernelAllocateDirectMemory(searchStart, searchEnd, len, alignment, 3 /* POOLED */, physAddrOut);
    return rc == (int)0x80020023 ? (int)0x8002000c : rc; // EAGAIN -> ORBIS_KERNEL_ERROR_ENOMEM
}

int sceKernelMemoryPoolCommit(GuestContext *ctx, void *addr, size_t len, int type, int prot, int flags) {
    (void)ctx;
    (void)type;
    (void)prot;
    (void)flags;
    if (!addr) return -EINVAL;
    if (len == 0) return 0; // ORBIS_OK for zero-length commit
    return 0; // ORBIS_OK
}

int sceKernelMemoryPoolDecommit(GuestContext *ctx, void *addr, size_t len, int flags) {
    (void)flags;
    if (!ctx || !addr || len == 0) return -EINVAL;
    GuestContext *proc = ctx->process_ctx ? ctx->process_ctx : ctx;
    uint64_t vaddr = (uint64_t)addr;
    if (RECOMP_MEM_OK(proc) && vaddr + len <= proc->mem_size) {
#if defined(__APPLE__) || defined(__linux__)
        madvise(proc->mem_base + vaddr, len, MADV_DONTNEED);
#endif
    }
    return 0; // ORBIS_OK
}

// Shims for guest execution
void shim_sceKernelAllocateDirectMemory(GuestContext *ctx) {
    off_t searchStart = (off_t)ctx->rdi;
    off_t searchEnd = (off_t)ctx->rsi;
    size_t length = (size_t)ctx->rdx;
    size_t alignment = (size_t)ctx->rcx;
    int type = (int)ctx->r8;
    uint64_t physAddrOutGuest = ctx->r9;

    off_t physAddr = 0;
    int rc = sceKernelAllocateDirectMemory(searchStart, searchEnd, length, alignment, type, &physAddr);
    if (rc == 0 && physAddrOutGuest != 0) {
        *(off_t *)(ctx->mem_base + physAddrOutGuest) = physAddr;
    }
    ctx->rax = (uint64_t)(int64_t)rc;
    SHIM_RETURN();
}

void shim_sceKernelAllocateMainDirectMemory(GuestContext *ctx) {
    size_t length = (size_t)ctx->rdi;
    size_t alignment = (size_t)ctx->rsi;
    int type = (int)ctx->rdx;
    uint64_t physAddrOutGuest = ctx->rcx;

    off_t physAddr = 0;
    int rc = sceKernelAllocateMainDirectMemory(length, alignment, type, &physAddr);
    if (rc == 0 && physAddrOutGuest != 0) {
        *(off_t *)(ctx->mem_base + physAddrOutGuest) = physAddr;
    }
    ctx->rax = (uint64_t)(int64_t)rc;
    SHIM_RETURN();
}

void shim_sceKernelGetDirectMemorySize(GuestContext *ctx) {
    ctx->rax = (uint64_t)sceKernelGetDirectMemorySize();
    SHIM_RETURN();
}

void shim_sceKernelAvailableDirectMemorySize(GuestContext *ctx) {
    uint64_t startOutGuest = ctx->rcx;
    uint64_t sizeOutGuest = ctx->r8;

    if (startOutGuest != 0 || sizeOutGuest != 0) {
        // PS4 SDK convention:
        // int sceKernelAvailableDirectMemorySize(off_t searchStart, off_t searchEnd, size_t alignment, off_t *startOut, size_t *sizeOut)
        // Returns 0 on success, writes free block start and size.
        // Returns 0x8002000c (SCE_KERNEL_ERROR_ENOMEM) if no free block is available in range.
        uint64_t searchStart = ctx->rdi;
        uint64_t searchEnd = ctx->rsi;
        uint64_t alignment = ctx->rdx;

        ps4_direct_mem_init();
        pthread_mutex_lock(&g_direct_mutex);
        size_t total_size = get_total_direct_mem_size();
        if (searchEnd == 0 || searchEnd > total_size) {
            searchEnd = total_size;
        }

        uint64_t best_start = 0;
        uint64_t best_size = 0;

        if (searchStart < searchEnd) {
            DirectMemBlock *curr = g_direct_blocks;
            while (curr) {
                if (curr->is_free) {
                    uint64_t b_start = (uint64_t)curr->phys_offset;
                    uint64_t b_end = b_start + curr->size;

                    if (b_end > searchStart && b_start < searchEnd) {
                        uint64_t cand_start = b_start < searchStart ? searchStart : b_start;
                        if (alignment > 0) {
                            cand_start = (cand_start + alignment - 1) & ~(alignment - 1);
                        }
                        uint64_t cand_end = b_end > searchEnd ? searchEnd : b_end;
                        if (cand_start < cand_end) {
                            best_start = cand_start;
                            best_size = cand_end - cand_start;
                            break;
                        }
                    }
                }
                curr = curr->next;
            }
        }
        pthread_mutex_unlock(&g_direct_mutex);

        if (startOutGuest && startOutGuest + sizeof(uint64_t) <= ctx->mem_size) {
            *(uint64_t *)(ctx->mem_base + startOutGuest) = best_start;
        }
        if (sizeOutGuest && sizeOutGuest + sizeof(uint64_t) <= ctx->mem_size) {
            *(uint64_t *)(ctx->mem_base + sizeOutGuest) = best_size;
        }
        if (best_size == 0) {
            ctx->rax = 0x8002000cULL; // SCE_KERNEL_ERROR_ENOMEM
        } else {
            ctx->rax = 0; // SCE_OK
        }
    } else {
        ctx->rax = (uint64_t)sceKernelAvailableDirectMemorySize();
    }
    SHIM_RETURN();
}

// The sceKernelMap* family takes "void **addr": in, the requested address or NULL; out, the
// mapped address. Here *addr is a host pointer (the thunk translated the guest's slot); the
// allocator below works in guest addresses.
#define HLE_ADDR_SLOT_CALL(call)                                                              \
  void *g = addr_in_out ? (void *)(uintptr_t)NPS5_H2G(ctx, *addr_in_out) : NULL;               \
  int rc = (call);                                                                             \
  if (rc == 0 && addr_in_out) *addr_in_out = NPS5_G2H(ctx, (uint64_t)(uintptr_t)g);            \
  return rc

int hle_sceKernelMapDirectMemory(GuestContext *ctx, void **addr_in_out, size_t len, int prot, int flags, off_t phys,
                                 size_t alignment) {
    HLE_ADDR_SLOT_CALL(sceKernelMapDirectMemory(ctx, &g, len, prot, flags, phys, alignment));
}

void shim_sceKernelReleaseDirectMemory(GuestContext *ctx) {
    off_t physAddr = (off_t)ctx->rdi;
    size_t length = (size_t)ctx->rsi;
    ctx->rax = (uint64_t)(int64_t)sceKernelReleaseDirectMemory(physAddr, length);
    SHIM_RETURN();
}

int hle_sceKernelMapFlexibleMemory(GuestContext *ctx, void **addr_in_out, size_t len, int prot, int flags) {
    HLE_ADDR_SLOT_CALL(sceKernelMapFlexibleMemory(ctx, &g, len, prot, flags));
}

int hle_sceKernelMapNamedFlexibleMemory(GuestContext *ctx, void **addr_in_out, size_t len, int prot, int flags,
                                        const char *name) {
    char tag[32] = {0};
    if (name) strncpy(tag, name, sizeof(tag) - 1);
    HLE_ADDR_SLOT_CALL(sceKernelMapNamedFlexibleMemory(ctx, &g, len, prot, flags, tag[0] ? tag : "flexible_mem"));
}

int hle_sceKernelReserveVirtualRange(GuestContext *ctx, void **addr_in_out, size_t len, int flags, size_t alignment) {
    HLE_ADDR_SLOT_CALL(sceKernelReserveVirtualRange(ctx, &g, len, flags, alignment));
}

void shim_sceKernelConfiguredFlexibleMemorySize(GuestContext *ctx) {
    uint64_t sizeOutGuest = ctx->rdi;
    uint64_t cfg_size = 512ULL * 1024 * 1024;
    int rc = sceKernelConfiguredFlexibleMemorySize(ctx, &cfg_size);
    if (sizeOutGuest == 0) {
        ctx->rax = (uint64_t)(int64_t)-EINVAL;
    } else if (RECOMP_MEM_OK(ctx) && sizeOutGuest + sizeof(uint64_t) <= ctx->mem_size) {
        *(uint64_t *)(ctx->mem_base + sizeOutGuest) = cfg_size;
        ctx->rax = (uint64_t)(int64_t)rc;
    } else {
        ctx->rax = (uint64_t)(int64_t)-EFAULT;
    }
    SHIM_RETURN();
}

void shim_sceKernelAvailableFlexibleMemorySize(GuestContext *ctx) {
    size_t avail = sceKernelAvailableFlexibleMemorySize(ctx);
    uint64_t sizeOutGuest = ctx->rdi;
    if (sizeOutGuest != 0 && RECOMP_MEM_OK(ctx) && sizeOutGuest + sizeof(uint64_t) <= ctx->mem_size) {
        *(uint64_t *)(ctx->mem_base + sizeOutGuest) = (uint64_t)avail;
        ctx->rax = 0; // ORBIS_OK
    } else {
        ctx->rax = (uint64_t)avail;
    }
    SHIM_RETURN();
}

void shim_sceKernelVirtualQuery(GuestContext *ctx) {
    uint64_t addr = ctx->rdi;
    int flags = (int)ctx->rsi;
    uint64_t infoGuest = ctx->rdx;
    size_t infoSize = (size_t)ctx->rcx;

    if (!infoGuest) {
        ctx->rax = (uint64_t)(int64_t)-EINVAL;
        SHIM_RETURN();
    }

    OrbisKernelVirtualQueryInfo info = {0};
    int rc = sceKernelVirtualQuery(ctx, (const void *)addr, flags, &info, sizeof(info));
    if (rc == 0) {
        size_t copy_sz = infoSize < sizeof(info) ? infoSize : sizeof(info);
        memcpy(ctx->mem_base + infoGuest, &info, copy_sz);
    }
    ctx->rax = (uint64_t)(int64_t)rc;
    SHIM_RETURN();
}

void shim_sceKernelQueryMemoryProtection(GuestContext *ctx) {
    uint64_t addr = ctx->rdi;
    uint64_t startGuest = ctx->rsi;
    uint64_t endGuest = ctx->rdx;
    uint64_t protGuest = ctx->rcx;

    void *start = NULL;
    void *end = NULL;
    int prot = 0;
    int rc = sceKernelQueryMemoryProtection(ctx, (const void *)addr, &start, &end, &prot);
    if (rc == 0) {
        if (startGuest) *(uint64_t *)(ctx->mem_base + startGuest) = (uint64_t)start;
        if (endGuest) *(uint64_t *)(ctx->mem_base + endGuest) = (uint64_t)end;
        if (protGuest) *(int *)(ctx->mem_base + protGuest) = prot;
    }
    ctx->rax = (uint64_t)(int64_t)rc;
    SHIM_RETURN();
}

void shim_sceKernelMemoryPoolReserve(GuestContext *ctx) {
    // PS4 ABI: sceKernelMemoryPoolReserve(void* addr_in, u64 len, u64 alignment, s32 flags, void** addr_out)
    // RDI: addr_in, RSI: len, RDX: alignment, RCX: flags, R8: addr_out
    void *addr_in = (void *)ctx->rdi;
    size_t len = (size_t)ctx->rsi;
    size_t alignment = (size_t)ctx->rdx;
    int flags = (int)ctx->rcx;
    uint64_t addrOutGuest = ctx->r8;

    // Handle compatibility with old 3-arg stub (addr_out in RDI) if R8 == 0
    if (addrOutGuest == 0 && ctx->rdi != 0 && ctx->rsi != 0 && ctx->rdx != 0 && ctx->rcx == 0) {
        addrOutGuest = ctx->rdi;
        addr_in = NULL;
        flags = (int)ctx->rdx;
    }

    void *addrOut = NULL;
    int rc = sceKernelMemoryPoolReserve(ctx, addr_in, len, alignment, flags, &addrOut);
    if (rc == 0 && addrOutGuest != 0 && addrOutGuest < ctx->mem_size) {
        *(uint64_t *)(ctx->mem_base + addrOutGuest) = (uint64_t)addrOut;
    }
    ctx->rax = (uint64_t)(int64_t)rc;
    SHIM_RETURN();
}

void shim_sceKernelMemoryPoolExpand(GuestContext *ctx) {
    // PS4 ABI: sceKernelMemoryPoolExpand(u64 searchStart, u64 searchEnd, u64 len, u64 alignment, u64* physAddrOut)
    // RDI: searchStart, RSI: searchEnd, RDX: len, RCX: alignment, R8: physAddrOut
    off_t searchStart = (off_t)ctx->rdi;
    off_t searchEnd = (off_t)ctx->rsi;
    size_t len = (size_t)ctx->rdx;
    size_t alignment = (size_t)ctx->rcx;
    uint64_t physAddrOutGuest = ctx->r8;

    off_t physAddr = 0;
    int rc = sceKernelMemoryPoolExpand(ctx, searchStart, searchEnd, len, alignment, &physAddr);
    if (rc == 0 && physAddrOutGuest != 0 && physAddrOutGuest < ctx->mem_size) {
        *(uint64_t *)(ctx->mem_base + physAddrOutGuest) = (uint64_t)physAddr;
    }
    ctx->rax = (uint64_t)(int64_t)rc;
    SHIM_RETURN();
}

void shim_sceKernelMemoryPoolCommit(GuestContext *ctx) {
    // PS4 ABI: sceKernelMemoryPoolCommit(void* addr, u64 len, s32 type, s32 prot, s32 flags)
    // RDI: addr, RSI: len, RDX: type, RCX: prot, R8: flags
    void *addr = (void *)ctx->rdi;
    size_t len = (size_t)ctx->rsi;
    int type = (int)ctx->rdx;
    int prot = (int)ctx->rcx;
    int flags = (int)ctx->r8;
    int rc = sceKernelMemoryPoolCommit(ctx, addr, len, type, prot, flags);
    ctx->rax = (uint64_t)(int64_t)rc;
    SHIM_RETURN();
}

void shim_sceKernelMemoryPoolDecommit(GuestContext *ctx) {
    // PS4 ABI: sceKernelMemoryPoolDecommit(void* addr, u64 len, s32 flags)
    // RDI: addr, RSI: len, RDX: flags
    void *addr = (void *)ctx->rdi;
    size_t len = (size_t)ctx->rsi;
    int flags = (int)ctx->rdx;
    int rc = sceKernelMemoryPoolDecommit(ctx, addr, len, flags);
    ctx->rax = (uint64_t)(int64_t)rc;
    SHIM_RETURN();
}

int sceKernelSetVirtualRangeName(GuestContext *ctx, const void *addr, size_t len, const char *name) {
    if (!name) return 0x8002000e; // ORBIS_KERNEL_ERROR_EFAULT
    if (strlen(name) >= 32) return 0x8002003f; // ORBIS_KERNEL_ERROR_ENAMETOOLONG
    if (!ctx) return 0x80020016; // ORBIS_KERNEL_ERROR_EINVAL

    GuestContext *proc = ctx->process_ctx ? ctx->process_ctx : ctx;
    uint64_t vaddr = (uint64_t)addr;
    uint64_t vend = vaddr + len;

    pthread_mutex_lock(&proc->vm_mutex);
    GuestVMExtent *curr = proc->vm_extents;
    while (curr) {
        uint64_t cstart = curr->addr;
        uint64_t cend = curr->addr + curr->size;
        if (!curr->is_free && cstart < vend && cend > vaddr) {
            strncpy(curr->name, name, sizeof(curr->name) - 1);
            curr->name[sizeof(curr->name) - 1] = '\0';
        }
        curr = curr->next;
    }
    recomp_vm_note_change(); // a renamed reservation is no longer one (recomp_vm_unmapped_ranges)
    pthread_mutex_unlock(&proc->vm_mutex);

    PS4_LOGF(NPS5_S_MEM, NPS5_L_DEBUG, "ps4-mem", "sceKernelSetVirtualRangeName: addr=%p len=%zu name='%s'\n", addr, len, name);
    fflush(stdout);
    return 0;
}

void shim_sceKernelSetVirtualRangeName(GuestContext *ctx) {
    void *addr = (void *)ctx->rdi;
    size_t len = (size_t)ctx->rsi;
    uint64_t nameGuest = ctx->rdx;
    const char *name = (nameGuest && RECOMP_MEM_OK(ctx)) ? (const char *)(ctx->mem_base + nameGuest) : NULL;
    int rc = sceKernelSetVirtualRangeName(ctx, addr, len, name);
    ctx->rax = (uint64_t)(int64_t)rc;
    SHIM_RETURN();
}

int sceKernelDirectMemoryQuery(off_t offset, int flags, OrbisDirectMemoryQueryInfo *info, size_t infoSize) {
    if (!info || infoSize < sizeof(OrbisDirectMemoryQueryInfo)) {
        return 0x80020016; // ORBIS_KERNEL_ERROR_EINVAL
    }
    ps4_direct_mem_init();

    pthread_mutex_lock(&g_direct_mutex);
    DirectMemBlock *curr = g_direct_blocks;
    DirectMemBlock *target = NULL;

    while (curr) {
        off_t bstart = curr->phys_offset;
        off_t bend = curr->phys_offset + curr->size;
        if (flags == 1) { // find_next allocated region
            if (!curr->is_free && bend > offset) {
                target = curr;
                break;
            }
        } else {
            if (offset >= bstart && offset < bend) {
                target = curr;
                break;
            }
        }
        curr = curr->next;
    }

    if (!target || target->is_free) {
        pthread_mutex_unlock(&g_direct_mutex);
        return 0x8002000d; // ORBIS_KERNEL_ERROR_EACCES
    }

    info->start = (uint64_t)target->phys_offset;
    info->end = (uint64_t)(target->phys_offset + target->size);
    info->memoryType = 0; // Standard direct memory
    pthread_mutex_unlock(&g_direct_mutex);

    return 0;
}

void shim_sceKernelDirectMemoryQuery(GuestContext *ctx) {
    off_t offset = (off_t)ctx->rdi;
    int flags = (int)ctx->rsi;
    uint64_t infoGuest = ctx->rdx;
    size_t infoSize = (size_t)ctx->rcx;

    if (!infoGuest) {
        ctx->rax = (uint64_t)0x80020016;
        SHIM_RETURN();
    }

    OrbisDirectMemoryQueryInfo info = {0};
    int rc = sceKernelDirectMemoryQuery(offset, flags, &info, sizeof(info));
    if (rc == 0) {
        size_t copy_sz = infoSize < sizeof(info) ? infoSize : sizeof(info);
        memcpy(ctx->mem_base + infoGuest, &info, copy_sz);
    }
    ctx->rax = (uint64_t)(int64_t)rc;
    SHIM_RETURN();
}

int sceKernelCheckedReleaseDirectMemory(off_t physAddr, size_t length) {
    PS4_LOGF(NPS5_S_MEM, NPS5_L_DEBUG, "ps4-mem", "sceKernelCheckedReleaseDirectMemory: phys=0x%llx len=%zu\n",
           (unsigned long long)physAddr, length);
    fflush(stdout);
    if ((physAddr & 0x3fff) != 0 || (length & 0x3fff) != 0) {
        return 0x80020016; // ORBIS_KERNEL_ERROR_EINVAL
    }
    if (length == 0) return 0;
    return sceKernelReleaseDirectMemory(physAddr, length);
}

void shim_sceKernelCheckedReleaseDirectMemory(GuestContext *ctx) {
    off_t physAddr = (off_t)ctx->rdi;
    size_t length = (size_t)ctx->rsi;
    int rc = sceKernelCheckedReleaseDirectMemory(physAddr, length);
    ctx->rax = (uint64_t)(int64_t)rc;
    SHIM_RETURN();
}

// sceKernelMprotect(addr, len, prot): the mapped part of [addr, addr+len) gets the protection, with
// extents split at the range edges (the rest of a mapping keeps what it had) and the CPU view of
// it applied to the host pages, so a store to a PROT_READ page or a load from a PROT_NONE one is a
// classified fault, as on the console (recomp_vm_protect). A range that wraps, leaves the guest
// window or maps nothing is EINVAL.
int sceKernelMprotect(GuestContext *ctx, const void *addr, size_t size, int prot) {
    if (!ctx) return 0x80020016;
    const int rc = recomp_vm_protect(ctx, (uint64_t)addr, size, prot, -1);
    return rc == 0 ? 0 : rc == ENOMEM ? 0x8002000c : 0x80020016;
}

int hle_sceKernelMprotect(GuestContext *ctx, const void *addr, size_t len, int prot) {
    return sceKernelMprotect(ctx, (const void *)(uintptr_t)NPS5_H2G(ctx, addr), len, prot);
}


// ---------------------------------------------------------------------------------
// sceKernelMtypeprotect / sceKernelBatchMap
// ---------------------------------------------------------------------------------

void ps4_gnm_notify_cpu_write(uint64_t addr, uint64_t size) __attribute__((weak));

// int sceKernelMtypeprotect(void *addr, size_t len, int mtype, int prot)
//
// Changes the protection and the memory type (cache attribute: 0 WB_ONION, 3 WC_GARLIC,
// 10 WB_GARLIC ...) of the mapped part of [addr, addr+len). Behaviour follows shadPS4
// (libraries/kernel/memory.cpp sceKernelMtypeprotect): the range is widened to 16 KiB pages,
// the protection is applied, the type is recorded, GPU-visible copies are invalidated. A range
// without any mapping fails with EINVAL. The host maps guest memory once with full rights
// (there is no per-range host mprotect, see sceKernelMprotect), so protection and type are
// bookkeeping that sceKernelVirtualQuery and later calls observe; extents are split at the
// range edges so a small range inside a large mapping does not retag the whole mapping.
int sceKernelMtypeprotect(GuestContext *ctx, const void *addr, size_t size, int mtype, int prot) {
    if (!ctx) return 0x80020016; // ORBIS_KERNEL_ERROR_EINVAL
    if (size == 0) return 0;
    const int rc = recomp_vm_protect(ctx, (uint64_t)addr, size, prot, mtype);
    if (rc != 0) return rc == ENOMEM ? 0x8002000c : 0x80020016;

    uint64_t start = (uint64_t)addr & ~0x3fffULL;
    uint64_t span = (((uint64_t)addr - start) + size + 0x3fffULL) & ~0x3fffULL;
    if (ps4_gnm_notify_cpu_write) ps4_gnm_notify_cpu_write(start, span);
    return 0;
}

void shim_sceKernelMtypeprotect(GuestContext *ctx) {
    int rc = sceKernelMtypeprotect(ctx, (const void *)ctx->rdi, (size_t)ctx->rsi, (int)ctx->rdx, (int)ctx->rcx);
    ctx->rax = (uint64_t)(int64_t)rc;
    SHIM_RETURN();
}

// int sceKernelBatchMap(SceKernelBatchMapEntry *entries, int numEntries, int *numEntriesOut)
//
// Runs the entries in order until the first failure. *numEntriesOut (may be NULL) receives the
// number of entries that completed, written on failure too; the failed entry is not counted.
// The result is the failed entry's error, or 0. Entry layout, operation numbers and the
// failure rules follow shadPS4 (libraries/kernel/memory.cpp sceKernelBatchMap2, SDK struct
// SceKernelBatchMapEntry): length == 0 or an unknown operation fails with EINVAL, a NULL
// array with a positive count too. The map operations pass MAP_FIXED (0x10), exactly like
// sceKernelBatchMap does on the console; `start` is the requested address (0 = anywhere) and
// receives the mapped address.
// The map operations pass MAP_FIXED. When an address was requested and the allocator placed the
// mapping elsewhere (it falls back to any free range when the requested one is taken) the
// console fails the entry; undo the stray mapping and fail it too.
static int batch_fixed_result(GuestContext *ctx, int result, uint64_t requested, void *mapped, uint64_t length) {
    if (result != 0 || requested == 0 || (uint64_t)mapped == requested) return result;
    recomp_vm_free(ctx, (uint64_t)mapped, (size_t)length);
    return 0x8002000c; // ORBIS_KERNEL_ERROR_ENOMEM
}

int sceKernelBatchMap(GuestContext *ctx, OrbisKernelBatchMapEntry *entries, int numEntries, int *numEntriesOut) {
    const int kFlagsMapFixed = 0x10;
    int result = 0;
    int processed = 0;
    for (int i = 0; i < numEntries; i++) {
        OrbisKernelBatchMapEntry *e = entries ? &entries[i] : NULL;
        if (!e || e->length == 0 || (uint32_t)e->operation > ORBIS_KERNEL_BATCH_MAP_TYPE_PROTECT) {
            result = 0x80020016; // ORBIS_KERNEL_ERROR_EINVAL
            break;
        }
        const int prot = (uint8_t)e->protection;
        void *mapped = (void *)e->start;
        // start == 0 asks for any address; a fixed map at 0 is refused, so it goes without MAP_FIXED.
        const int mapflags = e->start ? kFlagsMapFixed : 0;
        switch (e->operation) {
        case ORBIS_KERNEL_BATCH_MAP_DIRECT:
            result = sceKernelMapDirectMemory(ctx, &mapped, (size_t)e->length, prot, mapflags,
                                              (off_t)e->offset, 0);
            result = batch_fixed_result(ctx, result, e->start, mapped, e->length);
            if (result == 0) e->start = (uint64_t)mapped;
            break;
        case ORBIS_KERNEL_BATCH_MAP_UNMAP:
            result = recomp_vm_free(ctx, e->start, (size_t)e->length) == 0 ? 0 : 0x80020016;
            break;
        case ORBIS_KERNEL_BATCH_MAP_PROTECT:
            result = sceKernelMprotect(ctx, (const void *)e->start, (size_t)e->length, prot);
            break;
        case ORBIS_KERNEL_BATCH_MAP_FLEXIBLE:
            result = sceKernelMapNamedFlexibleMemory(ctx, &mapped, (size_t)e->length, prot, mapflags, "anon");
            result = batch_fixed_result(ctx, result, e->start, mapped, e->length);
            if (result == 0) e->start = (uint64_t)mapped;
            break;
        case ORBIS_KERNEL_BATCH_MAP_TYPE_PROTECT:
            result = sceKernelMtypeprotect(ctx, (const void *)e->start, (size_t)e->length, (uint8_t)e->type, prot);
            break;
        }
        if (result != 0) break;
        processed++;
    }
    if (numEntriesOut) *numEntriesOut = processed;
    return result;
}

void shim_sceKernelBatchMap(GuestContext *ctx) {
    OrbisKernelBatchMapEntry *entries =
        ctx->rdi ? (OrbisKernelBatchMapEntry *)(ctx->mem_base + ctx->rdi) : NULL;
    int *out = ctx->rdx ? (int *)(ctx->mem_base + ctx->rdx) : NULL;
    int rc = sceKernelBatchMap(ctx, entries, (int)ctx->rsi, out);
    ctx->rax = (uint64_t)(int64_t)rc;
    SHIM_RETURN();
}
