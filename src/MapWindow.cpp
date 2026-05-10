#include "MapWindow.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>

static std::string FmtDuration(int s) {
    if (s < 0)    return "---";
    if (s < 60)   return std::to_string(s) + "s";
    if (s < 3600) {
        int m = s / 60, sec = s % 60;
        return sec ? std::to_string(m) + "m" + std::to_string(sec) + "s"
                   : std::to_string(m) + "m";
    }
    int h = s / 3600, m = (s % 3600) / 60;
    return m ? std::to_string(h) + "h" + std::to_string(m) + "m"
             : std::to_string(h) + "h";
}

static const std::unordered_map<std::string, std::string> k_ObjLabels = {
    {"38-9",    "SMC"},
    {"38-18",   "ANZ"},
    {"96-37",   "Garri"},
    {"95-37",   "Garri"},
    {"96-32",   "Hills"},
    {"95-32",   "Hills"},
    {"96-33",   "Bay"},
    {"95-33",   "Bay"},
    {"1099-106","Fire Keep"},
    {"95-39",   "Titanpaw"},
    {"96-39",   "Spiritholme"},
};

static std::string ObjectiveLabel(const RealmReport::Objective& obj) {
    auto it = k_ObjLabels.find(obj.id);
    if (it != k_ObjLabels.end()) return it->second;
    // First word of name
    auto pos = obj.name.find(' ');
    return pos == std::string::npos ? obj.name : obj.name.substr(0, pos);
}

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
            // Append " !" when the map's data looks stale.
            // Use the age of the most recently flipped objective — WvW always has activity,
            // so a freshest-flip older than 5 minutes means the API is serving stale data.
            bool mapStale = false;
            if (RealmReport::WvWAPI::HasMatchData()) {
                int mostRecent = INT_MAX;
                for (const auto& wm : md.maps) {
                    if (wm.type != k_MapTypes[i]) continue;
                    for (const auto& obj : wm.objectives) {
                        if (obj.type == RealmReport::ObjectiveType::Camp  ||
                            obj.type == RealmReport::ObjectiveType::Tower ||
                            obj.type == RealmReport::ObjectiveType::Keep  ||
                            obj.type == RealmReport::ObjectiveType::Castle) {
                            if (obj.seconds_since_flip >= 0 && obj.seconds_since_flip < mostRecent)
                                mostRecent = obj.seconds_since_flip;
                        }
                    }
                    break;
                }
                mapStale = (mostRecent >= 300);
            }
            if (mapStale)
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.90f, 0.25f, 0.25f, 1.f));
            bool tabOpen = ImGui::BeginTabItem(k_TabNames[i]);
            if (mapStale)
                ImGui::PopStyleColor();

            if (tabOpen) {
                m_activeTab = i;

                // Set initial camera position to map centre once bounds are available.
                // Tiles load on demand via GetTile in RenderTiles — no batch prefetch,
                // which would flood the 100ms-throttled download queue and block other tabs.
                if (!m_prefetched[i]) {
                    auto b = RealmReport::WvWAPI::GetMapBounds(k_MapTypes[i]);
                    if (b.cont_max_x > 0.f) {
                        m_tabs[i].orig_x = (b.cont_min_x + b.cont_max_x) * 0.5f;
                        m_tabs[i].orig_y = (b.cont_min_y + b.cont_max_y) * 0.5f;
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

static ImU32 TeamColour(RealmReport::TeamColor t) {
    switch (t) {
        case RealmReport::TeamColor::Red:     return IM_COL32(220, 50,  50,  255);
        case RealmReport::TeamColor::Green:   return IM_COL32(50,  190, 50,  255);
        case RealmReport::TeamColor::Blue:    return IM_COL32(50,  100, 220, 255);
        default:                              return IM_COL32(140, 140, 140, 255);
    }
}

static float ObjRadius(RealmReport::ObjectiveType t) {
    switch (t) {
        case RealmReport::ObjectiveType::Castle: return 14.f;
        case RealmReport::ObjectiveType::Keep:   return 11.f;
        case RealmReport::ObjectiveType::Tower:  return  8.f;
        default:                                 return  5.f;
    }
}

void MapWindow::RenderObjectives(ImDrawList* dl, ImVec2 winPos, ImVec2 winSize,
                                  int idx, const RealmReport::WvWMap& wvwMap) {
    float t = (float)ImGui::GetTime();

    for (const auto& obj : wvwMap.objectives) {
        if (obj.coord_x == 0.f && obj.coord_y == 0.f) continue;

        ImVec2 sp = ContToScreen(idx, winPos, winSize, obj.coord_x, obj.coord_y);

        // Cull objectives outside the window (plus a margin)
        if (sp.x < winPos.x - 20.f || sp.x > winPos.x + winSize.x + 20.f) continue;
        if (sp.y < winPos.y - 20.f || sp.y > winPos.y + winSize.y + 20.f) continue;

        float r     = ObjRadius(obj.type);
        ImU32 col   = TeamColour(obj.owner);
        ImU32 black = IM_COL32(0, 0, 0, 200);

        // Draw shape by type
        if (obj.type == RealmReport::ObjectiveType::Castle ||
            obj.type == RealmReport::ObjectiveType::Keep) {
            dl->AddNgonFilled(sp, r, col, 6);
            dl->AddNgon(sp, r, black, 6, 1.5f);
        } else if (obj.type == RealmReport::ObjectiveType::Tower) {
            ImVec2 p0{sp.x - r, sp.y};
            ImVec2 p1{sp.x,     sp.y - r};
            ImVec2 p2{sp.x + r, sp.y};
            ImVec2 p3{sp.x,     sp.y + r};
            dl->AddQuadFilled(p0, p1, p2, p3, col);
            dl->AddQuad(p0, p1, p2, p3, black, 1.5f);
        } else {
            dl->AddCircleFilled(sp, r, col);
            dl->AddCircle(sp, r, black, 0, 1.5f);
        }

        // --- Ring: recently flipped (0–300s) ---
        if (obj.seconds_since_flip >= 0 && obj.seconds_since_flip < 300) {
            float progress = (float)obj.seconds_since_flip / 300.f;
            float pulse    = 0.5f + 0.5f * sinf(t * 3.f);
            float alpha    = (1.f - progress) * (0.4f + 0.5f * pulse);
            ImU32 ringCol  = ImGui::ColorConvertFloat4ToU32(ImVec4(1.f, 0.85f, 0.2f, alpha));
            float ringR    = r + 5.f + 2.f * pulse;
            dl->AddCircle(sp, ringR, ringCol, 0, 3.f);
        }

        // --- Label below icon ---
        {
            std::string label = ObjectiveLabel(obj);
            ImVec2 tsz = ImGui::CalcTextSize(label.c_str());
            ImVec2 tp  = ImVec2(sp.x - tsz.x * 0.5f,
                                sp.y + r + 3.f);
            // Dark background for readability over tiles
            dl->AddRectFilled(
                ImVec2(tp.x - 2.f, tp.y - 1.f),
                ImVec2(tp.x + tsz.x + 2.f, tp.y + tsz.y + 1.f),
                IM_COL32(0, 0, 0, 160));
            dl->AddText(tp, IM_COL32(255, 255, 255, 230), label.c_str());
        }

        // Hover tooltip
        if (ImGui::IsMouseHoveringRect(
                ImVec2(sp.x - r - 2.f, sp.y - r - 2.f),
                ImVec2(sp.x + r + 2.f, sp.y + r + 2.f))) {
            ImGui::BeginTooltip();
            ImGui::Text("%s", obj.name.c_str());
            ImGui::Text("Owner: %s | Tier: %d",
                        RealmReport::TeamColorToString(obj.owner),
                        obj.upgrade_tier);
            if (!obj.claimed_by.empty())
                ImGui::Text("Claimed: %s", obj.claimed_by.c_str());
            if (obj.seconds_since_flip >= 0)
                ImGui::Text("Flipped: %s ago", FmtDuration(obj.seconds_since_flip).c_str());
            ImGui::EndTooltip();
        }
    }
}

void MapWindow::RenderPlayerDot(ImDrawList* dl, ImVec2 winPos, ImVec2 winSize,
                                 int idx, float game_x, float game_z) {
    // game_x/z are 0 when Mumble Link not active
    if (game_x == 0.f && game_z == 0.f) return;

    float cx, cy;
    RealmReport::WvWAPI::MumbleToContinent(k_MapTypes[idx], game_x, game_z, cx, cy);
    if (cx == 0.f && cy == 0.f) return;  // bounds not loaded yet

    ImVec2 sp = ContToScreen(idx, winPos, winSize, cx, cy);

    // Cull if off-screen
    if (sp.x < winPos.x || sp.x > winPos.x + winSize.x) return;
    if (sp.y < winPos.y || sp.y > winPos.y + winSize.y) return;

    // Bright yellow dot with black outline
    dl->AddCircleFilled(sp, 7.f, IM_COL32(255, 240, 60, 255));
    dl->AddCircle(sp, 7.f, IM_COL32(0, 0, 0, 200), 0, 2.f);
}
