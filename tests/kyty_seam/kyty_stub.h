// SPDX-License-Identifier: GPL-2.0-or-later
// Test stub of kyty_gpu.dylib (kyty_stub.c): what it recorded, read by the test host through dlsym.
#ifndef KYTY_STUB_H
#define KYTY_STUB_H

#include <stdint.h>

#include "kytyGpuApi.h"

#define KYTY_STUB_MAX_CALLS 4096

typedef enum {
  STUB_INIT = 1, STUB_START, STUB_SHUTDOWN, STUB_REGISTER, STUB_UNREGISTER, STUB_PROTECT, STUB_FAULT, STUB_EXPORT, STUB_NID_ENTRY, STUB_SERVED,
} KytyStubKind;

typedef struct KytyStubCall {
  uint32_t kind;     // KytyStubKind
  int32_t entry;     // STUB_EXPORT: index of the entry (order of nid_entry lookups)
  char nid[16];      // STUB_EXPORT / STUB_NID_ENTRY
  uint64_t a[12];    // STUB_EXPORT: integer arguments; REGISTER: base,size,flags,alias; UNREGISTER/PROTECT: base,size,flags; FAULT: page,is_write,address as reported
  double f[2];       // STUB_EXPORT: first float arguments
} KytyStubCall;

typedef struct KytyStubState {
  uint32_t ncalls;
  KytyStubCall calls[KYTY_STUB_MAX_CALLS];
  const KytyGpuHostCallbacks *host; // as given to init
  uint64_t next_result;             // what the next export call returns (generic entries); 0 = 0x1000 + entry index
  int fault_disarms;                // handle_fault disarms the page through protect_guest (default 1)
  int serve_loads;                  // handle_fault of a load: complete it through fault_load/fault_load_apply with the bytes 0xA0, 0xA1, ...
                                    // (a stand-in for the page's published copy) and leave the page armed
  int in_main_thread;               // set by the function start_graphics ran through run_on_main_thread
} KytyStubState;

// Exported by the stub.
KytyStubState *KytyStubGetState(void);

#endif
