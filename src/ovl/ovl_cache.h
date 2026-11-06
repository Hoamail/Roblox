#pragma once
// 3-tier entity cache (players + NPC/mob Models). Moved verbatim from
// overlay.cpp in S3.1; overlay owns `ents` and constructs one EntityCtx.
#include "ovl_types.h"
#include <cstddef>
#include <map>
#include <string>
#include <vector>

struct EntityCtx {
    EntityCtx(Memory& m_, SDK& sdk_, const Offsets& o_,
              std::vector<Ent>& ents_, uint64_t& uisAddr_,
              uint64_t& stickPlayer_)
      : m(m_), sdk(sdk_), o(o_), ents(ents_), uisAddr(uisAddr_),
        stickPlayer(stickPlayer_) {}

    Memory& m;
    SDK& sdk;
    const Offsets& o;
    std::vector<Ent>& ents;
    uint64_t& uisAddr;
    uint64_t& stickPlayer;

    std::map<uint64_t, Ent> ecache;
    std::map<uint64_t, std::string> classCache;
    uint64_t cDm = 0, cPlayers = 0, lastPlaceSeen = 0;
    size_t npcScanSkip = 0;

    void refreshEnts();

private:
    std::string classOf(uint64_t obj);
    void resolveParts(Ent& e, uint64_t model);
    void clearResolvedParts(Ent& e);
};
