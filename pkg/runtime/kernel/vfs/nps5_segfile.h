// SPDX-License-Identifier: GPL-2.0-or-later
//
// Segmented files (tools-host/gamepack.py in the entry repository). APFS transparent compression cannot compress a
// file over 2 GiB, so gamepack stores a big read-only game file <dir>/<name> as <dir>/<name>.nps5seg/: segments
// 0000, 0001, ... (every one seg_size bytes except the last, each compressed by APFS) and manifest.json
// ("format": "nps5seg/1", "total_size", "seg_size", "seg_count", sha256 of every segment and of the whole file).
// The VFS presents such a directory to the guest as the original single file <name> (open, stat, read, pread, lseek,
// fstat, mmap copy-in, readv, aio, AMPR, directory listings), when <name> itself is missing.
//
// NPS5_VFS_SEGFILES=1 turns it on (default off). NPS5_VFS_SEGFILES_FDS=<n> caps the segment fds kept open across all
// segmented files (least recently used unpinned ones are closed first; default 64).
// Log: "[vfs] segfile <path>: N segments" once per file when it is first used.
#ifndef NPS5_SEGFILE_H
#define NPS5_SEGFILE_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nps5_segfile nps5_segfile;

int nps5_segfile_enabled(void);

// The segmented file presented at host path <host> (which does not exist itself), or NULL: feature off, no
// <host>.nps5seg directory (the name is matched case-insensitively, like the VFS's own lookup), or a manifest that
// does not match its segments (logged). The handle lives until nps5_segfile_reset (never freed while running).
nps5_segfile *nps5_segfile_lookup(const char *host);

uint64_t nps5_segfile_size(const nps5_segfile *f);
uint32_t nps5_segfile_count(const nps5_segfile *f);

// One pread inside the segment that holds logical offset <off>: at most the bytes left in that segment. Same
// contract as pread(2): bytes read, 0 at the logical EOF, -1 with errno. A segment that ends before the manifest
// says it should is EIO. The caller loops (a read across a segment boundary is two or more calls).
ssize_t nps5_segfile_pread_piece(nps5_segfile *f, void *buf, size_t n, uint64_t off);

// A whole read across segments. <rd> does each piece's pread on the segment fd (NULL = pread(2); the AMPR worker
// passes its EFAULT-refaulting reader). Returns the bytes read (fewer only at EOF or after an error), or -1 with errno
// when nothing could be read.
typedef ssize_t (*nps5_segfile_reader)(int fd, void *buf, size_t n, off_t off);
ssize_t nps5_segfile_pread(nps5_segfile *f, void *buf, size_t n, uint64_t off, nps5_segfile_reader rd);

// NPS5_VFS_SEGFILES_TRACE=<n>: the first n reads of segmented files print
// "[vfs] segread <segment dir> off=<logical offset> len=<bytes> fnv=<FNV-1a 64 of the bytes handed back>", so a run's
// reads can be checked byte for byte against the segments afterwards (tools-host/gamepack.py checktrace).
void nps5_segfile_trace(const nps5_segfile *f, uint64_t off, const void *buf, size_t n);

// Directory listings: for an entry <name> of the directory open as <dirfd> with type <dtype> (FreeBSD DT_*):
// 0 = list it unchanged; 1 = it is a segmented file: list it as <out> (the name without ".nps5seg"), type DT_REG;
// -1 = hide it (the original file is still next to it, a pack in progress).
int nps5_segfile_dirent(int dirfd, const char *name, unsigned char dtype, char *out, size_t out_sz);

// Closes every segment fd and forgets all handles and directory scans (tests; not while reads are in flight).
void nps5_segfile_reset(void);

#ifdef __cplusplus
}
#endif

#endif
