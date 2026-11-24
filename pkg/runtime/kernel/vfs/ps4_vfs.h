#ifndef PS4_VFS_H
#define PS4_VFS_H

#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>

#ifdef __cplusplus
extern "C" {
#endif

// Base threshold for virtual archive file descriptors
#define PS4_VFS_VIRTUAL_FD_BASE 10000

// Initialize VFS with host directory path corresponding to /app0
void ps4_vfs_init(const char *app_root);

// Returns the current app root path
const char *ps4_vfs_get_app_root(void);

// Register a custom mount point: guest_prefix (e.g. "/temp0" or "/addcont0") -> host_path
int ps4_vfs_mount(const char *guest_prefix, const char *host_path);
// Same, read-only: the write gate refuses every mutation below it (DLC, read-only save mounts, overlay files).
int ps4_vfs_mount_ro(const char *guest_prefix, const char *host_path);

// Unregister a mount point
int ps4_vfs_unmount(const char *guest_prefix);

// Resolves a guest path (e.g. /app0/assets/images/logo.png) to host filesystem path
// Returns 0 on success, or negative errno on error.
int ps4_vfs_resolve(const char *guest_path, char *host_path, size_t host_path_sz);

// Check if an fd is a virtual file descriptor managed by VFS (e.g. backed by PAK)
bool ps4_vfs_is_virtual_fd(int fd);
// True for a guest path in the read-only application image (/app0, /hostapp, a relative path): mutating calls fail with EROFS.
bool ps4_vfs_is_readonly(const char *guest_path);

// ---- THE write gate -------------------------------------------------------------------------------
// Every path-taking call that creates, changes or removes anything must go through these (a shim must never
// call ps4_vfs_resolve and then a host mutator: tools/kernelfs_unit.sh greps the shim sources for that).
// Failure is -1 with errno: EROFS for the read-only application image (/app0, /hostapp, relative paths, matched
// case-insensitively, and anything that resolves into the application directory through a host path, ".." or a
// symlink), ENODEV for a per-title root (/data, /temp0, /download0) whose host directory could not be created.
// Both ends of rename and link are checked. fd forms refuse fds that refer to the application image.
int ps4_vfs_resolve_for_write(const char *guest_path, char *host_path, size_t host_path_sz); // 0 or -errno
// Directory listing cache for case-insensitive lookups (NPS5_VFS_DIRCACHE=1, default off): test/diagnostic hooks.
void ps4_vfs_dircache_set(int on);
void ps4_vfs_dircache_stats(uint64_t *hits, uint64_t *scans);
int ps4_vfs_check_writable(const char *guest_path); // 0 or -errno
int ps4_vfs_fd_check_writable(int fd);              // 0 or -errno
int ps4_vfs_unlink(const char *g);
int ps4_vfs_mkdir(const char *g, mode_t mode);
int ps4_vfs_rmdir(const char *g);
int ps4_vfs_remove(const char *g);
int ps4_vfs_truncate(const char *g, off_t len);
int ps4_vfs_chmod(const char *g, mode_t mode);
int ps4_vfs_chown(const char *g, uid_t u, gid_t gr);
int ps4_vfs_chflags(const char *g, unsigned int flags);
int ps4_vfs_mkfifo(const char *g, mode_t mode);
int ps4_vfs_utimes(const char *g, const struct timeval tv[2]);
int ps4_vfs_setxattr(const char *g, const char *name, const void *value, size_t size, int flags);
int ps4_vfs_removexattr(const char *g, const char *name);
int ps4_vfs_rename(const char *from, const char *to);
int ps4_vfs_link(const char *existing, const char *newname);
int ps4_vfs_symlink(const char *target, const char *linkpath);
int ps4_vfs_fchmod(int fd, mode_t mode);
int ps4_vfs_fchown(int fd, uid_t u, gid_t gr);
int ps4_vfs_futimes(int fd, const struct timeval tv[2]);
int ps4_vfs_ftruncate(int fd, off_t len);
int ps4_vfs_fsetxattr(int fd, const char *name, const void *value, size_t size, int flags);
// fopen of a guest path: write modes (w, a, +, x) go through the gate, "r" modes through ps4_vfs_resolve.
FILE *ps4_vfs_fopen(const char *guest_path, const char *mode);

// Title id of the running application: $NPS5_TITLE_ID, else sce_sys/param.json "titleId" (PS5), else the
// SFO's TITLE_ID (PS4); "" when none is known.
const char *ps4_vfs_title_id(void);
// The writable, per-title host directory behind a guest root. kind is "data" (/data), "temp" (/temp0, emptied
// at start and at exit), "download0" (/download0) or "savedata" (/savedata*: <dir>/user_<id>/<name>/).
//   $NPS5_USERDATA_DIR/<kind>                                          (tests, relocation)
//   ~/Library/Application Support/nativeps5/<titleId>/<kind>           (default)
// Returns false when the PS4Recomp layout applies instead ($PS4RECOMP_DATA_DIR set without NPS5_USERDATA_DIR,
// for every kind but "data") or no location can be formed. The directory is created. Never inside the
// application image.
bool ps4_vfs_userdata_dir(const char *kind, char *out, size_t out_sz);

// Unified VFS operations handling both host filesystem and virtual archive files
int ps4_vfs_open(const char *guest_path, int host_flags, mode_t mode);
// Logs "[level-open] <name> t=<s>" once per levels/<name>/ directory seen in an open path; returns 1 when newly logged.
int ps4_vfs_note_level_path(const char *path);
int ps4_vfs_note_scene_path(const char *path);   // same, key from NPS5_SCENE_OPEN_KEY (default "levels/"); logs "[scene-open]" then the "[level-open]" alias
int ps4_vfs_scene_key_parse(const char *v, char *key, size_t cap, size_t *klen);  // 0 = off
void ps4_vfs_scene_reset(void);                  // re-read NPS5_SCENE_OPEN_KEY / NPS5_NO_LEVEL_LOG / NPS5_NO_LEVEL_ALIAS, forget logged names
int ps4_vfs_close(int fd);
ssize_t ps4_vfs_read(int fd, void *buf, size_t count);
// Load accounting (ps4_vfs.c): the exported counters nps5_vfs_bytes_read (_Atomic unsigned long long) and
// nps5_vfs_phase (_Atomic int) are always kept; NPS5_VFS_READLOG / NPS5_VFS_LOADPROFILE write files when set.
const char *nps5_vfs_current_scene(void); // last scene directory a file was opened in ("" = none; menus skipped)
void nps5_vfs_mark_first_picture(void);                        // presenter: the first picture is on screen
void ps4_vfs_readlog_note(int fd, long long off, long long n); // n bytes were read from fd at off (-1 = not known)
void ps4_vfs_readlog_note_path(const char *path, int fd, long long off,
                               long long n); // same for a private fd of a host path
ssize_t ps4_vfs_pread(int fd, void *buf, size_t count, off_t offset);
off_t ps4_vfs_lseek(int fd, off_t offset, int whence);
int ps4_vfs_fstat(int fd, struct stat *st);
int ps4_vfs_stat(const char *guest_path, struct stat *st);
int ps4_vfs_lstat(const char *guest_path, struct stat *st);
uint8_t ps4_vfs_resolve_dirent_type(int dirfd_, const char *name, uint8_t d_type, int *resolved);
int ps4_vfs_access(const char *guest_path, int mode);
void *ps4_vfs_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset);

// Cleanup VFS state
void ps4_vfs_destroy(void);

#ifdef __cplusplus
}
#endif

#endif // PS4_VFS_H
