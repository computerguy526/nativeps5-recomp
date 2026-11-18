// SPDX-License-Identifier: GPL-2.0-or-later
//
// System calls and C library calls that fill or read a buffer which may be guest memory (docs/m3/writewatch.md).
//
// A host library can keep guest pages write-protected or inaccessible on the host (the AGC write watch, nps5_mem.h host
// access hooks). A store or load by guest or host code faults and is resolved in the signal handler; the kernel does
// not fault: read() into such a page fails with EFAULT, write() from an inaccessible one too. These wrappers open the
// destination for the duration of the call (nps5_host_write_begin/end) or bring the source up to date first
// (nps5_host_read_prepare). For a buffer outside the guest reservation, and without hooks, they cost two compares.
// Use them in every HLE path that hands a guest pointer to the kernel or to a library that fills it in kernel mode.
// errno is that of the wrapped call.
#ifndef NPS5_KIO_H
#define NPS5_KIO_H

#include <errno.h>
#include <stdio.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#include "nps5_mem.h"

// Weak no-op defaults, so a unit test that links a few runtime files without core/nps5_mem.c still links; the runtime's
// definitions (strong, nps5_mem.c) replace them. Weak functions are never inlined.
__attribute__((weak)) void nps5_host_write_begin(void *p, size_t len) { (void)p; (void)len; }
__attribute__((weak)) void nps5_host_write_end(void *p, size_t len) { (void)p; (void)len; }
__attribute__((weak)) void nps5_host_read_prepare(const void *p, size_t len) { (void)p; (void)len; }
__attribute__((weak)) bool nps5_host_access_active(void) { return false; }
__attribute__((weak)) bool nps5_host_write_refault(void *p, size_t len, unsigned *tries) { (void)p; (void)len; (void)tries; return false; }

#define NPS5_KIO_FILL(T, p, len, call)       \
  do {                                       \
    void *kio_p = (void *)(p);               \
    const size_t kio_n = (size_t)(len);      \
    nps5_host_write_begin(kio_p, kio_n);     \
    const T kio_r = (call);                  \
    const int kio_e = errno;                 \
    nps5_host_write_end(kio_p, kio_n);       \
    errno = kio_e;                           \
    return kio_r;                            \
  } while (0)

static inline void nps5_kio_iov(const struct iovec *iov, int n, int phase) { // phase 0: begin, 1: end, 2: read
  // Without hooks the array is not touched here at all: the kernel validates it (an unmapped array is EFAULT, not a crash).
  // With hooks a bad array faults here, in user mode, and is reported as the guest's bad pointer.
  if (!iov || n <= 0 || !nps5_host_access_active()) return;
  for (int i = 0; i < n; i++) {
    if (phase == 0) nps5_host_write_begin(iov[i].iov_base, iov[i].iov_len);
    else if (phase == 1) nps5_host_write_end(iov[i].iov_base, iov[i].iov_len);
    else nps5_host_read_prepare(iov[i].iov_base, iov[i].iov_len);
  }
}

// read/pread: write_begin opens the pages, but nothing keeps them open until the kernel copies; a page closed again in
// between is EFAULT for a valid buffer (nps5_host_write_refault): open again and repeat. A sequential read that failed
// may have moved the file position (bytes copied before the closed page): it is put back first where the file can seek.
static inline ssize_t nps5_kpread(int fd, void *buf, size_t n, off_t off) {
  unsigned tries = 0;
  nps5_host_write_begin(buf, n);
  for (;;) {
    const ssize_t r = pread(fd, buf, n, off);
    const int e = errno;
    if (r < 0 && e == EFAULT && nps5_host_write_refault(buf, n, &tries)) continue;
    nps5_host_write_end(buf, n);
    errno = e;
    return r;
  }
}
static inline ssize_t nps5_kread(int fd, void *buf, size_t n) {
  unsigned tries = 0;
  const off_t pos = nps5_host_access_active() ? lseek(fd, 0, SEEK_CUR) : (off_t)-1;
  nps5_host_write_begin(buf, n);
  for (;;) {
    const ssize_t r = read(fd, buf, n);
    const int e = errno;
    if (r < 0 && e == EFAULT && nps5_host_write_refault(buf, n, &tries)) {
      if (pos >= 0) lseek(fd, pos, SEEK_SET);
      continue;
    }
    nps5_host_write_end(buf, n);
    errno = e;
    return r;
  }
}
static inline ssize_t nps5_krecv(int s, void *buf, size_t n, int flags) { NPS5_KIO_FILL(ssize_t, buf, n, recv(s, buf, n, flags)); }
// fread: stdio reads a large request straight into the caller's buffer with read(), so the same EFAULT applies (a short
// count with the error flag set: savedata and libc fread into guest memory). Repeated from the original stream position
// (clearerr + fseeko drop the stdio buffer) where the stream can seek; a pipe keeps the short count.
static inline size_t nps5_kfread(void *buf, size_t size, size_t count, FILE *f) {
  const size_t n = size * count;
  unsigned tries = 0;
  const off_t pos = (nps5_host_access_active() && n) ? ftello(f) : (off_t)-1;
  nps5_host_write_begin(buf, n);
  for (;;) {
    errno = 0;
    const size_t r = fread(buf, size, count, f);
    const int e = errno;
    // seek first: if the stream cannot go back, its error flag stays set (the caller sees a read error, not an EOF)
    if (r < count && pos >= 0 && ferror(f) && e == EFAULT && nps5_host_write_refault(buf, n, &tries) && fseeko(f, pos, SEEK_SET) == 0) {
      clearerr(f);
      continue;
    }
    nps5_host_write_end(buf, n);
    errno = e;
    return r;
  }
}
// readv: as nps5_kread, for every buffer of the vector; the file position is put back before a repeat where it can seek.
static inline ssize_t nps5_kreadv(int fd, const struct iovec *iov, int n) {
  nps5_kio_iov(iov, n, 0);
  const off_t pos = (iov && n > 0 && nps5_host_access_active()) ? lseek(fd, 0, SEEK_CUR) : (off_t)-1;
  ssize_t r = readv(fd, iov, n);
  int e = errno;
  for (unsigned t = 0; r < 0 && e == EFAULT && pos >= 0 && t < NPS5_KIO_RETRIES; t++) {
    unsigned one = 0;
    bool ours = false;
    for (int i = 0; i < n; i++) { one = 0; ours |= nps5_host_write_refault(iov[i].iov_base, iov[i].iov_len, &one); }
    if (!ours || lseek(fd, pos, SEEK_SET) != pos) break;
    r = readv(fd, iov, n);
    e = errno;
  }
  nps5_kio_iov(iov, n, 1);
  errno = e;
  return r;
}
static inline ssize_t nps5_kpreadv(int fd, const struct iovec *iov, int n, off_t off) {
  nps5_kio_iov(iov, n, 0);
  ssize_t r = preadv(fd, iov, n, off);
  int e = errno;
  // a page closed again before the kernel copied (see nps5_kpread): open every buffer again and repeat
  for (unsigned t = 0; r < 0 && e == EFAULT && nps5_host_access_active() && t < NPS5_KIO_RETRIES; t++) {
    unsigned one = 0;
    bool ours = false;
    for (int i = 0; i < n; i++) { one = 0; ours |= nps5_host_write_refault(iov[i].iov_base, iov[i].iov_len, &one); }
    if (!ours) break;
    r = preadv(fd, iov, n, off);
    e = errno;
  }
  nps5_kio_iov(iov, n, 1);
  errno = e;
  return r;
}
// A socket address and its length word, both written by the kernel.
static inline void nps5_kio_addr(struct sockaddr *a, socklen_t *l, int end) {
  if (l) (end ? nps5_host_write_end : nps5_host_write_begin)(l, sizeof *l);
  if (a) (end ? nps5_host_write_end : nps5_host_write_begin)(a, 256); // the caller's length is itself in guest memory: a bound
}
static inline ssize_t nps5_krecvfrom(int s, void *buf, size_t n, int flags, struct sockaddr *a, socklen_t *l) {
  nps5_host_write_begin(buf, n);
  nps5_kio_addr(a, l, 0);
  const ssize_t r = recvfrom(s, buf, n, flags, a, l);
  const int e = errno;
  nps5_kio_addr(a, l, 1);
  nps5_host_write_end(buf, n);
  errno = e;
  return r;
}
static inline ssize_t nps5_krecvmsg(int s, struct msghdr *m, int flags) {
  if (!nps5_host_access_active()) return recvmsg(s, m, flags);
  if (m) {
    nps5_kio_iov(m->msg_iov, m->msg_iovlen, 0);
    nps5_host_write_begin(m->msg_name, m->msg_namelen);
    nps5_host_write_begin(m->msg_control, m->msg_controllen);
  }
  const size_t nl = m ? m->msg_namelen : 0, cl = m ? m->msg_controllen : 0;
  const ssize_t r = recvmsg(s, m, flags);
  const int e = errno;
  if (m) {
    nps5_host_write_end(m->msg_control, cl);
    nps5_host_write_end(m->msg_name, nl);
    nps5_kio_iov(m->msg_iov, m->msg_iovlen, 1);
  }
  errno = e;
  return r;
}
#define NPS5_KIO_ADDR(call)        \
  do {                             \
    nps5_kio_addr(a, l, 0);        \
    const int kio_r = (call);      \
    const int kio_e = errno;       \
    nps5_kio_addr(a, l, 1);        \
    errno = kio_e;                 \
    return kio_r;                  \
  } while (0)
static inline int nps5_kaccept(int s, struct sockaddr *a, socklen_t *l) { NPS5_KIO_ADDR(accept(s, a, l)); }
static inline int nps5_kgetsockname(int s, struct sockaddr *a, socklen_t *l) { NPS5_KIO_ADDR(getsockname(s, a, l)); }
static inline int nps5_kgetpeername(int s, struct sockaddr *a, socklen_t *l) { NPS5_KIO_ADDR(getpeername(s, a, l)); }
static inline int nps5_kgetsockopt(int s, int level, int name, void *v, socklen_t *l) {
  struct sockaddr *a = (struct sockaddr *)v;
  NPS5_KIO_ADDR(getsockopt(s, level, name, v, l));
}
static inline int nps5_kselect(int n, fd_set *r, fd_set *w, fd_set *x, struct timeval *t) {
  void *const p[4] = {r, w, x, t};
  const size_t z[4] = {sizeof(fd_set), sizeof(fd_set), sizeof(fd_set), sizeof(struct timeval)};
  for (int i = 0; i < 4; i++)
    if (p[i]) nps5_host_write_begin(p[i], z[i]);
  const int ret = select(n, r, w, x, t);
  const int e = errno;
  for (int i = 3; i >= 0; i--)
    if (p[i]) nps5_host_write_end(p[i], z[i]);
  errno = e;
  return ret;
}

// The kernel reads the buffer.
static inline ssize_t nps5_kwrite(int fd, const void *buf, size_t n) { nps5_host_read_prepare(buf, n); return write(fd, buf, n); }
static inline ssize_t nps5_kpwrite(int fd, const void *buf, size_t n, off_t off) { nps5_host_read_prepare(buf, n); return pwrite(fd, buf, n, off); }
static inline ssize_t nps5_kwritev(int fd, const struct iovec *iov, int n) { nps5_kio_iov(iov, n, 2); return writev(fd, iov, n); }
static inline ssize_t nps5_ksend(int s, const void *buf, size_t n, int flags) { nps5_host_read_prepare(buf, n); return send(s, buf, n, flags); }
static inline ssize_t nps5_ksendto(int s, const void *buf, size_t n, int flags, const struct sockaddr *a, socklen_t l) {
  nps5_host_read_prepare(buf, n);
  nps5_host_read_prepare(a, l);
  return sendto(s, buf, n, flags, a, l);
}
static inline ssize_t nps5_ksendmsg(int s, const struct msghdr *m, int flags) {
  if (m && nps5_host_access_active()) {
    nps5_kio_iov(m->msg_iov, m->msg_iovlen, 2);
    nps5_host_read_prepare(m->msg_name, m->msg_namelen);
    nps5_host_read_prepare(m->msg_control, m->msg_controllen);
  }
  return sendmsg(s, m, flags);
}
static inline size_t nps5_kfwrite(const void *buf, size_t size, size_t count, FILE *f) {
  nps5_host_read_prepare(buf, size * count);
  return fwrite(buf, size, count, f);
}

#endif
