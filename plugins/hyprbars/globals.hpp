#pragma once

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/render/Texture.hpp>

inline HANDLE PHANDLE = nullptr;

struct SHyprButton {
    std::string  cmd     = "";
    bool         userfg  = false;
    CHyprColor   fgcol   = CHyprColor(0, 0, 0, 0);
    CHyprColor   bgcol   = CHyprColor(0, 0, 0, 0);
    float        size    = 10;
    std::string  icon    = "";
    SP<CTexture> iconTex = makeShared<CTexture>();
    SP<CTexture> iconTex2 = makeShared<CTexture>(); // SEKAI_MAXIMIZE2: 최대화한 창의 최대화 단추 = 복원 모양
};

class CHyprBar;

struct SGlobalState {
    std::vector<SHyprButton>  buttons;
    std::vector<WP<CHyprBar>> bars;
};

inline UP<SGlobalState> g_pGlobalState;
