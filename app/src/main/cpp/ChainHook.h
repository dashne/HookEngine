#pragma once
#include <cstdint>
#include "HookRegistry.h"

// Handler signature: read-only view of x0..x7 at target entry.
// Modifying args is out of scope here (Feature 1.9). Modifying return
// value is out of scope (Feature 1.9). Chaining is instrumentation.
typedef void (*ChainHandlerFn)(const uint64_t* x0_x7);

// Feature 1.8 — predicate signature. Read-only view of x0..x7. Returns
// true to fire the associated handler; false to skip it (handler body is
// NOT invoked, entry callback is NOT fired). Predicates and handlers are
// 1:1 paired per chain slot. A nullptr predicate means "always fire" —
// identical to the pre-1.8 behavior.
//
// The dispatcher's orig call and trampoline fallthrough are unaffected —
// the predicate only gates whether the handler observes the call. Even
// when all handlers are skipped, the original function still executes
// and returns its real value.
typedef bool (*ChainPredicateFn)(const uint64_t* x0_x7);

extern "C" HookResult ChainAttach(void* target, ChainHandlerFn handler, void** outTrampoline);

// Feature 1.8 — attach with an optional predicate. predicate == nullptr
// is equivalent to ChainAttach.
extern "C" HookResult ChainAttachCond(void* target, ChainPredicateFn predicate,
                                      ChainHandlerFn handler, void** outTrampoline);

extern "C" HookResult ChainDetach(void* target, ChainHandlerFn handler);
extern "C" int        ChainCount();                       // # of active chains
extern "C" int        ChainHandlerCount(void* target);    // # of handlers on target
extern "C" void       ChainResetAll();                    // tear down all chains (test helper)