// Host test of the AvPlayer frame selection (pkg/runtime/modules/libSceAvPlayer/avplayer_sync_logic.h). tools/avsync_unit.sh
#include "../../pkg/runtime/modules/libSceAvPlayer/avplayer_sync_logic.h"
#include <cstdio>
#include <vector>
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
int main() {
    { uint64_t t[] = {100, 133, 166}; CHECK(nps5_avsync_pick(t, 3, 50, 8) == -1); }     // not due: hold
    { uint64_t t[] = {100, 133, 166}; CHECK(nps5_avsync_pick(t, 3, 95, 8) == 0); }     // within tolerance
    { uint64_t t[] = {100, 133, 166}; CHECK(nps5_avsync_pick(t, 3, 100, 8) == 0); }    // one due: shown, none skipped
    { uint64_t t[] = {100, 133, 166}; CHECK(nps5_avsync_pick(t, 3, 160, 8) == 2); }    // 100/133/166 all due: skip to the last
    { uint64_t t[] = {100, 133, 166}; CHECK(nps5_avsync_pick(t, 3, 140, 8) == 1); }    // first is stale
    CHECK(nps5_avsync_pick(nullptr, 0, 1000, 8) == -1);
    // a looping clip: the clock restarts at 0 while frames of the old pass (late timestamps) are still queued. They must
    // not be held until the clock reaches their timestamps again (a freeze of one clip length per loop): a front frame
    // more than NPS5_AVSYNC_JUMP_MS ahead is a discontinuity and is handed out, one per poll.
    { uint64_t t[] = {9933, 9966, 0, 33}; CHECK(nps5_avsync_pick(t, 4, 5, 8) == 0); }
    { uint64_t t[] = {9966, 0, 33}; CHECK(nps5_avsync_pick(t, 3, 21, 8) == 0); }
    { uint64_t t[] = {0, 33}; CHECK(nps5_avsync_pick(t, 2, 38, 8) == 1); }               // then the normal rule again
    { uint64_t t[] = {5000}; CHECK(nps5_avsync_pick(t, 1, 0, 8) == 0); }                 // a stream that starts at an offset
    { uint64_t t[] = {900}; CHECK(nps5_avsync_pick(t, 1, 0, 8) == -1); }                 // an ordinary early frame is held
    { uint64_t t[] = {100}; CHECK(nps5_avsync_pick(t, 1, 50, ~0ull) == 0); }             // a huge tolerance does not wrap
    // 30 fps clip polled at 60 Hz with a 250 ms stall: old rule = fast forward (1 frame/poll), new rule = one jump, then 30 fps cadence
    std::vector<uint64_t> q; for (int i = 0; i < 30; ++i) q.push_back((uint64_t)(i * 33.333));
    uint64_t now = 0; int dropped = 0; std::vector<uint64_t> shown_at;
    for (int poll = 0; poll < 60; ++poll) {
        now = (uint64_t)(poll * 16.667); if (poll >= 10 && poll < 25) continue; // guest stalled ~250 ms
        int k = nps5_avsync_pick(q.data(), q.size(), now, 8);
        if (k >= 0) { dropped += k; shown_at.push_back(now); q.erase(q.begin(), q.begin() + k + 1); }
    }
    CHECK(dropped >= 5 && dropped <= 9); // the ~7 frames of the stall are skipped, not replayed
    for (size_t i = 1; i < shown_at.size(); ++i) { if (shown_at[i] <= 150 || shown_at[i - 1] >= 450) { uint64_t d = shown_at[i] - shown_at[i - 1]; CHECK(d >= 32 && d <= 34 + 17); } }
    Nps5AvRunStat s; s.add(33); s.add(34); s.add(32); CHECK(std::fabs(s.mean - 33) < 1e-9 && std::fabs(s.stddev() - 1) < 1e-9);
    printf(fails ? "avsync_unit FAILED (%d)\n" : "avsync_unit OK\n", fails);
    return fails ? 1 : 0;
}
