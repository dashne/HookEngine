#pragma once
#include <cstdint>
#include "HookRegistry.h"

// Handler signature: read-only view of x0..x7 at target entry.
// Modifying args is out of scope here (Feature 1.9). Modifying return
// value is out of scope (Feature 1.9). Conditional abort is out of scope
// (Feature 1.8). Chaining is instrumentation.
typedef void (*ChainHandlerFn)(const uint64_t* x0_x7);

extern "C" HookResult ChainAttach(void* target, ChainHandlerFn handler, void** outTrampoline);
extern "C" HookResult ChainDetach(void* target, ChainHandlerFn handler);
extern "C" int        ChainCount();                       // # of active chains
extern "C" int        ChainHandlerCount(void* target);    // # of handlers on target
extern "C" void       ChainResetAll();                    // tear down all chains (test helper)