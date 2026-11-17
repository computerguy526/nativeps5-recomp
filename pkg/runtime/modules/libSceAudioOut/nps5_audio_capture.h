#ifndef NPS5_AUDIO_CAPTURE_H
#define NPS5_AUDIO_CAPTURE_H

// Opt-in per-port PCM capture for the AudioOut HLE (verification tooling, no effect on playback).
//
// NPS5_AUDIO_CAPTURE=<dir> makes every AudioOut port (AudioOut2 ports are AudioOut ports underneath)
// write the exact stream the guest submitted, after format conversion, downmix and the port's volume
// (what is queued for the mixer), to
//   <dir>/port<handle>_type<type>.wav          float32 stereo 48 kHz (WAVE_FORMAT_IEEE_FLOAT)
//   <dir>/port<handle>_type<type>.grains.csv   one line per grain: index,first_frame,arrival_us
// arrival_us is the CLOCK_MONOTONIC microsecond time the guest's output call arrived. A background
// thread writes every 100 ms and rewrites the WAV header, so a run killed by timeout keeps its
// audio up to the last tick. Appending never does file I/O.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nps5_acap nps5_acap;

// Directory from NPS5_AUDIO_CAPTURE, or NULL when capture is off.
const char *nps5_acap_dir(void);

// Opens a capture for one port in `dir` (created if missing). Returns NULL on failure.
nps5_acap *nps5_acap_open(const char *dir, int handle, int type);

// Queues `frames` interleaved float stereo frames that arrived at `arrival_us`.
void nps5_acap_append(nps5_acap *c, const float *stereo, uint32_t frames, uint64_t arrival_us);

// Writes everything queued so far (all captures) and fixes the headers.
void nps5_acap_flush_all(void);

// Writes the rest, fixes the header and frees the capture.
void nps5_acap_close(nps5_acap *c);

#ifdef __cplusplus
}
#endif
#endif
