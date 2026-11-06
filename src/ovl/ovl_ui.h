#pragma once
// Helper UI dùng chung cho mọi tab. Định nghĩa nằm ở ovl_ui.cpp (S1.2).
#include "ovl_types.h"
#include "ovl_cfg.h"
#include <string>
#include <windows.h>

extern std::vector<HitRect> g_menuHits;
extern char g_toast[96];
extern DWORD g_toastTick;
extern int* g_capVk;
extern const char* g_capCfg;
extern DWORD g_capT0;
extern bool g_capDrained;
extern bool g_capPollReady;

void hitMark();
void markChildArea();
void toast(const char* fmt, ...);
int  parseKey(const std::string& raw);
std::string keyName(int vk);
void keyStateLabel(int vk, bool applying);
void keyBindWidget(const char* label, int* vk, const char* cfgKey, float w = 150.0f);
bool chkInt(const char* label, int* v);
bool chkInt(const char* label, bool* v);
bool sliderFloatCfg(const char* label, float* v, float lo, float hi,
                    const char* fmt, const char* cfgKey);
bool sliderIntCfg(const char* label, int* v, int lo, int hi,
                  const char* fmt, const char* cfgKey);
