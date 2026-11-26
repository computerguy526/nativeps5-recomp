// SPDX-License-Identifier: GPL-2.0-or-later
// UE5 shader library reader (host side of ABI 1.8 extract_ue5_shader_groups), free of the guest runtime: file access and the
// decoder are plugged in, so the unit test runs it on synthetic containers with a stub decoder. See nps5_ue5code.c.
#ifndef NPS5_UE5CORE_H
#define NPS5_UE5CORE_H

#include "../modules/libSceAnyPS5/kytyGpuApi.h"
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

// Every UE-version dependent knob is DATA in one table (nps5_ue5core.c, nps5_ue5_variants): the IoStore TOC versions, the
// library header version, the chunk types and the element sizes of the library arrays. A new UE 5.x is one row plus a
// fixture; a TOC or library version no row covers is refused with one log line and nothing is read.
typedef struct Nps5Ue5Variant {
    const char *name;
    uint32_t toc_min, toc_max; // IoStore TOC header versions (the byte at offset 16)
    uint32_t lib_min, lib_max; // library chunk header version (the u32 at offset 0)
    uint8_t chunk_library; // IoStore chunk type of the shader code library
    uint8_t chunk_group; // IoStore chunk type of a shader group
    uint32_t elem[7]; // element bytes of the library's arrays: map hash, shader hash, group chunk id, map entry,
    // shader entry, group entry, shader index (each array: u32 count + elements)
    uint8_t abi; // decoder convention (KytyGpuUe5Query.abi)
} Nps5Ue5Variant;
const Nps5Ue5Variant *nps5_ue5_variants(unsigned *count);
// Every row is validated once (the reader strides the library arrays by elem[]); a row that cannot hold what the reader reads
// (elem[2] != 12, elem[4] < 4, elem[5] < 16, elem[6] < 4, a size of 0 or over 256, bad ranges) is refused with a log line and
// never matches. A container is read only when its row's abi equals the query's abi.
// Tests only: replace the table (rows are used by pointer, keep them alive); NULL restores the built-in table.
void nps5_ue5_set_variants_for_test(const Nps5Ue5Variant *rows, unsigned n);

// Where the containers live. Paths are relative to the application root ("Game/Content/Paks/pakchunk0-ps5.utoc"), '/'
// separated; the same strings go to open().
typedef struct Nps5Ue5Io {
    void *user;
    // Calls cb(arg, path) for every IoStore table of contents (*.utoc) in a stable order; stops at nonzero.
    int (*list)(void *user, int (*cb)(void *arg, const char *path), void *arg);
    void *(*open)(void *user, const char *path); // NULL = no such file
    ssize_t (*pread)(void *user, void *file, void *buf, size_t n, uint64_t off); // pread(2) contract; thread safe
    void (*close)(void *user, void *file);
} Nps5Ue5Io;

// One decoding thread's state. decode() returns a pointer to raw_len decoded bytes (valid until the next call on this
// thread) or NULL on failure (decoder returned another size, or a failed CRC inside the decoder).
typedef struct Nps5Ue5Decoder {
    void *user;
    void *(*thread_open)(void *user, uint32_t index); // NULL result = this thread cannot decode
    const uint8_t *(*decode)(void *thread, const uint8_t *comp, uint32_t comp_len, uint32_t raw_len);
    void (*thread_close)(void *thread);
} Nps5Ue5Decoder;

// The runtime VFS as file access: containers under /app0 (the application root), plain or .nps5seg (nps5_ue5vfs.c).
const Nps5Ue5Io *nps5_ue5_vfs_io(void);

// Same contract as extract_ue5_shader_groups in the ABI. q->decoder is not looked at here; q->abi must equal the abi of the
// container's table row (other containers are not read).
int nps5_ue5_extract(const Nps5Ue5Io *io, const Nps5Ue5Decoder *dec, const KytyGpuUe5Query *q, KytyGpuUe5Result *out);

#endif
