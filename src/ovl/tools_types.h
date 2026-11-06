#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct ToolNode {
    uint64_t address = 0;
    uint64_t parent = 0;
    std::string name;
    std::string className;
    int depth = 0;
    int childCount = 0;
    bool expanded = false;
    bool childrenLoaded = false;
};

struct MacroEvent {
    DWORD atMs = 0;
    WORD vk = 0;
    bool down = false;
};

struct ToolState {
    uint64_t generation = 0;
    uint64_t place = 0;
    std::string jobId;
    uint64_t selectedInstance = 0;
    std::vector<ToolNode> nodes;
    std::string status;
    bool refreshPending = false;
    bool dumpRunning = false;
    int dumpNodes = 0;
    std::string dumpPath;
    bool macroRecording = false;
    bool macroPlaying = false;
    int macroEvents = 0;
    DWORD macroElapsed = 0;
};
