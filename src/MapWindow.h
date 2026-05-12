#pragma once
#include <array>
#include <string>
#include "WvWAPI.h"
#include "TileCache.h"
#include "nexus/Nexus.h"
#include "imgui.h"

class MapWindow {
public:
    void Init(AddonAPI_t* api, const std::string& dataDir);
    void Shutdown();

    // Called every frame from AddonRender when enabled.
    // game_x/game_z are Mumble Link fAvatarPosition[0] and [2] (metres).
    void Render(const RealmReport::MatchData& md, bool inWvw, int mumbleMapId,
                float game_x, float game_z, bool& pinned, float pinnedOpacity);

    bool enabled = false;

private:
    void RenderTab(int idx, const RealmReport::WvWMap& wvwMap, bool inWvw,
                   bool isPlayerMap, float game_x, float game_z);
    void RenderTiles(ImDrawList* dl, ImVec2 winPos, ImVec2 winSize, int idx);
    void RenderObjectives(ImDrawList* dl, ImVec2 winPos, ImVec2 winSize,
                          int idx, const RealmReport::WvWMap& wvwMap);
    void RenderPlayerDot(ImDrawList* dl, ImVec2 winPos, ImVec2 winSize,
                         int idx, float game_x, float game_z);

    ImVec2 ContToScreen(int idx, ImVec2 winPos, ImVec2 winSize, float cx, float cy) const;

    static const char* k_TabNames[4];
    static const char* k_MapTypes[4];
    static const int   k_MapIds[4];

    struct TabState {
        float zoom   = 0.25f;
        float orig_x = 0.f;
        float orig_y = 0.f;
    };
    std::array<TabState, 4> m_tabs;
    ImVec2 m_lastDrag = {0.f, 0.f};
    int  m_activeTab  = 0;
    bool m_prefetched[4] = {false, false, false, false};

    AddonAPI_t* m_api = nullptr;
    TileCache   m_tiles;
};
