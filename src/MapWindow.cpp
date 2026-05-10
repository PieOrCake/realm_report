#include "MapWindow.h"
#include <algorithm>
#include <cmath>

const char* MapWindow::k_TabNames[4] = {"EBG", "Blue BL", "Green BL", "Red BL"};
const char* MapWindow::k_MapTypes[4] = {"Center", "BlueHome", "GreenHome", "RedHome"};
const int   MapWindow::k_MapIds  [4] = {38, 96, 95, 1099};

void MapWindow::Init(AddonAPI_t* api, const std::string& dataDir) {
    m_api = api;
    m_tiles.Init(api, dataDir);
    // Prefetch happens lazily on first render once map bounds are available
}

void MapWindow::Shutdown() {
    m_tiles.Shutdown();
}

ImVec2 MapWindow::ContToScreen(int idx, ImVec2 winPos, ImVec2 winSize, float cx, float cy) const {
    const TabState& ts = m_tabs[idx];
    return {
        winPos.x + winSize.x * 0.5f + (cx - ts.orig_x) * ts.zoom,
        winPos.y + winSize.y * 0.5f + (cy - ts.orig_y) * ts.zoom
    };
}

void MapWindow::Render(const RealmReport::MatchData& md, bool inWvw,
                       int mumbleMapId, float game_x, float game_z) {
    m_tiles.ProcessReadyQueue();

    ImGui::SetNextWindowSize(ImVec2(600, 500), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(300, 250), ImVec2(FLT_MAX, FLT_MAX));
    if (!ImGui::Begin("WvW Battlefield Map", &enabled,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::End();
        return;
    }

    if (ImGui::BeginTabBar("##wvwmaps")) {
        for (int i = 0; i < 4; ++i) {
            if (ImGui::BeginTabItem(k_TabNames[i])) {
                m_activeTab = i;

                // Lazy prefetch: try once bounds are loaded
                if (!m_prefetched[i]) {
                    auto b = RealmReport::WvWAPI::GetMapBounds(k_MapTypes[i]);
                    if (b.cont_max_x > 0.f) {
                        m_tabs[i].orig_x = (b.cont_min_x + b.cont_max_x) * 0.5f;
                        m_tabs[i].orig_y = (b.cont_min_y + b.cont_max_y) * 0.5f;
                        m_tiles.PrefetchRegion(b.cont_min_x, b.cont_min_y,
                                               b.cont_max_x, b.cont_max_y);
                        m_prefetched[i] = true;
                    }
                }

                const RealmReport::WvWMap* wmap = nullptr;
                for (auto& m : md.maps)
                    if (m.type == k_MapTypes[i]) { wmap = &m; break; }

                bool isPlayerMap = (mumbleMapId == k_MapIds[i]);
                if (wmap)
                    RenderTab(i, *wmap, inWvw, isPlayerMap, game_x, game_z);

                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
}

void MapWindow::RenderTab(int idx, const RealmReport::WvWMap& wvwMap, bool inWvw,
                           bool isPlayerMap, float game_x, float game_z) {
    ImVec2 winPos  = ImGui::GetCursorScreenPos();
    ImVec2 winSize = ImGui::GetContentRegionAvail();
    if (winSize.x <= 0 || winSize.y <= 0) return;

    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Capture mouse input over the map area
    ImGui::InvisibleButton("##maparea", winSize);

    TabState& ts = m_tabs[idx];

    // Scroll wheel to zoom (zoom toward cursor)
    if (ImGui::IsItemHovered()) {
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.f) {
            ImVec2 mp = ImGui::GetIO().MousePos;
            float  cx = ts.orig_x + (mp.x - winPos.x - winSize.x * 0.5f) / ts.zoom;
            float  cy = ts.orig_y + (mp.y - winPos.y - winSize.y * 0.5f) / ts.zoom;
            ts.zoom *= (wheel > 0.f ? 1.2f : 1.f / 1.2f);
            ts.zoom  = std::clamp(ts.zoom, 0.02f, 8.0f);
            ts.orig_x = cx - (mp.x - winPos.x - winSize.x * 0.5f) / ts.zoom;
            ts.orig_y = cy - (mp.y - winPos.y - winSize.y * 0.5f) / ts.zoom;
        }
    }

    // Left drag to pan (track delta manually to avoid ResetMouseDragDelta compat issues)
    {
        ImVec2 drag = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.f);
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.f)) {
            ImVec2 delta{drag.x - m_lastDrag.x, drag.y - m_lastDrag.y};
            ts.orig_x -= delta.x / ts.zoom;
            ts.orig_y -= delta.y / ts.zoom;
            m_lastDrag = drag;
        } else {
            m_lastDrag = {0, 0};
        }
    }

    ImVec2 clipMax = {winPos.x + winSize.x, winPos.y + winSize.y};
    dl->PushClipRect(winPos, clipMax, true);

    RenderTiles(dl, winPos, winSize, idx);
    RenderObjectives(dl, winPos, winSize, idx, wvwMap);
    if (isPlayerMap) RenderPlayerDot(dl, winPos, winSize, idx, game_x, game_z);

    // "Not in WvW" dark overlay
    if (!inWvw) {
        dl->AddRectFilled(winPos, clipMax, IM_COL32(0, 0, 0, 140));
        const char* msg = "Not currently in WvW";
        ImVec2 tsz = ImGui::CalcTextSize(msg);
        dl->AddText(
            ImVec2(winPos.x + (winSize.x - tsz.x) * 0.5f,
                   winPos.y + (winSize.y - tsz.y) * 0.5f),
            IM_COL32(200, 200, 200, 255), msg);
    }

    dl->PopClipRect();
}

void MapWindow::RenderTiles(ImDrawList* dl, ImVec2 winPos, ImVec2 winSize, int idx) {
    const TabState& ts = m_tabs[idx];
    if (ts.zoom <= 0.f) return;

    // Choose tile zoom level: pick the smallest tz where tiles appear >= 200px wide on screen
    int tz = 4;
    for (int z = 4; z <= 7; ++z) {
        float span = 32768.0f / (float)(1 << z);
        tz = z;
        if (span * ts.zoom >= 200.f) break;
    }
    float tile_span = 32768.0f / (float)(1 << tz);

    // Visible continent rectangle
    float half_w = winSize.x * 0.5f / ts.zoom;
    float half_h = winSize.y * 0.5f / ts.zoom;
    float c_minX = ts.orig_x - half_w;
    float c_maxX = ts.orig_x + half_w;
    float c_minY = ts.orig_y - half_h;
    float c_maxY = ts.orig_y + half_h;

    int tx_min = (int)floorf(c_minX / tile_span);
    int tx_max = (int)floorf(c_maxX / tile_span);
    int ty_min = (int)floorf(c_minY / tile_span);
    int ty_max = (int)floorf(c_maxY / tile_span);

    for (int tx = tx_min; tx <= tx_max; ++tx) {
        for (int ty = ty_min; ty <= ty_max; ++ty) {
            float sx = winPos.x + winSize.x * 0.5f + ((float)tx * tile_span - ts.orig_x) * ts.zoom;
            float sy = winPos.y + winSize.y * 0.5f + ((float)ty * tile_span - ts.orig_y) * ts.zoom;
            float ex = sx + tile_span * ts.zoom;
            float ey = sy + tile_span * ts.zoom;

            Texture_t* tex = m_tiles.GetTile(tz, tx, ty);
            if (tex && tex->Resource) {
                dl->AddImage((ImTextureID)tex->Resource, {sx, sy}, {ex, ey});
            } else {
                // Dark placeholder while tile loads
                dl->AddRectFilled({sx, sy}, {ex, ey}, IM_COL32(25, 28, 35, 255));
            }
        }
    }
}

void MapWindow::RenderObjectives(ImDrawList* /*dl*/, ImVec2 /*winPos*/,
                                  ImVec2 /*winSize*/, int /*idx*/,
                                  const RealmReport::WvWMap& /*wvwMap*/) {
    // Implemented in Task 5
}

void MapWindow::RenderPlayerDot(ImDrawList* /*dl*/, ImVec2 /*winPos*/,
                                 ImVec2 /*winSize*/, int /*idx*/,
                                 float /*game_x*/, float /*game_z*/) {
    // Implemented in Task 7
}
