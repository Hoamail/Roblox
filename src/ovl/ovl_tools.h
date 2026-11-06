#pragma once
#include "sdk.h"
#include "tools_types.h"
#include <string>

void toolsTick(Memory& m, SDK& sdk, const Offsets& o, uint64_t place,
               const std::string& jobId, DWORD now);
ToolState toolsState();
void toolsRequestRefresh();
void toolsRequestChildren(uint64_t parent);
void toolsSelect(uint64_t address);
void toolsRequestDump(Memory& m, const Offsets& o);
void toolsCancelDump();
void toolsShutdown();
void toolsClear();
void toolsMacroTick(DWORD now, DWORD pid);
void toolsMacroStart();
void toolsMacroStop();
void toolsMacroPlay();
void toolsMacroCancel();
