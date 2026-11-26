// SPDX-License-Identifier: GPL-2.0-or-later
// What nps5_kyty.c needs of the UE5 shader library reader (core/nps5_ue5code.c), for the seam tests that do not link it:
// one synthetic group {1, 2, 3, 4} with one shader of stage 5, and a count of the calls.
#include "nps5_ue5code.h"

int g_ue5_stub_calls;

void nps5_ue5_set_process(GuestContext *process) { (void)process; }
int nps5_ue5_extract_groups(const KytyGpuUe5Query *q, KytyGpuUe5Result *out) {
    static const uint8_t bytes[4] = {1, 2, 3, 4};
    static const uint32_t freq[1] = {5};
    g_ue5_stub_calls++;
    if (!q || !out || !q->group) return -1;
    const KytyGpuUe5Group g = {
        .library = 0, .group = 0, .shader_count = 1, .raw_bytes = 4, .frequencies = freq, .bytes = bytes};
    out->groups_total = out->groups_decoded = 1;
    return q->group(q->arg, &g) ? -3 : 0;
}
