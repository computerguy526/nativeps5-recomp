/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef KYTY_HYBRID_KYTYGPUAPI_H_
#define KYTY_HYBRID_KYTYGPUAPI_H_

/* The C interface between a host process and KytyPS5's GPU stack built as a library (kyty_gpu).
 *
 * Origin: AstroHybrid's src/hybrid/kytyGpuApi.h (ABI 1.0-1.5; KytyPS5 by Jetsku, hybrid bridge by the
 * AstroHybrid project, both GPL-2.0). ABI 1.6 below is the nativeps5 extension for a host that is NOT
 * AnyPS5-on-Windows: the ps5recomp runtime on arm64 macOS (docs/m3/kyty-seam.md).
 *
 * What is different on a 1.6 host (KYTY_GPU_HOST_HOSTED_MEMORY in KytyGpuHostInfo.flags):
 *  - The HOST owns guest memory. The library never allocates, maps or protects guest memory itself: the
 *    host reports the GPU-visible ranges (register_memory / unregister_memory / protect_memory) and the
 *    library asks the host to arm its page watches (protect_guest). donate_guest_range is null.
 *  - Guest addresses are not host addresses: host = guest + KytyGpuHostInfo.guest_bias (2^40 for ASTRO BOT).
 *    Every address in this interface is a GUEST address unless it is called a host pointer.
 *  - Host pages are 16 KiB. The library keeps tracking 4 KiB pages (KytyGpuHostInfo.tracker_page_size);
 *    the host folds four tracker pages into one host page (strictest watch wins) and, on a fault, calls
 *    handle_fault for every armed tracker page of the faulting host page.
 *  - Exports are native functions of the host ABI (AAPCS64), found through nid_entry (or dlsym of the NID),
 *    with the C prototype of the PS5 function. The host's thunks read the guest's System V registers.
 *
 * Versioning: the host passes KYTY_GPU_ABI_VERSION and sizeof() of its structs. The library refuses a
 * different major version or a struct smaller than the fields it reads; fields are only ever appended. */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KYTY_GPU_ABI_VERSION_MAJOR 1u
#define KYTY_GPU_ABI_VERSION_MINOR 8u
#define KYTY_GPU_ABI_VERSION ((KYTY_GPU_ABI_VERSION_MAJOR << 16u) | KYTY_GPU_ABI_VERSION_MINOR)

#if defined(_WIN32)
#if defined(KYTY_GPU_BUILDING_DLL)
#define KYTY_GPU_EXPORT __declspec(dllexport)
#else
#define KYTY_GPU_EXPORT __declspec(dllimport)
#endif
#else
#define KYTY_GPU_EXPORT __attribute__((visibility("default")))
#endif

typedef enum KytyGpuStatus {
	KYTY_GPU_OK                = 0,
	KYTY_GPU_E_VERSION         = -1, /* major version or struct size not supported */
	KYTY_GPU_E_ARGUMENT        = -2,
	KYTY_GPU_E_STATE           = -3, /* called in the wrong lifecycle state */
	KYTY_GPU_E_NOT_IMPLEMENTED = -4,
	KYTY_GPU_E_HOST_CALLBACK   = -5, /* a required host callback is missing */
	KYTY_GPU_E_UNKNOWN_NID     = -6,
} KytyGpuStatus;

/* How an exported NID reaches its implementation. */
typedef enum KytyGpuBinding {
	KYTY_GPU_BINDING_KYTY_VERIFIED = 1, /* Kyty's HLE function; argument layout compared with AnyPS5's */
	KYTY_GPU_BINDING_KYTY          = 2, /* Kyty's HLE function; game ABI, layout not yet compared */
	KYTY_GPU_BINDING_BRIDGE        = 3, /* implemented by the bridge (host handles) */
} KytyGpuBinding;

typedef struct KytyGpuNidInfo {
	const char* nid;      /* export name, e.g. "UglJIZjGssM" */
	const char* library;  /* prx the game imports it from, e.g. "libSceAgcDriver.prx" */
	const char* function; /* implementation name, for logs */
	uint32_t    binding;  /* KytyGpuBinding */
	uint32_t    bound;    /* nonzero once the export reaches its implementation */
} KytyGpuNidInfo;

/* Memory flags for register_memory / protect_memory. */
#define KYTY_GPU_MEMORY_GPU_READ  0x1u
#define KYTY_GPU_MEMORY_GPU_WRITE 0x2u
#define KYTY_GPU_MEMORY_DIRECT    0x4u /* direct memory */
#define KYTY_GPU_MEMORY_FLEXIBLE  0x8u /* 1.6: flexible memory */
#define KYTY_GPU_MEMORY_CPU_READ  0x10u /* 1.6: the guest's own protection of the range */
#define KYTY_GPU_MEMORY_CPU_WRITE 0x20u

/* GPU -> host event kinds (host_callbacks.deliver_event). */
#define KYTY_GPU_EVENT_EOP_INTERRUPT 1u /* ident: AGC event id (queue), data: unused */
#define KYTY_GPU_EVENT_FLIP          2u /* ident: video out handle, data: flip argument */
#define KYTY_GPU_EVENT_VBLANK        3u /* ident: video out handle */

/* ABI 1.2: one pad's state as the library's window reads it (SCE button bits). */
typedef struct KytyGpuPadTouch {
	uint16_t x;
	uint16_t y;
	uint8_t  id;
	uint8_t  active;
	uint8_t  reserved[2];
} KytyGpuPadTouch;

typedef struct KytyGpuPadState {
	uint32_t        buttons;
	uint8_t         sticks[4]; /* left x, left y, right x, right y; 128 is centred */
	uint8_t         l2;
	uint8_t         r2;
	uint8_t         connected;
	uint8_t         reserved;
	KytyGpuPadTouch touch[2];
} KytyGpuPadState;

/* ABI 1.4: pad output the game asked for through the host's pad library. */
typedef struct KytyGpuPadOutput {
	uint32_t sequence;
	uint8_t  vibration_large;
	uint8_t  vibration_small;
	uint8_t  audio_vibration_large;
	uint8_t  audio_vibration_small;
	uint8_t  light_bar_valid;
	uint8_t  light_bar[3];
	uint8_t  trigger_set[2];
	uint8_t  reserved[2];
	uint8_t  trigger_command[2][56];
} KytyGpuPadOutput;

/* ABI 1.5: one pad's motion sensors (SDL axes): m/s^2 including gravity, rad/s. */
typedef struct KytyGpuPadMotion {
	float acceleration[3];
	float angular_velocity[3];
} KytyGpuPadMotion;

/* ABI 1.6: what the host process looks like. Constant after init. */
#define KYTY_GPU_HOST_HOSTED_MEMORY 0x1u /* the host owns guest memory (see the top of this file) */
#define KYTY_GPU_HOST_MAIN_QUEUE    0x2u /* run_on_main_thread works: the host's main thread serves the main queue */
#define KYTY_GPU_HOST_SCRIPTED_PAD  0x4u /* the host feeds the pad itself (NPS5_VPAD_SCRIPT); publish_pad is optional */

typedef struct KytyGpuHostInfo {
	uint32_t struct_size;
	uint32_t flags;             /* KYTY_GPU_HOST_* */
	uint64_t guest_bias;        /* host address = guest address + guest_bias (0: identity) */
	uint64_t guest_lo;          /* first valid guest address */
	uint64_t guest_hi;          /* end of the guest address space */
	uint32_t host_page_size;    /* 16384 on arm64 macOS */
	uint32_t tracker_page_size; /* granularity of protect_guest / handle_fault: 4096 */
} KytyGpuHostInfo;

/* protect_guest watch modes: what a CPU access to the tracker pages must trap. */
#define KYTY_GPU_WATCH_NONE   0u /* no trap (the guest's own protection applies) */
#define KYTY_GPU_WATCH_WRITE  1u /* trap the first store */
#define KYTY_GPU_WATCH_ACCESS 2u /* trap the first load or store */

/* clang-format off */ /* (the Kyty copy of this header keeps its tab style, byte for byte) */
/* ---- ABI 1.8 (nativeps5): UE5 shader library extraction ----
 * Called by the library from a host thread (never a guest thread), before or while the game runs. The host reads every IoStore
 * container under app0 (through its file layer: plain files and .nps5seg packs), parses the shader library chunks (type 8), and
 * for each shader group (type 9 chunk) decodes it by calling the guest decoder at `decoder` with convention `abi` on a guest
 * context the host owns (own guest stack + decoder memory), then calls `group` with the decoded bytes. Groups are taken in
 * container/library order (with threads > 1 they are delivered as they complete, so the order of delivery is only guaranteed
 * with threads = 1); decoded bytes are valid only during the call. progress(done, total) counts groups (total known after
 * the TOCs are read). Returns 0 = done (see out), -1 unsupported (no IoStore / abi unknown), -2 decoder call failed on the first
 * group, -3 cancelled. Null on hosts older than 1.8 (struct_size tells). */
typedef struct KytyGpuUe5Group {
	uint32_t library;            /* index of the library in delivery order */
	uint32_t group;              /* group index in its library */
	uint32_t shader_count;       /* from the library's group entry */
	uint32_t raw_bytes;          /* decoded size (= the library's uncompressed size) */
	const uint32_t* frequencies; /* shader_count entries: low 4 bits of the library shader entry (stage) */
	const uint8_t*  bytes;       /* raw_bytes decoded bytes */
} KytyGpuUe5Group;
typedef struct KytyGpuUe5Query {
	uint32_t struct_size;
	uint32_t abi;               /* 1 = oodlelz14 */
	uint64_t decoder;           /* guest vaddr */
	uint32_t max_raw_bytes;     /* a group above this is skipped and counted (default 64 MiB) */
	uint32_t threads;           /* decode contexts (1..8; 0 = host default 2) */
	int  (*group)(void* arg, const KytyGpuUe5Group* g);     /* nonzero = stop (cancel) */
	void (*progress)(void* arg, uint32_t done, uint32_t total);
	void* arg;
} KytyGpuUe5Query;
typedef struct KytyGpuUe5Result {
	uint32_t containers, libraries, groups_total, groups_decoded, groups_failed, groups_skipped;
	uint64_t compressed_bytes, raw_bytes, ms;
} KytyGpuUe5Result;
/* clang-format on */

typedef struct KytyGpuHostCallbacks {
	uint32_t struct_size; /* sizeof(KytyGpuHostCallbacks) on the host */
	uint32_t abi_version; /* KYTY_GPU_ABI_VERSION of the host */
	void*    user;        /* passed back to every callback */

	/* Diagnostics. level: 0 info, 1 warning, 2 error. May be null. */
	void (*log)(void* user, int level, const char* message);

	/* The game's event queues live in the host's libkernel. The bridge forwards the AGC and VideoOut
	 * event registrations to the host, which owns the queue handles, and raises events through
	 * deliver_event. Return 0 or a negative SCE error. udata is a guest value handed back in the event. */
	int (*add_gpu_event)(void* user, int64_t equeue, uint32_t kind, int64_t ident, void* udata);
	int (*delete_gpu_event)(void* user, int64_t equeue, uint32_t kind, int64_t ident);
	void (*deliver_event)(void* user, uint32_t kind, int64_t ident, uint64_t data);

	/* The registered range holding `address` (guest). Return 0 and fill the outputs, or a negative value
	 * when it is not registered. alias: an always-readable-and-writable host view of the same bytes
	 * (never armed by protect_guest), or null. */
	int (*query_memory)(void* user, uint64_t address, uint64_t* base, uint64_t* size,
	                    uint32_t* flags, const void** alias);

	/* The title's app0 directory (UTF-8), for shader and pipeline caches. May be null. */
	const char* (*app0_path)(void* user);

	/* ABI 1.1 (AnyPS5 on Windows only). Null on a hosted-memory host. */
	int (*donate_guest_range)(void* user, uint64_t size, uint64_t* base);

	/* ABI 1.2. The library owns the window and reads the pads; it publishes each pad's state here. */
	void (*publish_pad)(void* user, int port, const KytyGpuPadState* state);

	/* ABI 1.3. The player closed the library's window: the host ends the process its usual way. The
	 * renderer keeps running meanwhile. */
	void (*request_exit)(void* user, int code);

	/* ABI 1.4. Pad output the game requested since *seen. May be null. */
	int (*fetch_pad_output)(void* user, int port, uint32_t* seen, KytyGpuPadOutput* out);

	/* ABI 1.5. May be null. */
	void (*publish_pad_motion)(void* user, int port, const KytyGpuPadMotion* motion);

	/* ---- ABI 1.6 (nativeps5) ---- */

	/* Never null on a 1.6 host. */
	const KytyGpuHostInfo* (*host_info)(void* user);

	/* Arms or disarms the library's page watch on guest [address, address+size), both multiples of
	 * tracker_page_size, inside registered memory. The host combines it with the guest's own protection
	 * and with the other tracker pages of the same host page and changes the host protection of the
	 * guest's view (never of the alias). Callable from any thread, also from inside handle_fault.
	 * Returns 0, or a negative value (range not registered). */
	int (*protect_guest)(void* user, uint64_t address, uint64_t size, uint32_t watch);

	/* Runs fn(arg) on the process's main thread (macOS: windows and the event pump live there). wait != 0
	 * returns after fn did. fn may run for the life of the process when it pumps the main run loop itself
	 * (SDL does). Returns 0, or a negative value when the host has no main queue. */
	int (*run_on_main_thread)(void* user, void (*fn)(void* arg), void* arg, int wait);

	/* Calls the guest function at guest address `function` with up to six integer arguments (System V
	 * order) and returns its RAX. Only on a guest thread that is inside one of the library's exports
	 * (e.g. a command-buffer out-of-space callback); elsewhere it returns ~0 and logs. */
	uint64_t (*call_guest)(void* user, uint64_t function, const uint64_t* args6);

	/* ---- ABI 1.7 (nativeps5): a trapped load the library completes itself ----
	 *
	 * Only inside handle_fault(page, is_write = 0) for the page the load actually faulted on. fault_load
	 * returns 1 and what the trapped instruction reads (guest address, 1..16 bytes) when the host can
	 * complete it (arm64 LDR/LDUR/LDP forms, general and SIMD registers); 0 otherwise (not such a load,
	 * a neighbour's report, a system-call buffer). fault_load_apply completes it with `bytes` as the
	 * loaded memory: the thread continues behind the instruction and the page STAYS armed. This is what
	 * GuestLoad::Decode / GuestLoad::Apply are on x86 (KYTY_READ_FAULT_PUBLISHED): a GPU-owned page the
	 * game polls every frame is served from its published copy instead of a GPU drain per read.
	 * Both may be null on a 1.6 host (struct_size tells). */
	int (*fault_load)(void* user, uint64_t* address, uint32_t* size);
	int (*fault_load_apply)(void* user, const void* bytes, uint32_t size);

        /* clang-format off */
	/* ---- ABI 1.8 (nativeps5) ---- see KytyGpuUe5Query above. May be null (struct_size tells). */
	int (*extract_ue5_shader_groups)(void* user, const KytyGpuUe5Query* q, KytyGpuUe5Result* out);
        /* clang-format on */
} KytyGpuHostCallbacks;

/* The callback table of a 1.3 host, the oldest the Windows library accepts. */
#define KYTY_GPU_HOST_CALLBACKS_SIZE_1_3 offsetof(KytyGpuHostCallbacks, fetch_pad_output)
#define KYTY_GPU_HOST_CALLBACKS_SIZE_1_5 offsetof(KytyGpuHostCallbacks, host_info)
#define KYTY_GPU_HOST_CALLBACKS_SIZE_1_6 offsetof(KytyGpuHostCallbacks, fault_load)
#define KYTY_GPU_HOST_CALLBACKS_SIZE_1_7 offsetof(KytyGpuHostCallbacks, extract_ue5_shader_groups)

/* AnyPS5-on-Windows lazy start (unused by a 1.6 host, which always calls init first). */
#define KYTY_GPU_HOST_MODULE_DEFAULT "libAstroHybridHost.prx"
#define KYTY_GPU_HOST_ENTRY "AstroHybridHostCallbacks_nid_no_patch"
typedef const KytyGpuHostCallbacks* (*KytyGpuHostEntryFn)(void);

typedef struct KytyGpuPacket { /* identical to AGC's Packet */
	uint32_t* addr;
	uint32_t  dw_num;
	uint8_t   flags;
	uint8_t   reserved[3];
} KytyGpuPacket;

/* KytyGpuApi.pointer_mode: what the host's thunks pass for a POINTER parameter of an export. */
#define KYTY_GPU_POINTERS_GUEST 0u /* the guest's register value, untouched; pointer results are guest values */
#define KYTY_GPU_POINTERS_HOST  1u /* guest + bias (null stays null); pointer results are host pointers.
                                      Addresses INSIDE guest data (PM4 words, Packet.addr, shader headers)
                                      are guest values in both modes */

typedef struct KytyGpuApi {
	uint32_t struct_size;
	uint32_t abi_version;
	const char* build; /* git version of the library */

	/* Lifecycle. init binds every export (no graphics device yet); start_graphics creates the Vulkan
	 * renderer and presentation; shutdown stops them. A 1.6 host calls init, then register_memory for
	 * everything mapped, then start_graphics, all before the first guest thread runs. */
	int (*init)(const KytyGpuHostCallbacks* host);
	int (*start_graphics)(void);
	void (*shutdown)(void);

	/* Guest memory notifications (guest addresses). Hosted memory: register_memory may name a range that
	 * overlaps registered ones (it replaces them); unregister_memory may name any part of them (the rest
	 * stays) and is called BEFORE the host releases the pages, so it may wait for GPU work that still
	 * reads them; when it returns the library has disarmed every watch in the range. */
	int (*register_memory)(uint64_t base, uint64_t size, uint32_t flags, const void* alias);
	int (*unregister_memory)(uint64_t base, uint64_t size);
	int (*invalidate_memory)(uint64_t base, uint64_t size); /* the CPU wrote the range behind the watch */

	/* Command submission (same semantics as the AGC driver calls). */
	int (*submit_dcb)(const KytyGpuPacket* packet);
	int (*submit_acb)(uint32_t queue, const KytyGpuPacket* packet);

	/* Export table: count and the entry for an index or NID (null when out of range / unknown). */
	uint32_t (*nid_count)(void);
	const KytyGpuNidInfo* (*nid_info)(uint32_t index);
	const KytyGpuNidInfo* (*find_nid)(const char* nid);

	/* ---- ABI 1.6 (nativeps5) ---- */

	uint32_t pointer_mode; /* KYTY_GPU_POINTERS_* */
	uint32_t reserved0;

	/* The native entry point of an export (host ABI, C prototype of the PS5 function), or null. When
	 * this field is null the host uses dlsym(library, nid). */
	void* (*nid_entry)(const char* nid);

	/* A CPU access trapped on a tracker page the library armed (a guest address inside the page: the
	 * faulting byte for the page that faulted, the page's first byte for a neighbour; is_write: 1 a
	 * store, 0 a load). Called in the faulting thread's signal handler, or from a host thread before a
	 * system call reads or writes the page. The library brings the page up to date and disarms what the
	 * access needs through protect_guest before it returns. A 16 KiB host page holds four tracker pages:
	 * for a store the host also calls it for the WRITE-watched neighbours of the faulting page (reported
	 * written although they are not: that opens the host page); an ACCESS-watched neighbour is never
	 * reported (the host carries accesses to its neighbours out on the alias while it stays armed).
	 * Returns 1 when the page was the library's. */
	int (*handle_fault)(uint64_t address, int is_write);

	/* The guest changed the protection of part of a registered range (flags as in register_memory). */
	int (*protect_memory)(uint64_t base, uint64_t size, uint32_t flags);

        /* clang-format off */
	/* ---- ABI 1.8 (nativeps5) ---- May be null (struct_size tells).
	 * Called once by the host on its launch thread after the guest image, static TLS and preloaded modules are up and before
	 * the guest entry runs; only when the host set extract_ue5_shader_groups. May block for the whole launch phase (the host's
	 * main thread keeps running its window loop meanwhile). extract_ue5_shader_groups may be called from inside it, and from
	 * other host threads. */
	int (*guest_ready)(void);
        /* clang-format on */
} KytyGpuApi;

#define KYTY_GPU_API_SIZE_1_5 offsetof(KytyGpuApi, pointer_mode)
#define KYTY_GPU_API_SIZE_1_7 offsetof(KytyGpuApi, guest_ready)

/* The library's only C entry point; the host asks for the version it was built with. Returns null for
 * an unsupported version. */
KYTY_GPU_EXPORT const KytyGpuApi* KytyGpuGetApi(uint32_t abi_version);
typedef const KytyGpuApi* (*KytyGpuGetApiFn)(uint32_t abi_version);

#ifdef __cplusplus
}
#endif

#if defined(__cplusplus)
static_assert(sizeof(KytyGpuPadState) == 28, "KytyGpuPadState layout");
static_assert(sizeof(KytyGpuPacket) == 16, "KytyGpuPacket layout");
static_assert(offsetof(KytyGpuPacket, dw_num) == 8, "KytyGpuPacket layout");
static_assert(offsetof(KytyGpuPacket, flags) == 12, "KytyGpuPacket layout");
static_assert(sizeof(KytyGpuHostInfo) == 40, "KytyGpuHostInfo layout");
#else
_Static_assert(sizeof(KytyGpuPadState) == 28, "KytyGpuPadState layout");
_Static_assert(sizeof(KytyGpuPacket) == 16, "KytyGpuPacket layout");
_Static_assert(offsetof(KytyGpuPacket, dw_num) == 8, "KytyGpuPacket layout");
_Static_assert(offsetof(KytyGpuPacket, flags) == 12, "KytyGpuPacket layout");
_Static_assert(sizeof(KytyGpuHostInfo) == 40, "KytyGpuHostInfo layout");
#endif

#endif /* KYTY_HYBRID_KYTYGPUAPI_H_ */
