#include "nps5_sleep.h"
#include "ps4_syscalls.h"
#include "ps4_hitch.h"
#include "ps4_vfs.h"
#include "recomp_runtime.h"
#include "nps5_thunk.h"
#include "nps5_libkernel_hle.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "ps4_threading.h"
#include "nps5_kio.h"

static void guest_dir_cache_drop_fd(int fd);

// Host (macOS) errno -> FreeBSD/Orbis errno. The two agree below 90 except for the entries
// below; unmapped values pass through.
static inline int bsd_errno_from_host(int err) {
  switch (err) {
  case 93: return 87;  // ENOATTR
  default: break;
  }
  return err;
}

// SCE kernel error convention: 0x80020000 | FreeBSD errno (AnyPS5 KernelErrors.hpp SceKernelError).
static inline uint64_t sce_kernel_error(int err) {
  return (uint64_t)(int64_t)(int32_t)(0x80020000 | (uint32_t)bsd_errno_from_host(err));
}

// Remember the last errno handed to set_guest_errno so the sceKernel* wrappers of POSIX shims
// can turn the -1/errno result into an SCE error code. libc keeps -1/errno.
static __thread int t_last_errno;
#define set_guest_errno(ctx_, err_)                                            \
  do {                                                                         \
    int e__ = (err_);                                                          \
    t_last_errno = e__;                                                        \
    (set_guest_errno)((ctx_), e__);                                            \
  } while (0)

static void sce_wrap(GuestContext *ctx, void (*posix)(GuestContext *)) {
  t_last_errno = 0;
  posix(ctx);
  if ((int64_t)ctx->rax == -1 && t_last_errno)
    ctx->rax = sce_kernel_error(t_last_errno);
}

// PS4 specific: sceKernelUsleep sleeps for microseconds
void shim_sceKernelUsleep(GuestContext *ctx) {
  uint64_t ns = (uint64_t)(uint32_t)ctx->rdi * 1000ull;
  uint64_t rem = 0;
  // NPS5_TRACE_CONDWAIT=<ms> also reports sleeps of at least that long (requested and actual), same clock as the [condwait] lines.
  static int trace_ms = -1;
  if (trace_ms < 0) {
    const char *e = getenv("NPS5_TRACE_CONDWAIT");
    trace_ms = e ? (atoi(e) > 0 ? atoi(e) : 1) : 0;
  }
  const uint64_t t0 = trace_ms ? clock_gettime_nsec_np(CLOCK_UPTIME_RAW) : 0;
  const uint32_t req_usec = (uint32_t)ctx->rdi;
  // sceKernelUsleep is not interrupted: after a signal sleep the remaining time (not 0).
  while (ns && nps5_sleep_ns(ns, &rem) != 0 && errno == EINTR) ns = rem;
  if (trace_ms) {
    const uint64_t t1 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if ((t1 - t0) / 1000000ull >= (uint64_t)trace_ms) {
      uint64_t ret_addr = 0;
      (void)nps5_mem_safe_read(ctx->mem_base + ctx->rsp, &ret_addr, sizeof ret_addr);
      fprintf(stderr, "[usleep] req_ms=%.0f ms=%.0f end=%.0f tid=%llu ret=0x%llx\n", req_usec / 1e3, (double)(t1 - t0) / 1e6, (double)t1 / 1e6, (unsigned long long)(uintptr_t)pthread_self(), (unsigned long long)ret_addr);
      // guest frame-pointer chain (UE is built with frame pointers): the callers of the sleeping function.
      // Only for sleeps that pass the threshold (this block used to run for every sleep), and every read is checked.
      uint64_t fp = ctx->rbp;
      fprintf(stderr, "[usleep]   callers:");
      for (int i = 0; i < 8 && fp > 0x10000 && fp < (1ull << 44); i++) {
        uint64_t pair[2];
        if (!nps5_mem_safe_read(ctx->mem_base + fp, pair, sizeof pair)) break; // a bad rbp must not fault the runtime
        fprintf(stderr, " 0x%llx", (unsigned long long)pair[1]);
        fp = pair[0];
      }
      fprintf(stderr, "\n");
    }
  }
  ctx->rax = 0;
  SHIM_RETURN();
}

// POSIX write(fd, buf, count)
// The functions below named hle_* are native functions with host pointers; the guest reaches
// them through the generated thunks of nps5_libkernel_thunks.c (pkg/thunkgen), which do all
// guest<->host pointer translation.

// POSIX write(fd, buf, count)
ssize_t hle_write(GuestContext *ctx, int fd, const void *buf, size_t count) {
  ssize_t ret = nps5_kwrite(fd, buf, count);
  if (ret < 0) {
    set_guest_errno(ctx, errno);
  }
  return ret;
}

// PS4 / FreeBSD 64-bit iovec
struct guest_iovec {
  uint64_t iov_base;
  uint64_t iov_len;
};

// POSIX writev(fd, iov, iovcnt). iov is the host view of the guest's iovec array (the thunk
// translates each iov_base); NULL when the count was out of range.
ssize_t hle_writev(GuestContext *ctx, int fd, const struct iovec *iov, int iovcnt) {
  if (iov == NPS5_BAD_GUEST_ARRAY) { // the thunk could not read the guest's iovec array
    set_guest_errno(ctx, EFAULT);
    return -1;
  }
  if (iovcnt <= 0 || iovcnt > 1024 || !iov) {
    set_guest_errno(ctx, EINVAL);
    return -1;
  }
  ssize_t ret = nps5_kwritev(fd, iov, iovcnt);
  if (ret < 0) {
    set_guest_errno(ctx, errno);
  }
  return ret;
}

// sceKernelWrite
int64_t hle_sceKernelWrite(GuestContext *ctx, int fd, const void *buf, size_t count) {
  ssize_t ret = nps5_kwrite(fd, buf, count);
  if (ret < 0) {
    int err = errno;
    set_guest_errno(ctx, err);
    return (int64_t)sce_kernel_error(err);
  }
  return ret;
}

// sceKernelWritev
int64_t hle_sceKernelWritev(GuestContext *ctx, int fd, const struct iovec *iov, int iovcnt) {
  if (iov == NPS5_BAD_GUEST_ARRAY) {
    set_guest_errno(ctx, EFAULT);
    return (int64_t)sce_kernel_error(EFAULT);
  }
  if (iovcnt <= 0 || iovcnt > 1024 || !iov) {
    set_guest_errno(ctx, EINVAL);
    return (int64_t)sce_kernel_error(EINVAL);
  }
  ssize_t ret = nps5_kwritev(fd, iov, iovcnt);
  if (ret < 0) {
    int err = errno;
    set_guest_errno(ctx, err);
    return (int64_t)sce_kernel_error(err);
  }
  return ret;
}

// Exit (does not return). Through ps4_host_quit, as the host's own quit paths:
// the pipeline archive is saved and the log drained, and no static destructors
// run while game threads and Metal completion handlers still use global state.
// Overridden by modules/libc/nps5_libc_stdlib.c (strong definition).
__attribute__((weak)) void nps5_libc_run_atexit(GuestContext *ctx) { (void)ctx; }

void hle_exit(GuestContext *ctx, int status) {
  nps5_libc_run_atexit(ctx); // atexit / __cxa_atexit handlers (modules/libc)
  ps4_host_quit(status);
}

// errno pointer (allocated within dynamic TLS space)
void shim_error(GuestContext *ctx) {
  ctx->rax = ctx->fs_base + 0x100ULL;
  SHIM_RETURN();
}

// sysconf
void shim_sysconf(GuestContext *ctx) {
  int name = (int)ctx->rdi;
  int host_name = -1;
  switch (name) {
  case 47: // FreeBSD _SC_PAGESIZE
    host_name = _SC_PAGESIZE;
    break;
  case 57: // FreeBSD _SC_NPROCESSORS_CONF
    host_name = _SC_NPROCESSORS_CONF;
    break;
  case 58: // FreeBSD _SC_NPROCESSORS_ONLN
    host_name = _SC_NPROCESSORS_ONLN;
    break;
  case 1: // FreeBSD _SC_ARG_MAX
    host_name = _SC_ARG_MAX;
    break;
  case 2: // FreeBSD _SC_CHILD_MAX
    host_name = _SC_CHILD_MAX;
    break;
  case 3: // FreeBSD _SC_CLK_TCK
    host_name = _SC_CLK_TCK;
    break;
  case 5: // FreeBSD _SC_OPEN_MAX
    host_name = _SC_OPEN_MAX;
    break;
  default:
    host_name = name;
    break;
  }
  long val = sysconf(host_name);
  if (val < 0) {
    if (name == 47)
      val = 4096;
    else if (name == 57 || name == 58)
      val = 8;
    else
      val = 4096;
  }
  ctx->rax = (uint64_t)val;
  SHIM_RETURN();
}

// nanosleep
void shim_nanosleep(GuestContext *ctx) {
  uint64_t req_addr = ctx->rdi;
  uint64_t rem_addr = ctx->rsi;

  struct timespec req, rem;
  uint64_t *guest_req = (uint64_t *)(ctx->mem_base + req_addr);
  req.tv_sec = (time_t)guest_req[0];
  req.tv_nsec = (long)guest_req[1];

  int ret;
  if (req.tv_sec < 0 || req.tv_nsec < 0 || req.tv_nsec >= 1000000000L) {
    ret = -1;
    errno = EINVAL;
    rem.tv_sec = 0;
    rem.tv_nsec = 0;
  } else {
    uint64_t left = 0;
    ret = nps5_sleep_ns((uint64_t)req.tv_sec * 1000000000ull + (uint64_t)req.tv_nsec, &left);
    rem.tv_sec = (time_t)(left / 1000000000ull);
    rem.tv_nsec = (long)(left % 1000000000ull);
  }
  if (ret < 0) {
    set_guest_errno(ctx, errno);
  }
  if (ret != 0 && rem_addr) {
    uint64_t *guest_rem = (uint64_t *)(ctx->mem_base + rem_addr);
    guest_rem[0] = (uint64_t)rem.tv_sec;
    guest_rem[1] = (uint64_t)rem.tv_nsec;
  }
  ctx->rax = (uint64_t)ret;
  SHIM_RETURN();
}

// sched_yield
void nps5_yield_adaptive(void);
void shim_sched_yield(GuestContext *ctx) {
  nps5_yield_adaptive();
  ctx->rax = 0;
  SHIM_RETURN();
}

// mmap
// One guest mmap request. addr is a guest address (0: none). Returns the guest address or -1
// with *err set.
static uint64_t guest_mmap(GuestContext *ctx, uint64_t addr, size_t len, int prot, int flags, int fd, off_t offset,
                           const char *name, int *err) {
  if (len == 0) {
    *err = EINVAL;
    return (uint64_t)-1;
  }
  size_t align = 65536;
  size_t aligned_len = (len + align - 1) & ~(align - 1);
  // A fixed request covers exactly the pages named (16 KiB granularity); only the placement of
  // unhinted mappings uses the 64 KiB allocation granule.
  if (addr != 0 && (flags & 0x10)) aligned_len = (len + NPS5_PAGE_MASK) & ~NPS5_PAGE_MASK;

  int merr = 0;
  uint64_t alloc_addr = recomp_vm_map_ex(ctx, addr, aligned_len, align, prot, flags, name, NPS5_EXT_MAPPED, &merr);
  if (alloc_addr == (uint64_t)-1) {
    PS4_LOGF(NPS5_S_SYS, NPS5_L_WARN, "ps4-sys", "%s FAILED: addr=0x%llx len=%zu flags=0x%x\n", name,
             (unsigned long long)addr, len, flags);
    *err = merr == EINVAL ? EINVAL : ENOMEM;
    return (uint64_t)-1;
  }
  // Not anonymous and a valid fd: read the file data into the mapping.
  if (!(flags & 0x1000) && fd >= 0) {
    ssize_t nread;
    PS4_HITCH_TIMED(PS4_HITCH_IO, nread = ps4_vfs_pread(fd, ctx->mem_base + alloc_addr, len, offset));
    if (nread < 0) {
      *err = errno;
      PS4_LOGF(NPS5_S_SYS, NPS5_L_WARN, "ps4-sys", "%s file read FAILED: fd=%d offset=%lld len=%zu errno=%d (%s)\n",
               name, fd, (long long)offset, len, errno, strerror(errno));
      recomp_vm_free(ctx, alloc_addr, aligned_len);
      return (uint64_t)-1;
    }
  }
  PS4_LOGF(NPS5_S_SYS, NPS5_L_DEBUG, "ps4-sys", "%s: addr=0x%llx len=%zu prot=0x%x flags=0x%x fd=%d -> 0x%llx\n", name,
           (unsigned long long)addr, len, prot, flags, fd, (unsigned long long)alloc_addr);
  return alloc_addr;
}

// mmap. addr and the result are host pointers into the guest arena (the VM manager itself
// works in guest addresses, hence the conversions at this edge).
void *hle_mmap(GuestContext *ctx, void *addr, size_t len, int prot, int flags, int fd, off_t offset) {
  int err = 0;
  uint64_t g = guest_mmap(ctx, NPS5_H2G(ctx, addr), len, prot, flags, fd, offset, "mmap", &err);
  if (g == (uint64_t)-1) {
    set_guest_errno(ctx, err);
    return MAP_FAILED;
  }
  return NPS5_G2H(ctx, g);
}

// sceKernelMmap
// sceKernelMmap(addr, len, prot, flags, fd, offset, void **res)
int hle_sceKernelMmap(GuestContext *ctx, void *addr, size_t len, int prot, int flags, int fd, off_t offset, void **res) {
  if (len == 0 || !res) {
    return (int)0x80020016; // SCE_KERNEL_ERROR_EINVAL
  }
  int err = 0;
  uint64_t g = guest_mmap(ctx, NPS5_H2G(ctx, addr), len, prot, flags, fd, offset, "sceKernelMmap", &err);
  if (g == (uint64_t)-1) {
    return err == ENOMEM ? (int)0x8002000c /* SCE_KERNEL_ERROR_ENOMEM */ : (int)sce_kernel_error(err);
  }
  *res = NPS5_G2H(ctx, g);
  return 0;
}

// munmap
int hle_munmap(GuestContext *ctx, void *addr, size_t len) {
  if (recomp_vm_free(ctx, NPS5_H2G(ctx, addr), len) != 0) {
    set_guest_errno(ctx, EINVAL);
    return -1;
  }
  return 0;
}

// madvise
void shim_madvise(GuestContext *ctx) {
  uint64_t addr = ctx->rdi;
  size_t len = (size_t)ctx->rsi;
  int behav = (int)ctx->rdx;

  if (addr + len <= ctx->mem_size) {
#if defined(MADV_DONTNEED)
    int host_behav = MADV_DONTNEED;
#if defined(MADV_FREE)
    if (behav == 5)
      host_behav = MADV_FREE;
#endif
    madvise(ctx->mem_base + addr, len, host_behav);
#endif
  }
  ctx->rax = 0;
  SHIM_RETURN();
}

// read
void shim_read(GuestContext *ctx) {
  int fd = (int)ctx->rdi;
  uint64_t buf_addr = ctx->rsi;
  size_t count = (size_t)ctx->rdx;
  ssize_t ret;
  PS4_HITCH_TIMED(PS4_HITCH_IO,
                  ret = ps4_vfs_read(fd, ctx->mem_base + buf_addr, count));
  if (ret < 0) {
    set_guest_errno(ctx, errno);
  }
  ctx->rax = (uint64_t)ret;
  SHIM_RETURN();
}

// readv
void shim_readv(GuestContext *ctx) {
  int fd = (int)ctx->rdi;
  uint64_t iov_addr = ctx->rsi;
  int iovcnt = (int)ctx->rdx;

  if (iovcnt <= 0 || iovcnt > 1024) {
    set_guest_errno(ctx, EINVAL);
    ctx->rax = (uint64_t)-1;
    SHIM_RETURN();
  }

  struct guest_iovec *guest_iov =
      (struct guest_iovec *)(ctx->mem_base + iov_addr);
  struct iovec host_iov[iovcnt];

  for (int i = 0; i < iovcnt; i++) {
    host_iov[i].iov_base = ctx->mem_base + guest_iov[i].iov_base;
    host_iov[i].iov_len = (size_t)guest_iov[i].iov_len;
  }

  ssize_t ret = nps5_kreadv(fd, host_iov, iovcnt);
  if (ret > 0) ps4_vfs_readlog_note(fd, -1, ret);
  if (ret < 0) {
    set_guest_errno(ctx, errno);
  }
  ctx->rax = (uint64_t)ret;
  SHIM_RETURN();
}

// sceKernelRead
void shim_sceKernelRead(GuestContext *ctx) {
  int fd = (int)ctx->rdi;
  uint64_t buf_addr = ctx->rsi;
  size_t count = (size_t)ctx->rdx;
  ssize_t ret;
  PS4_HITCH_TIMED(PS4_HITCH_IO,
                  ret = ps4_vfs_read(fd, ctx->mem_base + buf_addr, count));
  if (ret < 0) {
    int err = errno;
    set_guest_errno(ctx, err);
    ctx->rax = sce_kernel_error(err);
  } else {
    ctx->rax = (uint64_t)ret;
  }
  SHIM_RETURN();
}

// sceKernelReadv
void shim_sceKernelReadv(GuestContext *ctx) {
  int fd = (int)ctx->rdi;
  uint64_t iov_addr = ctx->rsi;
  int iovcnt = (int)ctx->rdx;

  if (iovcnt <= 0 || iovcnt > 1024) {
    set_guest_errno(ctx, EINVAL);
    ctx->rax = sce_kernel_error(EINVAL);
    SHIM_RETURN();
  }

  struct guest_iovec *guest_iov =
      (struct guest_iovec *)(ctx->mem_base + iov_addr);
  struct iovec host_iov[iovcnt];

  for (int i = 0; i < iovcnt; i++) {
    host_iov[i].iov_base = ctx->mem_base + guest_iov[i].iov_base;
    host_iov[i].iov_len = (size_t)guest_iov[i].iov_len;
  }

  ssize_t ret = nps5_kreadv(fd, host_iov, iovcnt);
  if (ret > 0) ps4_vfs_readlog_note(fd, -1, ret);
  if (ret < 0) {
    int err = errno;
    set_guest_errno(ctx, err);
    ctx->rax = sce_kernel_error(err);
  } else {
    ctx->rax = (uint64_t)ret;
  }
  SHIM_RETURN();
}

// sceKernelPread(fd, buf, nbytes, offset)
void shim_sceKernelPread(GuestContext *ctx) {
  int fd = (int)ctx->rdi;
  uint64_t buf_addr = ctx->rsi;
  size_t nbytes = (size_t)ctx->rdx;
  off_t offset = (off_t)ctx->rcx;

  ssize_t ret;
  PS4_HITCH_TIMED(
      PS4_HITCH_IO,
      ret = ps4_vfs_pread(fd, ctx->mem_base + buf_addr, nbytes, offset));
  if (ret < 0) {
    int err = errno;
    set_guest_errno(ctx, err);
    ctx->rax = sce_kernel_error(err);
  } else {
    ctx->rax = (uint64_t)ret;
  }
  SHIM_RETURN();
}

// sceKernelPreadv(fd, iov, iovcnt, offset)
void shim_sceKernelPreadv(GuestContext *ctx) {
  int fd = (int)ctx->rdi;
  uint64_t iov_addr = ctx->rsi;
  int iovcnt = (int)ctx->rdx;
  off_t offset = (off_t)ctx->rcx;

  if (iovcnt <= 0 || iovcnt > 1024) {
    set_guest_errno(ctx, EINVAL);
    ctx->rax = sce_kernel_error(EINVAL);
    SHIM_RETURN();
  }

  struct guest_iovec *guest_iov =
      (struct guest_iovec *)(ctx->mem_base + iov_addr);
  struct iovec host_iov[iovcnt];

  for (int i = 0; i < iovcnt; i++) {
    host_iov[i].iov_base = ctx->mem_base + guest_iov[i].iov_base;
    host_iov[i].iov_len = (size_t)guest_iov[i].iov_len;
  }

  ssize_t ret;
  PS4_HITCH_TIMED(PS4_HITCH_IO, ret = nps5_kpreadv(fd, host_iov, iovcnt, offset));
  if (ret > 0) ps4_vfs_readlog_note(fd, (long long)offset, ret);
  if (ret < 0) {
    int err = errno;
    set_guest_errno(ctx, err);
    ctx->rax = sce_kernel_error(err);
  } else {
    ctx->rax = (uint64_t)ret;
  }
  SHIM_RETURN();
}

// Translate FreeBSD / Orbis OS open() flags to host (Darwin / Linux) open()
// flags
static int ps4_open_flags_to_host(int ps4_flags, bool *out_direct) {
  int host_flags = 0;
  int acc = ps4_flags & 0x3;
  if (acc == 1) {
    host_flags |= O_WRONLY;
  } else if (acc == 2) {
    host_flags |= O_RDWR;
  } else {
    host_flags |= O_RDONLY;
  }

  if (ps4_flags & 0x0004)
    host_flags |= O_NONBLOCK;
  if (ps4_flags & 0x0008)
    host_flags |= O_APPEND;
  if (ps4_flags & 0x0080)
    host_flags |= O_SYNC;
  if (ps4_flags & 0x0100)
    host_flags |= O_NOFOLLOW;
  if (ps4_flags & 0x0200)
    host_flags |= O_CREAT;
  if (ps4_flags & 0x0400)
    host_flags |= O_TRUNC;
  if (ps4_flags & 0x0800)
    host_flags |= O_EXCL;
#ifdef O_DSYNC
  if (ps4_flags & 0x1000)
    host_flags |= O_DSYNC;
#else
  if (ps4_flags & 0x1000)
    host_flags |= O_SYNC;
#endif

  // FreeBSD O_DIRECTORY = 0x00020000 -> host O_DIRECTORY (0x00100000 on macOS)
  if (ps4_flags & 0x00020000)
    host_flags |= O_DIRECTORY;

  // FreeBSD O_CLOEXEC = 0x00100000 -> host O_CLOEXEC (0x01000000 on macOS)
  if (ps4_flags & 0x00100000)
    host_flags |= O_CLOEXEC;

  if (out_direct) {
    *out_direct = (ps4_flags & 0x00010000) != 0;
  }

  return host_flags;
}

static int host_open_flags_to_ps4(int host_flags) {
  int ps4_flags = 0;
  int acc = host_flags & 0x3;
  if (acc == O_WRONLY)
    ps4_flags |= 0x1;
  else if (acc == O_RDWR)
    ps4_flags |= 0x2;
  else
    ps4_flags |= 0x0;

  if (host_flags & O_NONBLOCK)
    ps4_flags |= 0x0004;
  if (host_flags & O_APPEND)
    ps4_flags |= 0x0008;
  if (host_flags & O_SYNC)
    ps4_flags |= 0x0080;
  if (host_flags & O_NOFOLLOW)
    ps4_flags |= 0x0100;
  if (host_flags & O_CREAT)
    ps4_flags |= 0x0200;
  if (host_flags & O_TRUNC)
    ps4_flags |= 0x0400;
  if (host_flags & O_EXCL)
    ps4_flags |= 0x0800;
  if (host_flags & O_DIRECTORY)
    ps4_flags |= 0x00020000;
  if (host_flags & O_CLOEXEC)
    ps4_flags |= 0x00100000;

  return ps4_flags;
}

// fcntl
void shim_fcntl(GuestContext *ctx) {
  int fd = (int)ctx->rdi;
  int cmd = (int)ctx->rsi;
  uint64_t arg = ctx->rdx;
  int ret;
  if (cmd == F_SETFL) {
    bool is_direct = false;
    int host_flags = ps4_open_flags_to_host((int)arg, &is_direct);
    ret = fcntl(fd, cmd, host_flags);
#if defined(__APPLE__) && defined(F_NOCACHE)
    if (ret >= 0 && is_direct) {
      fcntl(fd, F_NOCACHE, 1);
    }
#endif
  } else {
    ret = fcntl(fd, cmd, arg);
    if (ret >= 0 && cmd == F_GETFL) {
      ret = host_open_flags_to_ps4(ret);
    }
  }
  if (ret < 0) {
    set_guest_errno(ctx, errno);
  }
  ctx->rax = (uint64_t)ret;
  SHIM_RETURN();
}

// open
void shim_open(GuestContext *ctx) {
  uint64_t path_addr = ctx->rdi;
  int flags = (int)ctx->rsi;
  mode_t mode = (mode_t)ctx->rdx;
  const char *path = (const char *)(ctx->mem_base + path_addr);
  bool is_direct = false;
  int host_flags = ps4_open_flags_to_host(flags, &is_direct);
  int ret = ps4_vfs_open(path, host_flags, mode);
  if (ret < 0) {
    set_guest_errno(ctx, errno);
  } else if (!ps4_vfs_is_virtual_fd(ret)) {
#if defined(__APPLE__) && defined(F_NOCACHE)
    if (is_direct) {
      fcntl(ret, F_NOCACHE, 1);
    }
#endif
  }
  PS4_LOGF(NPS5_S_SYS, NPS5_L_DEBUG, "ps4-sys", "open: '%s' (flags=0x%x, host_flags=0x%x) -> fd=%d\n",
         path ? path : "(null)", flags, host_flags, ret);
  fflush(stdout);
  ctx->rax = (uint64_t)ret;
  SHIM_RETURN();
}

// close
void shim_close(GuestContext *ctx) {
  int fd = (int)ctx->rdi;
  guest_dir_cache_drop_fd(fd);
  int ret = ps4_vfs_close(fd);
  if (ret < 0) {
    set_guest_errno(ctx, errno);
  }
  ctx->rax = (uint64_t)ret;
  SHIM_RETURN();
}

// lseek
void shim_lseek(GuestContext *ctx) {
  int fd = (int)ctx->rdi;
  off_t offset = (off_t)ctx->rsi;
  int whence = (int)ctx->rdx;
  off_t ret = ps4_vfs_lseek(fd, offset, whence);
  if (ret < 0) {
    set_guest_errno(ctx, errno);
  }
  ctx->rax = (uint64_t)ret;
  SHIM_RETURN();
}

// sceKernelOpen
void shim_sceKernelOpen(GuestContext *ctx) {
  uint64_t path_addr = ctx->rdi;
  int flags = (int)ctx->rsi;
  mode_t mode = (mode_t)ctx->rdx;
  const char *path = (const char *)(ctx->mem_base + path_addr);
  bool is_direct = false;
  int host_flags = ps4_open_flags_to_host(flags, &is_direct);
  int ret = ps4_vfs_open(path, host_flags, mode);
  if (ret < 0) {
    int err = errno;
    set_guest_errno(ctx, err);
    ctx->rax = sce_kernel_error(err);
  } else {
    if (!ps4_vfs_is_virtual_fd(ret)) {
#if defined(__APPLE__) && defined(F_NOCACHE)
      if (is_direct) {
        fcntl(ret, F_NOCACHE, 1);
      }
#endif
    }
    ctx->rax = (uint64_t)ret;
  }
  PS4_LOGF(NPS5_S_SYS, NPS5_L_DEBUG, "ps4-sys", "sceKernelOpen: '%s' (flags=0x%x, host_flags=0x%x) -> fd=%d "
         "(ret=0x%llx)\n",
         path ? path : "(null)", flags, host_flags, ret,
         (unsigned long long)ctx->rax);
  fflush(stdout);
  SHIM_RETURN();
}

// sceKernelCheckReachability
void shim_sceKernelCheckReachability(GuestContext *ctx) {
  uint64_t path_addr = ctx->rdi;
  if (!path_addr || !RECOMP_MEM_OK(ctx)) {
    ctx->rax = 0x8002000e; // SCE_KERNEL_ERROR_EFAULT
    SHIM_RETURN();
  }
  const char *path = (const char *)(ctx->mem_base + path_addr);
  if (strlen(path) > 255) {
    ctx->rax = 0x8002003f; // SCE_KERNEL_ERROR_ENAMETOOLONG
    SHIM_RETURN();
  }
  if (strcmp(path, "/") == 0) {
    ctx->rax = 0;
    SHIM_RETURN();
  }
  if (ps4_vfs_access(path, F_OK) == 0) {
    ctx->rax = 0;
  } else {
    ctx->rax = 0x80020002; // SCE_KERNEL_ERROR_ENOENT
  }
  SHIM_RETURN();
}

// sceKernelClose
void shim_sceKernelClose(GuestContext *ctx) {
  int fd = (int)ctx->rdi;
  guest_dir_cache_drop_fd(fd);
  int ret = ps4_vfs_close(fd);
  if (ret < 0) {
    int err = errno;
    set_guest_errno(ctx, err);
    ctx->rax = sce_kernel_error(err);
  } else {
    ctx->rax = 0;
  }
  SHIM_RETURN();
}

// sceKernelLseek
void shim_sceKernelLseek(GuestContext *ctx) {
  int fd = (int)ctx->rdi;
  off_t offset = (off_t)ctx->rsi;
  int whence = (int)ctx->rdx;
  off_t ret = ps4_vfs_lseek(fd, offset, whence);
  if (ret < 0) {
    int err = errno;
    set_guest_errno(ctx, err);
    ctx->rax = sce_kernel_error(err);
  } else {
    ctx->rax = (uint64_t)ret;
  }
  SHIM_RETURN();
}

// Exact 120-byte layout for PS4 Orbis OS / FreeBSD 9 64-bit struct stat
struct ps4_stat {
  uint32_t st_dev;          // 0x00
  uint32_t st_ino;          // 0x04
  uint16_t st_mode;         // 0x08
  uint16_t st_nlink;        // 0x0A
  uint32_t st_uid;          // 0x0C
  uint32_t st_gid;          // 0x10
  uint32_t st_rdev;         // 0x14
  int64_t st_atime_sec;     // 0x18
  int64_t st_atime_nsec;    // 0x20
  int64_t st_mtime_sec;     // 0x28
  int64_t st_mtime_nsec;    // 0x30
  int64_t st_ctime_sec;     // 0x38
  int64_t st_ctime_nsec;    // 0x40
  int64_t st_size;          // 0x48
  int64_t st_blocks;        // 0x50
  uint32_t st_blksize;      // 0x58
  uint32_t st_flags;        // 0x5C
  uint32_t st_gen;          // 0x60
  int32_t st_lspare;        // 0x64
  int64_t st_birthtim_sec;  // 0x68
  int64_t st_birthtim_nsec; // 0x70
};
_Static_assert(sizeof(struct ps4_stat) == 120,
               "ps4_stat must be exactly 120 bytes");

static void fill_ps4_stat(struct ps4_stat *gst, const struct stat *st) {
  memset(gst, 0, sizeof(struct ps4_stat));
  gst->st_dev = (uint32_t)st->st_dev;
  gst->st_ino = (uint32_t)st->st_ino;
  gst->st_mode = (uint16_t)st->st_mode;
  gst->st_nlink = (uint16_t)st->st_nlink;
  gst->st_uid = (uint32_t)st->st_uid;
  gst->st_gid = (uint32_t)st->st_gid;
  gst->st_rdev = (uint32_t)st->st_rdev;
#if defined(__APPLE__)
  gst->st_atime_sec = (int64_t)st->st_atimespec.tv_sec;
  gst->st_atime_nsec = (int64_t)st->st_atimespec.tv_nsec;
  gst->st_mtime_sec = (int64_t)st->st_mtimespec.tv_sec;
  gst->st_mtime_nsec = (int64_t)st->st_mtimespec.tv_nsec;
  gst->st_ctime_sec = (int64_t)st->st_ctimespec.tv_sec;
  gst->st_ctime_nsec = (int64_t)st->st_ctimespec.tv_nsec;
  gst->st_birthtim_sec = (int64_t)st->st_birthtimespec.tv_sec;
  gst->st_birthtim_nsec = (int64_t)st->st_birthtimespec.tv_nsec;
#else
  gst->st_atime_sec = (int64_t)st->st_atim.tv_sec;
  gst->st_atime_nsec = (int64_t)st->st_atim.tv_nsec;
  gst->st_mtime_sec = (int64_t)st->st_mtim.tv_sec;
  gst->st_mtime_nsec = (int64_t)st->st_mtim.tv_nsec;
  gst->st_ctime_sec = (int64_t)st->st_ctim.tv_sec;
  gst->st_ctime_nsec = (int64_t)st->st_ctim.tv_nsec;
#endif
  gst->st_size = (int64_t)st->st_size;
  gst->st_blocks = (int64_t)st->st_blocks;
  gst->st_blksize = (uint32_t)st->st_blksize;
  gst->st_flags = (uint32_t)st->st_flags;
  gst->st_gen = (uint32_t)st->st_gen;
  gst->st_lspare = 0;
}

// stat
void shim_stat(GuestContext *ctx) {
  uint64_t path_addr = ctx->rdi;
  uint64_t statbuf_addr = ctx->rsi;
  const char *path = (const char *)(ctx->mem_base + path_addr);
  struct stat st;
  int ret = ps4_vfs_stat(path, &st);
  if (ret == 0) {
    struct ps4_stat *gst = (struct ps4_stat *)(ctx->mem_base + statbuf_addr);
    fill_ps4_stat(gst, &st);
    ctx->rax = 0;
  } else {
    set_guest_errno(ctx, errno);
    ctx->rax = (uint64_t)-1;
  }
  SHIM_RETURN();
}

// fstat
void shim_fstat(GuestContext *ctx) {
  int fd = (int)ctx->rdi;
  uint64_t statbuf_addr = ctx->rsi;
  struct stat st;
  int ret = ps4_vfs_fstat(fd, &st);
  if (ret == 0) {
    struct ps4_stat *gst = (struct ps4_stat *)(ctx->mem_base + statbuf_addr);
    fill_ps4_stat(gst, &st);
    ctx->rax = 0;
  } else {
    set_guest_errno(ctx, errno);
    ctx->rax = (uint64_t)-1;
  }
  SHIM_RETURN();
}

void nps5_fill_guest_stat(void *gst, const struct stat *st) { fill_ps4_stat((struct ps4_stat *)gst, st); }

// sceKernelStat
void shim_sceKernelStat(GuestContext *ctx) {
  uint64_t path_addr = ctx->rdi;
  uint64_t statbuf_addr = ctx->rsi;
  const char *path = (const char *)(ctx->mem_base + path_addr);
  struct stat st;
  int ret = ps4_vfs_stat(path, &st);
  { const char *e = getenv("NPS5_VFS_TRACE_INI"); size_t n = strlen(path); // see docs/m3/uecvar.md
    if (e && *e == '1' && n > 4 && !strcasecmp(path + n - 4, ".ini") && ret == 0) fprintf(stderr, "[ps4-vfs] ini stat ok: %s size=%lld\n", path, (long long)st.st_size); }
  if (ret == 0) {
    struct ps4_stat *gst = (struct ps4_stat *)(ctx->mem_base + statbuf_addr);
    fill_ps4_stat(gst, &st);
    ctx->rax = 0;
  } else {
    int err = errno;
    set_guest_errno(ctx, err);
    ctx->rax = sce_kernel_error(err);
  }
  SHIM_RETURN();
}

// sceKernelFstat
void shim_sceKernelFstat(GuestContext *ctx) {
  int fd = (int)ctx->rdi;
  uint64_t statbuf_addr = ctx->rsi;
  struct stat st;
  int ret = ps4_vfs_fstat(fd, &st);
  if (ret == 0) {
    struct ps4_stat *gst = (struct ps4_stat *)(ctx->mem_base + statbuf_addr);
    fill_ps4_stat(gst, &st);
    ctx->rax = 0;
  } else {
    int err = errno;
    set_guest_errno(ctx, err);
    ctx->rax = sce_kernel_error(err);
  }
  SHIM_RETURN();
}

// chmod
void shim_chmod(GuestContext *ctx) {
  uint64_t path_addr = ctx->rdi;
  mode_t mode = (mode_t)ctx->rsi;
  const char *path = (const char *)(ctx->mem_base + path_addr);
  int ret = ps4_vfs_chmod(path, mode);
  if (ret < 0) {
    set_guest_errno(ctx, errno);
    ctx->rax = (uint64_t)-1;
  } else {
    ctx->rax = 0;
  }
  SHIM_RETURN();
}

// fchmod
void shim_fchmod(GuestContext *ctx) {
  int fd = (int)ctx->rdi;
  mode_t mode = (mode_t)ctx->rsi;
  int ret = ps4_vfs_fchmod(fd, mode);
  if (ret < 0) {
    set_guest_errno(ctx, errno);
    ctx->rax = (uint64_t)-1;
  } else {
    ctx->rax = 0;
  }
  SHIM_RETURN();
}

// utimes
void shim_utimes(GuestContext *ctx) {
  uint64_t path_addr = ctx->rdi;
  uint64_t times_addr = ctx->rsi;
  const char *path = (const char *)(ctx->mem_base + path_addr);
  const struct timeval *host_times = NULL;
  struct timeval tv[2];
  if (times_addr && RECOMP_MEM_OK(ctx)) {
    const int64_t *guest_tv = (const int64_t *)(ctx->mem_base + times_addr);
    tv[0].tv_sec = (time_t)guest_tv[0];
    tv[0].tv_usec = (suseconds_t)guest_tv[1];
    tv[1].tv_sec = (time_t)guest_tv[2];
    tv[1].tv_usec = (suseconds_t)guest_tv[3];
    host_times = tv;
  }
  int ret = ps4_vfs_utimes(path, host_times);
  if (ret < 0) {
    set_guest_errno(ctx, errno);
    ctx->rax = (uint64_t)-1;
  } else {
    ctx->rax = 0;
  }
  SHIM_RETURN();
}

// futimes
void shim_futimes(GuestContext *ctx) {
  int fd = (int)ctx->rdi;
  uint64_t times_addr = ctx->rsi;
  const struct timeval *host_times = NULL;
  struct timeval tv[2];
  if (times_addr && RECOMP_MEM_OK(ctx)) {
    const int64_t *guest_tv = (const int64_t *)(ctx->mem_base + times_addr);
    tv[0].tv_sec = (time_t)guest_tv[0];
    tv[0].tv_usec = (suseconds_t)guest_tv[1];
    tv[1].tv_sec = (time_t)guest_tv[2];
    tv[1].tv_usec = (suseconds_t)guest_tv[3];
    host_times = tv;
  }
  int ret = ps4_vfs_futimes(fd, host_times);
  if (ret < 0) {
    set_guest_errno(ctx, errno);
    ctx->rax = (uint64_t)-1;
  } else {
    ctx->rax = 0;
  }
  SHIM_RETURN();
}

// fsync
void shim_fsync(GuestContext *ctx) {
  int fd = (int)ctx->rdi;
  int ret = fsync(fd);
  if (ret < 0) {
    set_guest_errno(ctx, errno);
    ctx->rax = (uint64_t)-1;
  } else {
    ctx->rax = 0;
  }
  SHIM_RETURN();
}

void shim_sceKernelFsync(GuestContext *ctx) {
  int fd = (int)ctx->rdi;
  int ret = fsync(fd);
  if (ret < 0) {
    ctx->rax = sce_kernel_error(errno);
  } else {
    ctx->rax = 0;
  }
  SHIM_RETURN();
}

// ftruncate
void shim_ftruncate(GuestContext *ctx) {
  int fd = (int)ctx->rdi;
  off_t length = (off_t)ctx->rsi;
  int ret = ps4_vfs_ftruncate(fd, length);
  if (ret < 0) {
    set_guest_errno(ctx, errno);
    ctx->rax = (uint64_t)-1;
  } else {
    ctx->rax = 0;
  }
  SHIM_RETURN();
}

void shim_sceKernelFtruncate(GuestContext *ctx) {
  int fd = (int)ctx->rdi;
  off_t length = (off_t)ctx->rsi;
  int ret = ps4_vfs_ftruncate(fd, length);
  if (ret < 0) {
    ctx->rax = sce_kernel_error(errno);
  } else {
    ctx->rax = 0;
  }
  SHIM_RETURN();
}

// msync
void shim_msync(GuestContext *ctx) {
  uint64_t addr = ctx->rdi;
  size_t len = (size_t)ctx->rsi;
  int flags = (int)ctx->rdx;
  void *host_addr =
      (addr && RECOMP_MEM_OK(ctx)) ? (void *)(ctx->mem_base + addr) : NULL;
  int ret = msync(host_addr, len, flags);
  if (ret < 0) {
    set_guest_errno(ctx, errno);
    ctx->rax = (uint64_t)-1;
  } else {
    ctx->rax = 0;
  }
  SHIM_RETURN();
}

// pwrite
void shim_pwrite(GuestContext *ctx) {
  int fd = (int)ctx->rdi;
  uint64_t buf_addr = ctx->rsi;
  size_t nbyte = (size_t)ctx->rdx;
  off_t offset = (off_t)ctx->rcx;
  const void *buf = (buf_addr && RECOMP_MEM_OK(ctx))
                        ? (const void *)(ctx->mem_base + buf_addr)
                        : NULL;
  ssize_t ret = nps5_kpwrite(fd, buf, nbyte, offset);
  if (ret < 0) {
    set_guest_errno(ctx, errno);
    ctx->rax = (uint64_t)-1;
  } else {
    ctx->rax = (uint64_t)ret;
  }
  SHIM_RETURN();
}

void shim_sceKernelPwrite(GuestContext *ctx) {
  int fd = (int)ctx->rdi;
  uint64_t buf_addr = ctx->rsi;
  size_t nbyte = (size_t)ctx->rdx;
  off_t offset = (off_t)ctx->rcx;
  const void *buf = (buf_addr && RECOMP_MEM_OK(ctx))
                        ? (const void *)(ctx->mem_base + buf_addr)
                        : NULL;
  ssize_t ret = nps5_kpwrite(fd, buf, nbyte, offset);
  if (ret < 0) {
    ctx->rax = sce_kernel_error(errno);
  } else {
    ctx->rax = (uint64_t)ret;
  }
  SHIM_RETURN();
}

// unlink
void shim_unlink(GuestContext *ctx) {
  uint64_t path_addr = ctx->rdi;
  const char *path = (const char *)(ctx->mem_base + path_addr);
  int ret = ps4_vfs_unlink(path);
  if (ret < 0) {
    set_guest_errno(ctx, errno);
    ctx->rax = (uint64_t)-1;
  } else {
    ctx->rax = 0;
  }
  SHIM_RETURN();
}

// mkdir
void shim_mkdir(GuestContext *ctx) {
  uint64_t path_addr = ctx->rdi;
  mode_t mode = (mode_t)ctx->rsi;
  const char *path = (const char *)(ctx->mem_base + path_addr);
  int ret = ps4_vfs_mkdir(path, mode);
  if (ret < 0) {
    set_guest_errno(ctx, errno);
    ctx->rax = (uint64_t)-1;
  } else {
    ctx->rax = 0;
  }
  SHIM_RETURN();
}

// rmdir
void shim_rmdir(GuestContext *ctx) {
  uint64_t path_addr = ctx->rdi;
  const char *path = (const char *)(ctx->mem_base + path_addr);
  int ret = ps4_vfs_rmdir(path);
  if (ret < 0) {
    set_guest_errno(ctx, errno);
    ctx->rax = (uint64_t)-1;
  } else {
    ctx->rax = 0;
  }
  SHIM_RETURN();
}

void shim_sceKernelRmdir(GuestContext *ctx) {
  uint64_t path_addr = ctx->rdi;
  const char *path = (const char *)(ctx->mem_base + path_addr);
  int ret = ps4_vfs_rmdir(path);
  if (ret < 0) {
    ctx->rax = sce_kernel_error(errno);
  } else {
    ctx->rax = 0;
  }
  SHIM_RETURN();
}

// rename
void shim_rename(GuestContext *ctx) {
  uint64_t old_addr = ctx->rdi;
  uint64_t new_addr = ctx->rsi;
  const char *oldpath = (const char *)(ctx->mem_base + old_addr);
  const char *newpath = (const char *)(ctx->mem_base + new_addr);
  int ret = ps4_vfs_rename(oldpath, newpath);
  if (ret < 0) {
    set_guest_errno(ctx, errno);
    ctx->rax = (uint64_t)-1;
  } else {
    ctx->rax = 0;
  }
  SHIM_RETURN();
}

// truncate
void shim_truncate(GuestContext *ctx) {
  uint64_t path_addr = ctx->rdi;
  off_t length = (off_t)ctx->rsi;
  const char *path = (const char *)(ctx->mem_base + path_addr);
  int ret = ps4_vfs_truncate(path, length);
  if (ret < 0) {
    set_guest_errno(ctx, errno);
    ctx->rax = (uint64_t)-1;
  } else {
    ctx->rax = 0;
  }
  SHIM_RETURN();
}

// mlock
void shim_mlock(GuestContext *ctx) {
  ctx->rax = 0;
  SHIM_RETURN();
}

// getdents

// ---------------------------------------------------------------------------
// Directory enumeration in the guest's FreeBSD dirent format.
// Host getdirentries64 returns Darwin dirent64 records, which the guest cannot
// parse. The directory listing is snapshotted per fd (keyed by dev/ino) and
// served as FreeBSD records:
//   struct dirent { uint32_t d_fileno; uint16_t d_reclen; uint8_t d_type;
//                   uint8_t d_namlen; char d_name[d_namlen + 1]; } (4-aligned)
// AppleDouble "._*" files (created by macOS on exFAT/FAT volumes) are hidden.
typedef struct GuestDirCache {
  int fd;
  dev_t dev;
  ino_t ino;
  int count;
  int pos;
  char **names;
  uint32_t *inos;
  uint8_t *types;
} GuestDirCache;

#define GUEST_DIR_CACHE_SLOTS 64
static GuestDirCache g_dir_cache[GUEST_DIR_CACHE_SLOTS];
static pthread_mutex_t g_dir_cache_lock = PTHREAD_MUTEX_INITIALIZER;

static void guest_dir_cache_free(GuestDirCache *c) {
  for (int i = 0; i < c->count; i++)
    free(c->names[i]);
  free(c->names);
  free(c->inos);
  free(c->types);
  memset(c, 0, sizeof(*c));
  c->fd = -1;
}

static void guest_dir_cache_drop_fd(int fd) {
  pthread_mutex_lock(&g_dir_cache_lock);
  for (int i = 0; i < GUEST_DIR_CACHE_SLOTS; i++) {
    if (g_dir_cache[i].names && g_dir_cache[i].fd == fd)
      guest_dir_cache_free(&g_dir_cache[i]);
  }
  pthread_mutex_unlock(&g_dir_cache_lock);
}

static GuestDirCache *guest_dir_cache_get(int fd) {
  struct stat st;
  if (fstat(fd, &st) != 0)
    return NULL;
  if (!S_ISDIR(st.st_mode)) {
    errno = ENOTDIR;
    return NULL;
  }
  GuestDirCache *slot = NULL;
  for (int i = 0; i < GUEST_DIR_CACHE_SLOTS; i++) {
    GuestDirCache *c = &g_dir_cache[i];
    if (c->names && c->fd == fd) {
      if (c->dev == st.st_dev && c->ino == st.st_ino)
        return c;
      guest_dir_cache_free(c);
      slot = c;
      break;
    }
  }
  if (!slot) {
    for (int i = 0; i < GUEST_DIR_CACHE_SLOTS && !slot; i++) {
      if (!g_dir_cache[i].names)
        slot = &g_dir_cache[i];
    }
    if (!slot) {
      slot = &g_dir_cache[0];
      guest_dir_cache_free(slot);
    }
  }
  int dfd = dup(fd);
  if (dfd < 0)
    return NULL;
  DIR *d = fdopendir(dfd);
  if (!d) {
    close(dfd);
    return NULL;
  }
  rewinddir(d);
  int cap = 64;
  slot->names = (char **)calloc(cap, sizeof(char *));
  slot->inos = (uint32_t *)calloc(cap, sizeof(uint32_t));
  slot->types = (uint8_t *)calloc(cap, sizeof(uint8_t));
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (strncmp(e->d_name, "._", 2) == 0)
      continue;
    if (strlen(e->d_name) > 255)
      continue;
    if (slot->count == cap) {
      cap *= 2;
      slot->names = (char **)realloc(slot->names, cap * sizeof(char *));
      slot->inos = (uint32_t *)realloc(slot->inos, cap * sizeof(uint32_t));
      slot->types = (uint8_t *)realloc(slot->types, cap * sizeof(uint8_t));
    }
    slot->names[slot->count] = strdup(e->d_name);
    slot->inos[slot->count] =
        (uint32_t)e->d_ino ? (uint32_t)e->d_ino : (uint32_t)(slot->count + 1);
    slot->types[slot->count] = e->d_type; // DT_* values match FreeBSD
    slot->count++;
  }
  closedir(d); // closes dfd
  slot->fd = fd;
  slot->dev = st.st_dev;
  slot->ino = st.st_ino;
  slot->pos = 0;
  return slot;
}

// Fills buf with FreeBSD dirents. *basep (if given) is the entry index; a
// caller-supplied base restarts enumeration from that index. Returns bytes
// written, 0 at end, or -1 with errno set.
static int guest_getdirentries(int fd, char *buf, size_t nbytes,
                               int64_t *basep) {
  pthread_mutex_lock(&g_dir_cache_lock);
  GuestDirCache *c = guest_dir_cache_get(fd);
  if (!c) {
    pthread_mutex_unlock(&g_dir_cache_lock);
    return -1;
  }
  if (basep && *basep >= 0 && *basep <= c->count)
    c->pos = (int)*basep;
  if (basep && *basep < 0)
    c->pos = 0;
  size_t off = 0;
  while (c->pos < c->count) {
    size_t namlen = strlen(c->names[c->pos]);
    size_t reclen = (8 + namlen + 1 + 3) & ~(size_t)3;
    if (off + reclen > nbytes)
      break;
    char *r = buf + off;
    memset(r, 0, reclen);
    *(uint32_t *)(r + 0) = c->inos[c->pos];
    *(uint16_t *)(r + 4) = (uint16_t)reclen;
    *(uint8_t *)(r + 6) = c->types[c->pos];
    *(uint8_t *)(r + 7) = (uint8_t)namlen;
    memcpy(r + 8, c->names[c->pos], namlen);
    off += reclen;
    c->pos++;
  }
  if (off == 0 && c->pos < c->count) {
    pthread_mutex_unlock(&g_dir_cache_lock);
    errno = EINVAL;
    return -1;
  }
  if (basep)
    *basep = c->pos;
  pthread_mutex_unlock(&g_dir_cache_lock);
  return (int)off;
}

void shim_getdents(GuestContext *ctx) {
  int fd = (int)ctx->rdi;
  char *buf = (char *)(ctx->mem_base + ctx->rsi);
  size_t nbytes = (size_t)ctx->rdx;
  int ret = guest_getdirentries(fd, buf, nbytes, NULL);
  if (ret < 0) {
    set_guest_errno(ctx, errno);
    ctx->rax = (uint64_t)-1;
  } else {
    ctx->rax = (uint64_t)ret;
  }
  SHIM_RETURN();
}

// getdirentries
void shim_getdirentries(GuestContext *ctx) {
  int fd = (int)ctx->rdi;
  uint64_t buf_addr = ctx->rsi;
  size_t nbytes = (size_t)ctx->rdx;
  uint64_t basep_addr = ctx->rcx;
  char *buf = (char *)(ctx->mem_base + buf_addr);
  int64_t base = -2;
  int ret = guest_getdirentries(fd, buf, nbytes, basep_addr ? &base : NULL);
  if (ret >= 0 && basep_addr) {
    *(int64_t *)(ctx->mem_base + basep_addr) = base;
  }
  if (ret < 0) {
    set_guest_errno(ctx, errno);
    ctx->rax = (uint64_t)-1;
  } else {
    ctx->rax = (uint64_t)ret;
  }
  SHIM_RETURN();
}

void shim_sceKernelGetdirentries(GuestContext *ctx) {
  int fd = (int)ctx->rdi;
  uint64_t buf_addr = ctx->rsi;
  size_t nbytes = (size_t)ctx->rdx;
  uint64_t basep_addr = ctx->rcx;
  char *buf = (char *)(ctx->mem_base + buf_addr);
  int64_t base = -2;
  int ret = guest_getdirentries(fd, buf, nbytes, basep_addr ? &base : NULL);
  if (ret >= 0 && basep_addr) {
    *(int64_t *)(ctx->mem_base + basep_addr) = base;
  }
  if (ret < 0) {
    ctx->rax = sce_kernel_error(errno);
  } else {
    ctx->rax = (uint64_t)ret;
  }
  SHIM_RETURN();
}

void shim_sceKernelSleep(GuestContext *ctx) {
  unsigned int seconds = (unsigned int)ctx->rdi;
  sleep(seconds);
  ctx->rax = 0;
  SHIM_RETURN();
}

void shim_sceKernelStopUnloadModule(GuestContext *ctx) {
  (void)ctx;
  ctx->rax = 0;
  SHIM_RETURN();
}

void shim_sceKernelGetPrtAperture(GuestContext *ctx) {
  (void)ctx;
  ctx->rax = 0;
  SHIM_RETURN();
}

// ioctl
void shim_ioctl(GuestContext *ctx) {
  int fd = (int)ctx->rdi;
  unsigned long request = (unsigned long)ctx->rsi;
  uint64_t arg = ctx->rdx;

  void *host_arg = arg ? (void *)(ctx->mem_base + arg) : NULL;
  int ret = ioctl(fd, request, host_arg);
  if (ret < 0) {
    set_guest_errno(ctx, errno);
  }
  ctx->rax = (uint64_t)ret;
  SHIM_RETURN();
}

// poll
void shim_poll(GuestContext *ctx) {
  uint64_t fds_addr = ctx->rdi;
  nfds_t nfds = (nfds_t)ctx->rsi;
  int timeout = (int)ctx->rdx;

  if (nfds > 1024) {
    set_guest_errno(ctx, EINVAL);
    ctx->rax = (uint64_t)-1;
    SHIM_RETURN();
  }

  struct pollfd *guest_fds = (struct pollfd *)(ctx->mem_base + fds_addr);
  struct pollfd host_fds[nfds ? nfds : 1];
  for (nfds_t i = 0; i < nfds; i++) {
    host_fds[i].fd = guest_fds[i].fd;
    host_fds[i].events = guest_fds[i].events;
    host_fds[i].revents = 0;
  }

  int ret = poll(host_fds, nfds, timeout);
  if (ret >= 0) {
    for (nfds_t i = 0; i < nfds; i++) {
      guest_fds[i].revents = host_fds[i].revents;
    }
  } else {
    set_guest_errno(ctx, errno);
  }
  ctx->rax = (uint64_t)ret;
  SHIM_RETURN();
}

// sigaction
struct guest_sigaction {
  uint64_t handler;
  int32_t flags;
  uint32_t mask[4];
};

static struct guest_sigaction g_guest_sigactions[64] = {0};
static pthread_mutex_t g_sigaction_mutex = PTHREAD_MUTEX_INITIALIZER;

void shim_sigaction(GuestContext *ctx) {
  int sig = (int)ctx->rdi;
  uint64_t act_addr = ctx->rsi;
  uint64_t oact_addr = ctx->rdx;

  if (sig <= 0 || sig >= 64) {
    set_guest_errno(ctx, EINVAL);
    ctx->rax = (uint64_t)-1;
    SHIM_RETURN();
  }

  pthread_mutex_lock(&g_sigaction_mutex);
  if (oact_addr) {
    struct guest_sigaction *oact =
        (struct guest_sigaction *)(ctx->mem_base + oact_addr);
    *oact = g_guest_sigactions[sig];
  }
  if (act_addr) {
    struct guest_sigaction *act =
        (struct guest_sigaction *)(ctx->mem_base + act_addr);
    g_guest_sigactions[sig] = *act;
  }
  pthread_mutex_unlock(&g_sigaction_mutex);

  ctx->rax = 0;
  SHIM_RETURN();
}

// sigprocmask
void shim_sigprocmask(GuestContext *ctx) {
  int how = (int)ctx->rdi;
  uint64_t set_addr = ctx->rsi;
  uint64_t oldset_addr = ctx->rdx;

  sigset_t host_set, host_oldset;
  sigset_t *p_set = NULL;
  sigset_t *p_old = oldset_addr ? &host_oldset : NULL;

  if (set_addr) {
    uint32_t *guest_bits = (uint32_t *)(ctx->mem_base + set_addr);
    host_set = (sigset_t)guest_bits[0];
    p_set = &host_set;
  }

  if (p_set && how != SIG_UNBLOCK) {
    // Synchronous faults are how the runtime works (shadow-stack guard, host-call bridge, the AGC write watch): a
    // blocked SIGSEGV or SIGBUS would kill the process at the next one. The guest's own handlers are not host handlers.
    sigdelset(&host_set, SIGSEGV);
    sigdelset(&host_set, SIGBUS);
    sigdelset(&host_set, SIGILL);
    sigdelset(&host_set, SIGTRAP);
  }
  int ret = sigprocmask(how, p_set, p_old);
  if (ret == 0) {
    if (oldset_addr) {
      uint32_t *guest_old = (uint32_t *)(ctx->mem_base + oldset_addr);
      guest_old[0] = (uint32_t)host_oldset;
      guest_old[1] = 0;
      guest_old[2] = 0;
      guest_old[3] = 0;
    }
  } else {
    set_guest_errno(ctx, errno);
  }
  ctx->rax = (uint64_t)ret;
  SHIM_RETURN();
}

// raise
void shim_raise(GuestContext *ctx) {
  int sig = (int)ctx->rdi;
  int ret = raise(sig);
  if (ret < 0) {
    set_guest_errno(ctx, errno);
  }
  ctx->rax = (uint64_t)ret;
  SHIM_RETURN();
}

// Raw SYSCALL instruction entry (lifted by the lifter's liftSyscall through _recomp_call_shim).
// Kernel convention: rax = number, args rdi, rsi, rdx, r10, r8, r9; result in rax; on error
// CF is set and rax = errno (FreeBSD). The instruction also clobbers rcx and r11 (r11 = rflags,
// so r11 bit 0 carries CF back to the lowering, which loads it into the C flag).
// The numbers are served by shim_syscall (libc syscall() convention: rdi = number, args
// rsi, rdx, rcx, r8, r9, 7th on the stack at [rsp+8]), after remapping the registers.
void shim_raw_syscall(GuestContext *ctx) {
  uint64_t num = ctx->rax, a1 = ctx->rdi, a2 = ctx->rsi, a3 = ctx->rdx;
  uint64_t a4 = ctx->r10, a5 = ctx->r8, a6 = ctx->r9;
  uint64_t rsp0 = ctx->rsp;
  // A fake call frame below the red zone: [rsp] = return slot (popped by SHIM_RETURN),
  // [rsp+8] = the sixth argument (mmap's offset).
  uint64_t fake = (rsp0 - 128 - 16) & ~0xfULL;
  uint64_t *frame = (uint64_t *)NPS5_G2H(ctx, fake); // guest stack: always mapped below rsp
  frame[0] = 0;
  frame[1] = a6;
  ctx->rsp = fake;
  ctx->rdi = num;
  ctx->rsi = a1;
  ctx->rdx = a2;
  ctx->rcx = a3;
  ctx->r8 = a4;
  ctx->r9 = a5;
  int *errp = ctx->fs_base ? (int *)NPS5_G2H(ctx, ctx->fs_base + 0x100ULL) : NULL; // set_guest_errno's slot
  if (errp)
    *errp = 0;
  shim_syscall(ctx);
  ctx->rsp = rsp0;
  uint64_t cf = 0;
  if (ctx->rax == (uint64_t)-1) {
    int e = errp ? *errp : 0;
    ctx->rax = (uint64_t)(e ? e : EINVAL);
    cf = 1;
  }
  ctx->rcx = 0;
  ctx->r11 = 0x202 | cf;
}

// The SYSCALL instruction in the flat call model (liftSyscall): the lowering pushes the guest
// return address and a shadow entry and goes through the shim bridge like a call of a shim stub,
// so the bridge expects the shim to pop the return address (SHIM_RETURN).
void shim_raw_syscall_flat(GuestContext *ctx) {
  shim_raw_syscall(ctx);
  ctx->rsp += 8;
}

// syscall — numbers must match emitter.ImplementedSyscalls.
void shim_syscall(GuestContext *ctx) {
  int num = (int)ctx->rdi;
  switch (num) {
  case 1: { // SYS_exit
    int status = (int)ctx->rsi;
    ps4_host_quit(status); // as shim_exit
    break;
  }
  case 3: { // SYS_read
    int fd = (int)ctx->rsi;
    uint64_t buf = ctx->rdx;
    size_t count = (size_t)ctx->rcx;
    ssize_t ret;
    // through the VFS fd table like shim_read: a pak virtual fd (>= PS4_VFS_VIRTUAL_FD_BASE) is not a host descriptor
    PS4_HITCH_TIMED(PS4_HITCH_IO, ret = ps4_vfs_read(fd, ctx->mem_base + buf, count));
    if (ret < 0)
      set_guest_errno(ctx, errno);
    ctx->rax = (uint64_t)ret;
    SHIM_RETURN();
  }
  case 4: { // SYS_write
    int fd = (int)ctx->rsi;
    uint64_t buf = ctx->rdx;
    size_t count = (size_t)ctx->rcx;
    ssize_t ret;
    if (ps4_vfs_is_virtual_fd(fd)) { errno = EBADF; ret = -1; } // pak files are read-only; never write a host fd by a virtual number
    else ret = nps5_kwrite(fd, ctx->mem_base + buf, count);
    if (ret < 0)
      set_guest_errno(ctx, errno);
    ctx->rax = (uint64_t)ret;
    SHIM_RETURN();
  }
  case 5: { // SYS_open
    uint64_t path_addr = ctx->rsi;
    int flags = (int)ctx->rdx;
    mode_t mode = (mode_t)ctx->rcx;
    const char *path = (const char *)(ctx->mem_base + path_addr);
    const char *actual_path = path;
    bool is_direct = false;
    int host_flags = ps4_open_flags_to_host(flags, &is_direct);
    int ret = ps4_vfs_open(path, host_flags, mode); // the same gate as shim_open (EROFS for the application image)
    if (ret < 0) {
      set_guest_errno(ctx, errno);
    } else {
#if defined(__APPLE__) && defined(F_NOCACHE)
      if (is_direct) {
        fcntl(ret, F_NOCACHE, 1);
      }
#endif
    }
    PS4_LOGF(NPS5_S_SYS, NPS5_L_DEBUG, "ps4-sys", "syscall 5 (SYS_open): '%s' (resolved='%s', flags=0x%x, "
           "host_flags=0x%x) -> fd=%d\n",
           path ? path : "(null)", actual_path ? actual_path : "(null)", flags,
           host_flags, ret);
    fflush(stdout);
    ctx->rax = (uint64_t)ret;
    SHIM_RETURN();
  }
  case 6: { // SYS_close
    int fd = (int)ctx->rsi;
    guest_dir_cache_drop_fd(fd);
    int ret = ps4_vfs_close(fd); // never close(2) a host fd by a virtual number
    if (ret < 0)
      set_guest_errno(ctx, errno);
    ctx->rax = (uint64_t)ret;
    SHIM_RETURN();
  }
  case 20: // SYS_getpid
    ctx->rax = ctx->thread_id ? ctx->thread_id : 1;
    SHIM_RETURN();
  case 24: // SYS_getuid
  case 25: // SYS_geteuid
  case 47: // SYS_getgid
    ctx->rax = 1;
    SHIM_RETURN();
  case 73: { // SYS_munmap
    uint64_t addr = ctx->rsi;
    size_t len = (size_t)ctx->rdx;
    int ret = recomp_vm_free(ctx, addr, len);
    if (ret != 0) {
      set_guest_errno(ctx, EINVAL);
      ctx->rax = (uint64_t)-1;
    } else {
      ctx->rax = 0;
    }
    SHIM_RETURN();
  }
  case 74: { // SYS_mprotect
    uint64_t addr = ctx->rsi;
    size_t len = (size_t)ctx->rdx;
    int prot = (int)ctx->rcx;
    // Under vm_mutex, splitting at the range edges (a bare write through a looked-up extent pointer
    // raced with unmap and covered the whole extent). Best effort, as before: the result is 0.
    (void)recomp_vm_protect(ctx, addr, len, prot, -1);
    ctx->rax = 0;
    SHIM_RETURN();
  }
  case 432: // SYS_thr_self
    ctx->rax = ctx->thread_id ? ctx->thread_id : 1;
    SHIM_RETURN();
  case 477: { // SYS_mmap(void *addr, size_t len, int prot, int flags, int fd,
              // off_t offset)
    uint64_t addr = ctx->rsi;
    size_t len = (size_t)ctx->rdx;
    int prot = (int)ctx->rcx;
    int flags = (int)ctx->r8;
    int fd = (int)ctx->r9;
    off_t offset = 0;
    if (ctx->rsp && RECOMP_MEM_OK(ctx) && ctx->rsp + 16 <= ctx->mem_size) {
      offset = (off_t) * (uint64_t *)(ctx->mem_base + ctx->rsp + 8);
    }

    PS4_LOGF(NPS5_S_SYS, NPS5_L_DEBUG, "ps4-sys", "syscall 477 (SYS_mmap): addr=0x%llx len=%zu (%.2f MB) "
           "prot=0x%x flags=0x%x fd=%d offset=%lld\n",
           (unsigned long long)addr, len, (double)len / (1024.0 * 1024.0), prot,
           flags, fd, (long long)offset);
    fflush(stdout);

    if (len == 0) {
      set_guest_errno(ctx, EINVAL);
      ctx->rax = (uint64_t)-1;
      SHIM_RETURN();
    }

    size_t align = 65536;
    size_t aligned_len = (len + align - 1) & ~(align - 1);

    // A fixed request covers exactly the pages named (16 KiB granularity); only the placement of

    // unhinted mappings uses the 64 KiB allocation granule.

    if (addr != 0 && (flags & 0x10)) aligned_len = (len + NPS5_PAGE_MASK) & ~NPS5_PAGE_MASK;

    int merr = 0;
    uint64_t alloc_addr = recomp_vm_map_ex(ctx, addr, aligned_len, align, prot, flags, "mmap_sys", NPS5_EXT_MAPPED, &merr);

    if (alloc_addr == (uint64_t)-1) {
      PS4_LOGF(NPS5_S_SYS, NPS5_L_WARN, "ps4-sys", "ERROR: syscall 477 (SYS_mmap) FAILED: addr=0x%llx "
              "len=%zu\n",
              (unsigned long long)addr, len);
      set_guest_errno(ctx, merr == EINVAL ? EINVAL : ENOMEM);
      ctx->rax = (uint64_t)-1;
    } else {
      if (!(flags & 0x1000) && fd >= 0) {
        ssize_t nread;
        PS4_HITCH_TIMED(PS4_HITCH_IO,
                        nread =
                            nps5_kpread(fd, ctx->mem_base + alloc_addr, len, offset));
        if (nread < 0) {
          PS4_LOGF(NPS5_S_SYS, NPS5_L_WARN, "ps4-sys", "syscall 477 file read FAILED: fd=%d offset=%lld "
                  "len=%zu errno=%d (%s)\n",
                  fd, (long long)offset, len, errno, strerror(errno));
          set_guest_errno(ctx, errno);
          recomp_vm_free(ctx, alloc_addr, aligned_len);
          ctx->rax = (uint64_t)-1;
          SHIM_RETURN();
        }
        PS4_LOGF(NPS5_S_SYS, NPS5_L_DEBUG, "ps4-sys", "syscall 477 file read SUCCESS: read=%zd bytes into "
               "0x%llx\n",
               nread, (unsigned long long)alloc_addr);
        fflush(stdout);
      }
      PS4_LOGF(NPS5_S_SYS, NPS5_L_DEBUG, "ps4-sys", "syscall 477 SUCCESS: out=0x%llx\n",
             (unsigned long long)alloc_addr);
      fflush(stdout);
      ctx->rax = alloc_addr;
    }
    SHIM_RETURN();
  }
  case 116: { // SYS_gettimeofday
    shim_gettimeofday(ctx);
    return;
  }
  case 118: { // SYS_getrusage
    shim_getrusage(ctx);
    return;
  }
  case 232: { // SYS_clock_gettime
    shim_clock_gettime(ctx);
    return;
  }
  default:
    PS4_LOGF(NPS5_S_SYS, NPS5_L_WARN, "ps4-sys", "WARN: Unsupported syscall num=%d (rsi=0x%llx, "
            "rdx=0x%llx, rcx=0x%llx, r8=0x%llx)\n",
            num, (unsigned long long)ctx->rsi, (unsigned long long)ctx->rdx,
            (unsigned long long)ctx->rcx, (unsigned long long)ctx->r8);
    set_guest_errno(ctx, ENOSYS);
    ctx->rax = (uint64_t)-1;
    SHIM_RETURN();
  }
}

// Stack protector canary check failure
void shim___stack_chk_fail(GuestContext *ctx) {
  uint64_t ret_addr = (ctx && RECOMP_MEM_OK(ctx) && ctx->rsp + 8 <= ctx->mem_size)
                          ? MEM_U64(ctx->rsp)
                          : 0;
  uint64_t r14_val = (ctx && RECOMP_MEM_OK(ctx) && ctx->r14 + 8 <= ctx->mem_size)
                         ? MEM_U64(ctx->r14)
                         : 0;
  uint64_t stack_canary_30 = (ctx && RECOMP_MEM_OK(ctx) && ctx->rbp >= 0x30 &&
                              ctx->rbp - 0x30 + 8 <= ctx->mem_size)
                                 ? MEM_U64(ctx->rbp - 0x30)
                                 : 0;
  uint64_t stack_canary_28 = (ctx && RECOMP_MEM_OK(ctx) && ctx->rbp >= 0x28 &&
                              ctx->rbp - 0x28 + 8 <= ctx->mem_size)
                                 ? MEM_U64(ctx->rbp - 0x28)
                                 : 0;
  fprintf(
      stderr,
      "*** stack smashing detected ***: terminated (caller=0x%llx, rsp=0x%llx, "
      "rbp=0x%llx, r14=0x%llx, *r14=0x%llx, [rbp-0x30]=0x%llx, "
      "[rbp-0x28]=0x%llx, rax=0x%llx, rcx=0x%llx)\n",
      (unsigned long long)ret_addr, (unsigned long long)(ctx ? ctx->rsp : 0),
      (unsigned long long)(ctx ? ctx->rbp : 0),
      (unsigned long long)(ctx ? ctx->r14 : 0), (unsigned long long)r14_val,
      (unsigned long long)stack_canary_30, (unsigned long long)stack_canary_28,
      (unsigned long long)(ctx ? ctx->rax : 0),
      (unsigned long long)(ctx ? ctx->rcx : 0));
  if (ctx) {
    recomp_dump_guest_context(ctx);
  }
  fflush(stderr);
  abort();
}

// clock_gettime(clockid_t clk_id, struct timespec *tp)
void shim_clock_gettime(GuestContext *ctx) {
  int clk_id = (int)ctx->rdi;
  uint64_t tp_addr = ctx->rsi;
  if (!tp_addr) {
    set_guest_errno(ctx, EFAULT);
    ctx->rax = (uint64_t)-1;
    SHIM_RETURN();
  }

  clockid_t host_clk = CLOCK_REALTIME;
  // FreeBSD clock IDs:
  // 0: CLOCK_REALTIME
  // 4: CLOCK_MONOTONIC
  // 5: CLOCK_UPTIME
  if (clk_id == 4 || clk_id == 5) {
    host_clk = CLOCK_MONOTONIC;
  }

  struct timespec host_ts;
  int ret = clock_gettime(host_clk, &host_ts);
  if (ret < 0) {
    set_guest_errno(ctx, errno);
    ctx->rax = (uint64_t)-1;
  } else {
    struct guest_timespec {
      int64_t tv_sec;
      int64_t tv_nsec;
    };
    struct guest_timespec *g_ts =
        (struct guest_timespec *)(ctx->mem_base + tp_addr);
    g_ts->tv_sec = (int64_t)host_ts.tv_sec;
    g_ts->tv_nsec = (int64_t)host_ts.tv_nsec;
    ctx->rax = 0;
  }
  SHIM_RETURN();
}

// gettimeofday(struct timeval *tv, struct timezone *tz)
void shim_gettimeofday(GuestContext *ctx) {
  uint64_t tv_addr = ctx->rdi;
  uint64_t tz_addr = ctx->rsi;

  struct timeval host_tv;
  struct timezone host_tz;
  struct timezone *tz_ptr = tz_addr ? &host_tz : NULL;

  int ret = gettimeofday(&host_tv, tz_ptr);
  if (ret < 0) {
    set_guest_errno(ctx, errno);
    ctx->rax = (uint64_t)-1;
  } else {
    if (tv_addr) {
      struct guest_timeval {
        int64_t tv_sec;
        int64_t tv_usec;
      };
      struct guest_timeval *g_tv =
          (struct guest_timeval *)(ctx->mem_base + tv_addr);
      g_tv->tv_sec = (int64_t)host_tv.tv_sec;
      g_tv->tv_usec = (int64_t)host_tv.tv_usec;
    }
    if (tz_addr) {
      struct timezone *g_tz = (struct timezone *)(ctx->mem_base + tz_addr);
      *g_tz = host_tz;
    }
    ctx->rax = 0;
  }
  SHIM_RETURN();
}

// getrusage(int who, struct rusage *usage)
void shim_getrusage(GuestContext *ctx) {
  int who = (int)ctx->rdi;
  uint64_t usage_addr = ctx->rsi;
  if (!usage_addr) {
    set_guest_errno(ctx, EFAULT);
    ctx->rax = (uint64_t)-1;
    SHIM_RETURN();
  }

  // Map FreeBSD who (0 = RUSAGE_SELF, -1 = RUSAGE_CHILDREN)
  int host_who = RUSAGE_SELF;
  if (who == -1) {
    host_who = RUSAGE_CHILDREN;
  }

  struct rusage host_ru;
  int ret = getrusage(host_who, &host_ru);
  if (ret < 0) {
    set_guest_errno(ctx, errno);
    ctx->rax = (uint64_t)-1;
  } else {
    struct rusage *g_ru = (struct rusage *)(ctx->mem_base + usage_addr);
    memset(g_ru, 0, sizeof(struct rusage));
    g_ru->ru_utime = host_ru.ru_utime;
    g_ru->ru_stime = host_ru.ru_stime;
    g_ru->ru_maxrss = host_ru.ru_maxrss;
    ctx->rax = 0;
  }
  SHIM_RETURN();
}

// FreeBSD rlim_t is signed; RLIM_INFINITY is 0x7fffffffffffffff.
static uint64_t guest_rlim(rlim_t v) {
  if (v == RLIM_INFINITY) {
    return 0x7fffffffffffffffULL;
  }
  return (uint64_t)v;
}

static int host_rlimit_resource(int freebsd_res) {
  switch (freebsd_res) {
  case 0:
    return RLIMIT_CPU;
  case 1:
    return RLIMIT_FSIZE;
  case 2:
    return RLIMIT_DATA;
  case 3:
    return RLIMIT_STACK;
  case 4:
    return RLIMIT_CORE;
  case 5:
#ifdef RLIMIT_RSS
    return RLIMIT_RSS;
#else
    return RLIMIT_AS;
#endif
  case 6:
    return RLIMIT_MEMLOCK;
  case 7:
    return RLIMIT_NPROC;
  case 8:
    return RLIMIT_NOFILE;
  case 10:
    return RLIMIT_AS;
  default:
    return -1;
  }
}

// getrlimit(int resource, struct rlimit *rlp)
void shim_getrlimit(GuestContext *ctx) {
  int resource = (int)ctx->rdi;
  uint64_t rlp = ctx->rsi;
  if (!rlp) {
    set_guest_errno(ctx, EFAULT);
    ctx->rax = (uint64_t)-1;
    SHIM_RETURN();
  }

  int host_res = host_rlimit_resource(resource);
  uint64_t *out = (uint64_t *)(ctx->mem_base + rlp);
  if (host_res < 0) {
    out[0] = 0x7fffffffffffffffULL;
    out[1] = 0x7fffffffffffffffULL;
    ctx->rax = 0;
    SHIM_RETURN();
  }

  struct rlimit lim;
  if (getrlimit(host_res, &lim) != 0) {
    set_guest_errno(ctx, errno);
    ctx->rax = (uint64_t)-1;
    SHIM_RETURN();
  }
  out[0] = guest_rlim(lim.rlim_cur);
  out[1] = guest_rlim(lim.rlim_max);
  ctx->rax = 0;
  SHIM_RETURN();
}

// cpuset_getaffinity(cpulevel_t, cpuwhich_t, id_t, size_t, cpuset_t *)
void shim_cpuset_getaffinity(GuestContext *ctx) {
  size_t setsize = (size_t)ctx->rcx;
  uint64_t mask_addr = ctx->r8;
  if (!mask_addr || setsize == 0 || mask_addr + setsize > ctx->mem_size) {
    set_guest_errno(ctx, EFAULT);
    ctx->rax = (uint64_t)-1;
    SHIM_RETURN();
  }

  uint8_t *mask = ctx->mem_base + mask_addr;
  memset(mask, 0, setsize);

  long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
  if (ncpu < 1) {
    ncpu = 1;
  }
  for (long i = 0; i < ncpu; i++) {
    size_t word = (size_t)i / 64;
    unsigned shift = (unsigned)i % 64;
    size_t off = word * 8;
    if (off + 8 > setsize) {
      break;
    }
    uint64_t bits;
    memcpy(&bits, mask + off, 8);
    bits |= (uint64_t)1 << shift;
    memcpy(mask + off, &bits, 8);
  }
  ctx->rax = 0;
  SHIM_RETURN();
}

void shim_memcpy(GuestContext *ctx) {
  uint64_t dst = ctx->rdi;
  uint64_t src = ctx->rsi;
  size_t n = (size_t)ctx->rdx;
  memcpy(ctx->mem_base + dst, ctx->mem_base + src, n);
  ctx->rax = dst;
  SHIM_RETURN();
}

void shim_memmove(GuestContext *ctx) {
  uint64_t dst = ctx->rdi;
  uint64_t src = ctx->rsi;
  size_t n = (size_t)ctx->rdx;
  memmove(ctx->mem_base + dst, ctx->mem_base + src, n);
  ctx->rax = dst;
  SHIM_RETURN();
}

void shim_memset(GuestContext *ctx) {
  uint64_t dst = ctx->rdi;
  int c = (int)(uint8_t)ctx->rsi;
  size_t n = (size_t)ctx->rdx;
  memset(ctx->mem_base + dst, c, n);
  ctx->rax = dst;
  SHIM_RETURN();
}

void shim_strlen(GuestContext *ctx) {
  const char *s = (const char *)(ctx->mem_base + ctx->rdi);
  ctx->rax = (uint64_t)strlen(s);
  SHIM_RETURN();
}

void shim_strcpy(GuestContext *ctx) {
  uint64_t dst = ctx->rdi;
  char *d = (char *)(ctx->mem_base + dst);
  const char *s = (const char *)(ctx->mem_base + ctx->rsi);
  strcpy(d, s);
  ctx->rax = dst;
  SHIM_RETURN();
}

void shim_strncpy(GuestContext *ctx) {
  uint64_t dst = ctx->rdi;
  char *d = (char *)(ctx->mem_base + dst);
  const char *s = (const char *)(ctx->mem_base + ctx->rsi);
  size_t n = (size_t)ctx->rdx;
  strncpy(d, s, n);
  ctx->rax = dst;
  SHIM_RETURN();
}

void shim_strcmp(GuestContext *ctx) {
  const char *a = (const char *)(ctx->mem_base + ctx->rdi);
  const char *b = (const char *)(ctx->mem_base + ctx->rsi);
  ctx->rax = (uint64_t)(uint32_t)(int32_t)strcmp(a, b);
  SHIM_RETURN();
}


// sceKernel* file calls: same operations as the POSIX shims, SCE error codes instead of -1.
void shim_sceKernelMkdir(GuestContext *ctx) { sce_wrap(ctx, shim_mkdir); }
void shim_sceKernelUnlink(GuestContext *ctx) { sce_wrap(ctx, shim_unlink); }
void shim_sceKernelRename(GuestContext *ctx) { sce_wrap(ctx, shim_rename); }
void shim_sceKernelTruncate(GuestContext *ctx) { sce_wrap(ctx, shim_truncate); }
void shim_sceKernelChmod(GuestContext *ctx) { sce_wrap(ctx, shim_chmod); }
void shim_sceKernelUtimes(GuestContext *ctx) { sce_wrap(ctx, shim_utimes); }
void shim_sceKernelGetdents(GuestContext *ctx) { sce_wrap(ctx, shim_getdents); }
