#pragma once

// Feature 3.4 Path A — Session 51c.
//
// Probes and hooks BpServiceManager::getService(const String16&) inside
// libbinder.so. Purpose: determine whether framework-cached service lookups
// reach the C++ BpServiceManager layer even though they bypass
// IPCThreadState::transact.
//
// v1 is counter-only. If the hook fires on cached lookups (verified by
// ServiceManagerTest.bpServiceManagerHookFiresOnCachedLookup), v2 adds the
// null/alias override logic at this point using the String16 layout observed
// in the v1 diagnostic dump.

extern "C" int  BpSmProbe();       // counts + logs candidate symbols
extern "C" int  BpSmInstall();     // 0 = hooked, -1 = not found / failed
extern "C" void BpSmUninstall();
extern "C" int  BpSmCount();       // cumulative hit count