#pragma once
// World module API. Definitions live in ovl_world.cpp.
#include "sdk.h"
#include <cstdint>

void worldRestoreAll(Memory& m, const Offsets& o);
void worldTick(Memory& m, SDK& sdk, const Offsets& o, bool haveWrite,
               DWORD nowLoop, uint64_t camAddr);
