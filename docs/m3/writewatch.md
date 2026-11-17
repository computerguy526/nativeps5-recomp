# The AGC write watch and the runtime (m3-wwatch)

AnyPS5's AGC driver can watch guest pages: it write-protects them on the host and takes the first store as a fault
(write watch), and it can make pages inaccessible so that a GPU result stays on the GPU until the CPU touches them (read
traps). With the watch the driver answers "did the CPU change this texture or buffer?" from page stamps. Without it,
it keeps a CPU copy of every cached storage image and compares bytes: on PPSA30552 that was 30 to 56 ms of memcmp per
frame and 1.2 GB of copies. Until this branch nothing the runtime maps was watched. The GPU-side design is AnyPS5
`docs/nps5-coherence.md` (branch `nps5-coherence`).

## Why the first attempt crashed

Registering ranges from `AgcGuestAllocationRegister` alone (cpudraw's experiment) crashed Greenhollow at startup: a
SIGBUS on a guest store to a watched page was not resolved. The VM layer calls `on_protect` before the host `mprotect`.
For a range that goes from `PROT_NONE` to read-write the bridge registered it with the watch at that point, and the
watch read the host protection of the pages, still none, as the guest's. It then refused the first store ("the guest
has no write access here"), the fault reached the crash report, and the report died on its own locks.

## What the runtime owes the watch

| Duty | Where |
|---|---|
| Every host protection change of guest pages goes through the watch, so it records the guest's protection when it changes and keeps its own on top | `nps5_mem_host_protect` (used by `nps5_mem_commit` and `nps5_mem_protect_inner`), hook `protect` = `GuestWriteWatchProtect_nid_postfix` |
| Every memory fault is offered to the watch before it is a crash, on every thread, with the kind of access | `nps5_diag_add_access_resolver`; the crash handler computes store/load from ESR and calls `GuestWriteWatchHandleAccess_nid_postfix` |
| A kernel-mode write into guest memory holds a host write (the kernel does not fault into the handler: `read()` into a protected page is `EFAULT`) | `nps5_kio.h` wrappers, hook `write_begin`/`write_end` = `GuestArenaBeginHostWrite`/`EndHostWrite` |
| A kernel-mode read of guest memory first brings trapped pages up to date | `nps5_kio.h`, `nps5_mem_safe_read`, `nps5_mem_range_readable`; hook `read_prepare` = `GuestArenaPrepareHostRead` |
| Memory with two guest addresses is never watched (a store through one address does not trip the other's protection) | `Nps5VmHooks.on_alias`, reported by `sceKernelMapDirectMemory` before the second mapping's `on_map`; the bridge keeps an alias list and calls `AgcGuestAllocationWatch(..., 0)` |
| Synchronous fault signals are never blocked | `shim_sigprocmask` strips SIGSEGV, SIGBUS, SIGILL and SIGTRAP from a blocking mask |

The hooks are generic (`Nps5HostAccessHooks` in `nps5_mem.h`); the AnyPS5 bridge (`modules/libSceAnyPS5/nps5_anyps5.c`)
installs them. It looks the watch's entry points up by name, so the runtime still runs on AnyPS5 libraries without
them: nothing is watched then, as before. `NPS5_AGC_WRITE_WATCH=0` turns the watch off on both sides.

Which ranges are watched: every range registered with the GPU flag (direct and flexible memory, and mappings with a GPU
protection bit), unless aliased. Registration alone protects nothing; the driver protects a page when it collects it,
which it does only for memory it validated as a texture or buffer.

## The fault path

`crash_handler` (SIGSEGV/SIGBUS, `SA_SIGINFO | SA_ONSTACK`, empty mask):

1. shadow-stack guard, 2. host-call bridge, 3. the resolvers, each with `(address, is_write)`; 4. only then the crash
report.

A resolver takes no runtime lock. The watch's own lock is a plain mutex taken for a few instructions; a store to a
protected page costs one signal and one `mprotect`. A load from a trapped page waits in the handler on a Mach semaphore
until the watch's resolver thread has stored the image; the handler runs on the faulting thread's own stack (only the
main thread has an alternate stack). `errno` is saved and restored around the resolvers. Once the handler decides the
fault is a crash it sets `g_nps5_in_crash_handler` on the thread, and no hook runs under the report (the report reads
guest memory with `nps5_mem_safe_read`).

A fault inside a resolver arrives with the signal blocked and ends the process at once (the kernel's rule); the test
checks it does not hang.

## Kernel I/O (`core/nps5_kio.h`)

`nps5_kread`, `nps5_kpread`, `nps5_kreadv`, `nps5_kpreadv`, `nps5_krecv`, `nps5_krecvfrom`, `nps5_krecvmsg`,
`nps5_kfread`, `nps5_kaccept`, `nps5_kgetsockname`, `nps5_kgetpeername`, `nps5_kgetsockopt`, `nps5_kselect` hold a host
write over what the kernel fills; `nps5_kwrite`, `nps5_kpwrite`, `nps5_kwritev`, `nps5_ksend`, `nps5_ksendto`,
`nps5_ksendmsg`, `nps5_kfwrite` prepare the source. For a buffer outside the guest reservation, and without hooks, a
wrapper costs two compares. Applied in `ps4_syscalls.c`, `ps4_posix.c`, `ps4_net.c`, `nps5_libc_stdio.c`, `ps4_aio.c`,
`nps5_ampr.c`, `ps4_savedata.c`; `ps4_vfs_read`/`ps4_vfs_pread` bracket the whole call.

**Rule for new HLE code:** a guest pointer handed to a system call, or to a library that fills it from another thread,
goes through `nps5_kio.h` or an explicit `nps5_host_write_begin`/`end`. Plain C stores and loads need nothing.

Not wrapped (known gaps): `ioctl` with a guest argument, `stat` family results (the callers convert through a host
struct), paths read by the kernel from guest memory (resolved through the VFS copy first), and `setvbuf` (a no-op here).

## Tests

- `TestHostAccessHooks` (`tests/memory_vm_test/watch_test.c`, a small page watch standing in for AnyPS5's): 8 threads
  storing to 192 pages while the main thread re-arms them (every store resolved, none lost, none reported as a load);
  loads from trapped pages on two threads waiting for a resolver thread; `read()` into a protected page (`EFAULT` bare,
  works through `nps5_kread` and is recorded); `write()` from a trapped page; `nps5_mem_safe_read` and
  `nps5_mem_range_readable` on trapped pages; the protect hook; a fault inside the resolver and an unclaimed fault both
  end the process within the time limit. Run 5 times per test invocation.
- `TestGuestVM`: the alias report for a physical range mapped twice (addresses, size, before `on_map`).
- `TestDiagFaultResolver`: the plain resolvers still work.
- AnyPS5 `core/libs/tests/write_watch` mode `host-runtime`: the real watch behind a foreign handler.

## Open

- Homebrew gate and title runs with the watch on are pending (held while integration had the machine).
- `go test ./pkg/runtime/...` passes; `tools/libc_unit.sh` fails to link on the base commit already (unrelated symbols).
