// Host test of the [apediag] tracer logic (pkg/runtime/modules/libSceAnyPS5/nps5_apediag_logic.h) on synthetic object tables.
// Build/run: tools/apediag_unit.sh
#include "../../pkg/runtime/modules/libSceAnyPS5/nps5_apediag_logic.h"
#include <stdio.h>
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void put32(uint8_t *b, size_t off, uint32_t v) { memcpy(b + off, &v, 4); }

int main(void) {
    // FNV-1a 32 reference values (empty string, "a", "foobar")
    CHECK(nps5_ad_fnv("", 0) == 0x811c9dc5u, "fnv empty");
    CHECK(nps5_ad_fnv("a", 1) == 0xe40c292cu, "fnv a: %x", nps5_ad_fnv("a", 1));
    CHECK(nps5_ad_fnv("foobar", 6) == 0xbf9cf968u, "fnv foobar: %x", nps5_ad_fnv("foobar", 6));

    // id file parsing: comments, blanks, CRLF, hex with and without 0x, missing label
    Nps5AdId ids[8];
    const char *txt = "# comment\n\n0xdeadbeef counter_6\r\n00c0ffee  trg capture\n  1234 \nzz nothex\n";
    int n = nps5_ad_ids_parse(txt, ids, 8);
    CHECK(n == 3, "parsed %d", n);
    CHECK(ids[0].hash == 0xdeadbeefu && !strcmp(ids[0].label, "counter_6"), "id0 %x %s", ids[0].hash, ids[0].label);
    CHECK(ids[1].hash == 0x00c0ffeeu && !strcmp(ids[1].label, "trg capture"), "id1 %s", ids[1].label);
    CHECK(ids[2].hash == 0x1234 && !strcmp(ids[2].label, "id"), "id2 default label %s", ids[2].label);
    CHECK(nps5_ad_ids_parse("a x\nb y\nc z\n", ids, 2) == 2, "max respected");
    n = nps5_ad_ids_parse(txt, ids, 8);
    CHECK(nps5_ad_id_find(ids, n, 0x00c0ffeeu) == 1 && nps5_ad_id_find(ids, n, 7) == -1, "find");

    // scan: id hash stored at an aligned offset of a synthetic object; zero words and unaligned copies are ignored
    uint8_t obj[0x100];
    memset(obj, 0, sizeof obj);
    size_t off = 0;
    CHECK(nps5_ad_scan_ids(obj, sizeof obj, ids, n, &off) == -1, "empty object has no id");
    put32(obj, 0x41, 0xdeadbeefu); // unaligned: must not match
    CHECK(nps5_ad_scan_ids(obj, sizeof obj, ids, n, &off) == -1, "unaligned ignored");
    put32(obj, 0x60, 0x00c0ffeeu);
    CHECK(nps5_ad_scan_ids(obj, sizeof obj, ids, n, &off) == 1 && off == 0x60, "scan finds id at 0x60 (%zu)", off);

    // diff: first call primes, later calls list changes, timers are muted after the limit
    Nps5AdObj o;
    nps5_ad_obj_init(&o, 0x1000, 0x5d0, 0x20);
    uint8_t w[0x20] = {0};
    char out[256];
    CHECK(nps5_ad_diff(&o, w, out, sizeof out, 3) == 0 && !out[0], "prime lists nothing");
    CHECK(nps5_ad_diff(&o, w, out, sizeof out, 3) == 0, "no change, no output");
    put32(w, 0x10, 2);
    CHECK(nps5_ad_diff(&o, w, out, sizeof out, 3) == 1 && strstr(out, "+0x5e0:0->2"), "state change listed: %s", out);
    for (uint32_t v = 1; v <= 6; v++) { // a timer at +4
        put32(w, 4, v * 100);
        int l = nps5_ad_diff(&o, w, out, sizeof out, 3);
        if (v <= 3) CHECK(l == 1 && strstr(out, "+0x5d4"), "timer change %u listed: %s", v, out);
        else CHECK(l == 0, "timer muted after 3 changes (v=%u listed %d)", v, l);
    }
    put32(w, 0x10, 3);
    CHECK(nps5_ad_diff(&o, w, out, sizeof out, 3) == 1 && strstr(out, "+0x5e0:2->3"), "other offsets still reported: %s", out);
    // tiny output buffer never overflows
    char tiny[16];
    put32(w, 0x14, 9); put32(w, 0x18, 9);
    nps5_ad_diff(&o, w, tiny, sizeof tiny, 3);
    CHECK(strlen(tiny) < sizeof tiny, "tiny buffer terminated");
    // window clamp
    nps5_ad_obj_init(&o, 1, 0, 0x10000);
    CHECK(o.len == NPS5_AD_WIN_MAX, "window clamped");

    // counter rule: 6 of 9 bots in state 8 longer than 0.1 s fire a Count=6 counter; the sixth still inside 0.1 s does not
    uint32_t st[9] = {8, 8, 8, 8, 8, 8, 3, 0, 2};
    float tin[9] = {5, 5, 5, 5, 5, 0.05f, 0, 0, 0};
    uint8_t valid[9] = {1, 1, 1, 1, 1, 1, 1, 1, 1};
    CHECK(nps5_ad_ready_count(st, tin, valid, 9, 8, 0.1f) == 5, "5 ready while the 6th is fresh");
    tin[5] = 0.5f;
    CHECK(nps5_ad_ready_count(st, tin, valid, 9, 8, 0.1f) == 6, "6 ready");
    valid[0] = 0; // an unseen / destroyed bot is not counted
    CHECK(nps5_ad_ready_count(st, tin, valid, 9, 8, 0.1f) == 5, "invalid bot skipped");
    st[5] = 7; // the stuck case: the last ape's FSM never reaches 8
    valid[0] = 1;
    CHECK(nps5_ad_ready_count(st, tin, valid, 9, 8, 0.1f) == 5, "stuck 6th ape: 5 ready");

    // delay trigger state names
    CHECK(!strcmp(nps5_ad_dt_state_name(0), "Wait") && !strcmp(nps5_ad_dt_state_name(1), "EnterDelay") &&
              !strcmp(nps5_ad_dt_state_name(5), "Leave") && !strcmp(nps5_ad_dt_state_name(6), "?"), "state names");

    // line budget: untagged objects stop at their cap, tagged ones go further, the total always wins
    Nps5AdBudget b = {10, 0, 2, 5};
    Nps5AdObj u, t;
    nps5_ad_obj_init(&u, 1, 0, 8); nps5_ad_obj_init(&t, 2, 0, 8); t.id = 0;
    int gu = 0, gt = 0;
    for (int i = 0; i < 20; i++) gu += nps5_ad_budget_take(&b, &u);
    CHECK(gu == 2, "untagged cap: %d", gu);
    for (int i = 0; i < 20; i++) gt += nps5_ad_budget_take(&b, &t);
    CHECK(gt == 5, "tagged cap: %d", gt);
    nps5_ad_obj_init(&u, 3, 0, 8);
    int more = 0;
    b.used = 9;
    for (int i = 0; i < 5; i++) more += nps5_ad_budget_take(&b, &u);
    CHECK(more == 1, "total cap wins: %d", more);

    if (fails) { printf("%d failed\n", fails); return 1; }
    printf("apediag_unit: all passed\n");
    return 0;
}
