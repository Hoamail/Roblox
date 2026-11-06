#pragma once
// Server module API. Definitions live in ovl_server.cpp.
#include "sdk.h"          // Memory, SDK, Offsets
#include "ovl_types.h"    // SrvState, SrvWire
#include <atomic>
#include <string>
#include <vector>

extern std::atomic<bool> g_srvRun;   // main sets false at exit, worker reads it

void srvWorkerThread();
void srvTick(Memory& m, SDK& sdk, const Offsets& o, DWORD now,
             bool fgGame, bool typingNow, bool aiming,
             const std::vector<std::string>& names, int entCount);
int  runSrvSelfTest(uint64_t place);
bool srvValidJob(const std::string& s);
