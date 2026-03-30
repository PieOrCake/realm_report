#include <windows.h>
#include <shellapi.h>
#include <mmsystem.h>
#include <string>
#include <vector>
#include <cstring>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <thread>

#include "nexus/Nexus.h"
#include "imgui.h"
#include "WvWAPI.h"

// Version constants
#define V_MAJOR 0
#define V_MINOR 9
#define V_BUILD 1
#define V_REVISION 1


// --- Mumble Link structures (minimal, for map type detection) ---

#pragma pack(push, 1)
struct GW2Context {
    unsigned char serverAddress[28];
    uint32_t mapId;
    uint32_t mapType;
    uint32_t shardId;
    uint32_t instance;
    uint32_t buildId;
    uint32_t uiState;
    uint16_t compassWidth;
    uint16_t compassHeight;
    float    compassRotation;
    float    playerX;
    float    playerY;
    float    mapCenterX;
    float    mapCenterY;
    float    mapScale;
    uint32_t processId;
    uint8_t  mountIndex;
};
#pragma pack(pop)

struct MumbleLinkedMem {
    uint32_t uiVersion;
    uint32_t uiTick;
    float    fAvatarPosition[3];
    float    fAvatarFront[3];
    float    fAvatarTop[3];
    wchar_t  name[256];
    float    fCameraPosition[3];
    float    fCameraFront[3];
    float    fCameraTop[3];
    wchar_t  identity[256];
    uint32_t context_len;
    unsigned char context[256];
    wchar_t  description[2048];
};

// WvW map types from GW2 Mumble context
static bool IsWvWMapType(uint32_t mapType) {
    switch (mapType) {
        case 9:  // Eternal Battlegrounds
        case 10: // Blue Borderlands
        case 11: // Green Borderlands
        case 12: // Red Borderlands
        case 15: // WvW Lounge (Armistice Bastion)
            return true;
        default:
            return false;
    }
}

// Global variables
HMODULE hSelf;
AddonDefinition_t AddonDef{};
AddonAPI_t* APIDefs = nullptr;
bool g_WindowVisible = false;
static bool g_IsOnWvWMap = false;

// UI State
static int g_WorldComboIndex = -1;
static char g_WorldFilter[128] = "";
static bool g_ShowSpawns = false;
static bool g_ShowRuins = false;

// Map filter: which maps to show
static bool g_FilterEBG = true;
static bool g_FilterRedBL = true;
static bool g_FilterBlueBL = true;
static bool g_FilterGreenBL = true;

// Type filter
static bool g_FilterCamps = true;
static bool g_FilterTowers = true;
static bool g_FilterKeeps = true;
static bool g_FilterCastles = true;

// Sort state
static RealmReport::SortColumn g_SortColumn = RealmReport::SortColumn::TimeSinceFlip;
static bool g_SortAscending = true;

// Display mode
static bool g_FlatList = false;

// --- Toast Notification System ---

static bool g_FlipNotifications = true;
static float g_ToastDuration = 5.0f;         // seconds visible (configurable)
static const float TOAST_FADE_TIME = 1.5f;  // seconds to fade out

// Toast anchor & size (bottom-right of anchor is where newest toast appears)
static float g_ToastAnchorX = -1.0f; // -1 = default (screen right edge)
static float g_ToastAnchorY = -1.0f; // -1 = default (screen bottom edge)
static float g_ToastW = 230.0f;
static float g_ToastH = 48.0f;
static bool  g_ToastEditMode = false;
static bool  g_ToastResetPos = false;
static bool  g_Paused = false;
static bool  g_WindowPinned = false;
static float g_PinnedOpacity = 0.6f;
static bool  g_FlipSound = false;
static std::string g_FlipSoundFile;  // just the filename, resolved against sounds/ dir

struct Toast {
    std::string obj_name;
    RealmReport::ObjectiveType type;
    std::string map_display;
    RealmReport::TeamColor old_owner;
    RealmReport::TeamColor new_owner;
    std::chrono::steady_clock::time_point spawn_time;
};

static std::vector<Toast> g_Toasts;

static bool PassesMapFilterByType(const std::string& map_type) {
    if (map_type == "Center")    return g_FilterEBG;
    if (map_type == "RedHome")   return g_FilterRedBL;
    if (map_type == "BlueHome")  return g_FilterBlueBL;
    if (map_type == "GreenHome") return g_FilterGreenBL;
    return true;
}

// --- Visual Helpers ---

static ImVec4 GetTeamColor(RealmReport::TeamColor color) {
    switch (color) {
        case RealmReport::TeamColor::Red:     return ImVec4(0.90f, 0.25f, 0.25f, 1.0f);
        case RealmReport::TeamColor::Green:   return ImVec4(0.25f, 0.80f, 0.25f, 1.0f);
        case RealmReport::TeamColor::Blue:    return ImVec4(0.35f, 0.55f, 0.95f, 1.0f);
        case RealmReport::TeamColor::Neutral: return ImVec4(0.50f, 0.50f, 0.50f, 1.0f);
        default:                              return ImVec4(0.70f, 0.70f, 0.70f, 1.0f);
    }
}

static ImU32 GetTeamColorU32(RealmReport::TeamColor color) {
    ImVec4 c = GetTeamColor(color);
    return ImGui::ColorConvertFloat4ToU32(c);
}

// Dimmer team colors for row backgrounds
static ImU32 GetTeamBgU32(RealmReport::TeamColor color, float alpha = 0.08f) {
    ImVec4 c = GetTeamColor(color);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, alpha));
}

// Type badge colors and labels
static ImVec4 GetTypeBadgeColor(RealmReport::ObjectiveType type) {
    switch (type) {
        case RealmReport::ObjectiveType::Camp:   return ImVec4(0.55f, 0.45f, 0.30f, 1.0f);
        case RealmReport::ObjectiveType::Tower:  return ImVec4(0.40f, 0.55f, 0.70f, 1.0f);
        case RealmReport::ObjectiveType::Keep:   return ImVec4(0.60f, 0.35f, 0.60f, 1.0f);
        case RealmReport::ObjectiveType::Castle: return ImVec4(0.75f, 0.55f, 0.20f, 1.0f);
        default:                                 return ImVec4(0.40f, 0.40f, 0.40f, 1.0f);
    }
}

static const char* GetTypeBadgeLabel(RealmReport::ObjectiveType type) {
    switch (type) {
        case RealmReport::ObjectiveType::Camp:   return "C";
        case RealmReport::ObjectiveType::Tower:  return "T";
        case RealmReport::ObjectiveType::Keep:   return "K";
        case RealmReport::ObjectiveType::Castle: return "S";
        case RealmReport::ObjectiveType::Ruins:  return "R";
        default:                                 return "?";
    }
}

static const char* MapShortName(const std::string& type) {
    if (type == "Center")    return "EBG";
    if (type == "RedHome")   return "Red BL";
    if (type == "BlueHome")  return "Blue BL";
    if (type == "GreenHome") return "Green BL";
    return "???";
}

static ImVec4 GetMapHeaderColor(const std::string& type) {
    if (type == "Center")    return ImVec4(0.85f, 0.75f, 0.40f, 1.0f);
    if (type == "RedHome")   return ImVec4(0.90f, 0.30f, 0.30f, 1.0f);
    if (type == "BlueHome")  return ImVec4(0.40f, 0.55f, 0.95f, 1.0f);
    if (type == "GreenHome") return ImVec4(0.30f, 0.80f, 0.30f, 1.0f);
    return ImVec4(0.70f, 0.70f, 0.70f, 1.0f);
}

static std::string FormatDuration(int seconds) {
    if (seconds < 0) return "---";
    if (seconds < 60) {
        return std::to_string(seconds) + "s";
    }
    if (seconds < 3600) {
        int m = seconds / 60;
        int s = seconds % 60;
        if (s == 0) return std::to_string(m) + "m";
        return std::to_string(m) + "m" + std::to_string(s) + "s";
    }
    int h = seconds / 3600;
    int m = (seconds % 3600) / 60;
    if (m == 0) return std::to_string(h) + "h";
    return std::to_string(h) + "h" + std::to_string(m) + "m";
}

static ImVec4 GetFlipTimeColor(int seconds) {
    if (seconds < 0)   return ImVec4(0.40f, 0.40f, 0.40f, 1.0f);
    if (seconds < 120)  return ImVec4(1.00f, 0.35f, 0.35f, 1.0f); // just flipped - red alert
    if (seconds < 300)  return ImVec4(1.00f, 0.75f, 0.20f, 1.0f); // recent - yellow
    if (seconds < 900)  return ImVec4(0.80f, 0.80f, 0.80f, 1.0f); // moderate
    return ImVec4(0.55f, 0.55f, 0.55f, 1.0f);                     // old - dim
}

// Draw a colored circle (owner indicator)
static void DrawOwnerDot(ImDrawList* dl, ImVec2 pos, float radius, RealmReport::TeamColor color) {
    ImU32 col = GetTeamColorU32(color);
    dl->AddCircleFilled(pos, radius, col);
    dl->AddCircle(pos, radius, ImGui::ColorConvertFloat4ToU32(ImVec4(0, 0, 0, 0.4f)), 0, 1.0f);
}

// Draw a compact type badge [C] [T] [K] [S]
static void DrawTypeBadge(ImDrawList* dl, ImVec2 pos, RealmReport::ObjectiveType type) {
    ImVec4 bg = GetTypeBadgeColor(type);
    const char* label = GetTypeBadgeLabel(type);
    float fontSize = ImGui::GetFontSize();
    float badgeH = fontSize;
    float badgeW = fontSize;
    ImVec2 topLeft(pos.x, pos.y);
    ImVec2 botRight(pos.x + badgeW, pos.y + badgeH);

    dl->AddRectFilled(topLeft, botRight, ImGui::ColorConvertFloat4ToU32(bg), 3.0f);
    dl->AddRect(topLeft, botRight, ImGui::ColorConvertFloat4ToU32(ImVec4(0, 0, 0, 0.3f)), 3.0f, 0, 1.0f);

    // Center text in badge
    ImVec2 textSize = ImGui::CalcTextSize(label);
    ImVec2 textPos(pos.x + (badgeW - textSize.x) * 0.5f, pos.y + (badgeH - textSize.y) * 0.5f);
    dl->AddText(textPos, IM_COL32(255, 255, 255, 230), label);
}

// Tier display with colored pips
static void DrawTierPips(int tier, int yaks) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float radius = 3.5f;
    float spacing = 10.0f;
    float yOffset = ImGui::GetFontSize() * 0.5f;

    ImU32 colOff = ImGui::ColorConvertFloat4ToU32(ImVec4(0.25f, 0.25f, 0.25f, 0.6f));
    ImU32 colT1 = ImGui::ColorConvertFloat4ToU32(ImVec4(0.70f, 0.70f, 0.70f, 1.0f));
    ImU32 colT2 = ImGui::ColorConvertFloat4ToU32(ImVec4(0.95f, 0.85f, 0.20f, 1.0f));
    ImU32 colT3 = ImGui::ColorConvertFloat4ToU32(ImVec4(1.00f, 0.55f, 0.10f, 1.0f));

    ImU32 colors[3] = { colT1, colT2, colT3 };

    for (int i = 0; i < 3; i++) {
        ImVec2 center(pos.x + radius + i * spacing, pos.y + yOffset);
        if (i < tier) {
            dl->AddCircleFilled(center, radius, colors[i]);
            dl->AddCircle(center, radius, ImGui::ColorConvertFloat4ToU32(ImVec4(0, 0, 0, 0.3f)), 0, 1.0f);
        } else {
            dl->AddCircle(center, radius, colOff, 0, 1.5f);
        }
    }

    // Reserve space
    ImGui::Dummy(ImVec2(3 * spacing, ImGui::GetFontSize()));

    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::Text("Tier %d  (%d yaks)", tier, yaks);
        ImGui::EndTooltip();
    }
}

static bool PassesMapFilter(const std::string& map_type) {
    if (map_type == "Center")    return g_FilterEBG;
    if (map_type == "RedHome")   return g_FilterRedBL;
    if (map_type == "BlueHome")  return g_FilterBlueBL;
    if (map_type == "GreenHome") return g_FilterGreenBL;
    return true;
}

static bool PassesTypeFilter(RealmReport::ObjectiveType type) {
    switch (type) {
        case RealmReport::ObjectiveType::Camp:   return g_FilterCamps;
        case RealmReport::ObjectiveType::Tower:  return g_FilterTowers;
        case RealmReport::ObjectiveType::Keep:   return g_FilterKeeps;
        case RealmReport::ObjectiveType::Castle: return g_FilterCastles;
        case RealmReport::ObjectiveType::Spawn:  return g_ShowSpawns;
        case RealmReport::ObjectiveType::Ruins:  return g_ShowRuins;
        default:                                 return false;
    }
}

// DLL entry point
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH: hSelf = hModule; break;
    case DLL_PROCESS_DETACH: break;
    case DLL_THREAD_ATTACH: break;
    case DLL_THREAD_DETACH: break;
    }
    return TRUE;
}

// Forward declarations
void AddonLoad(AddonAPI_t* aApi);
void AddonUnload();
void ProcessKeybind(const char* aIdentifier, bool aIsRelease);
void AddonRender();
void AddonOptions();

// --- World Selection Combo ---

static void RenderWorldSelector() {
    const auto& worlds = RealmReport::WvWAPI::GetWorlds();
    int selected = RealmReport::WvWAPI::GetSelectedWorld();

    if (!RealmReport::WvWAPI::IsWorldListReady()) {
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.0f, 1.0f), "Loading worlds...");
        return;
    }

    if (worlds.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "No worlds loaded");
        return;
    }

    // Find current selection name for preview
    std::string preview = "Select a world...";
    for (size_t i = 0; i < worlds.size(); i++) {
        if (worlds[i].id == selected) {
            preview = worlds[i].name;
            g_WorldComboIndex = (int)i;
            break;
        }
    }

    ImGui::Text("World:");
    ImGui::SameLine();
    ImGui::PushItemWidth(250);
    if (ImGui::BeginCombo("##world_select", preview.c_str())) {
        // Filter input at top of dropdown
        ImGui::PushItemWidth(-1);
        ImGui::InputTextWithHint("##world_filter", "Filter...", g_WorldFilter, sizeof(g_WorldFilter));
        ImGui::PopItemWidth();
        ImGui::Separator();

        std::string filter_lower(g_WorldFilter);
        std::transform(filter_lower.begin(), filter_lower.end(), filter_lower.begin(), ::tolower);

        for (size_t i = 0; i < worlds.size(); i++) {
            // Apply filter
            if (!filter_lower.empty()) {
                std::string name_lower = worlds[i].name;
                std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);
                if (name_lower.find(filter_lower) == std::string::npos) continue;
            }

            bool is_selected = (worlds[i].id == selected);
            if (ImGui::Selectable(worlds[i].name.c_str(), is_selected)) {
                RealmReport::WvWAPI::SetSelectedWorld(worlds[i].id);
                RealmReport::WvWAPI::SaveSelectedWorld();
                g_WorldComboIndex = (int)i;
                g_WorldFilter[0] = '\0';

                // Start or trigger immediate fetch
                if (!RealmReport::WvWAPI::IsPolling()) {
                    RealmReport::WvWAPI::StartPolling();
                } else {
                    RealmReport::WvWAPI::FetchNow();
                }
            }
            if (is_selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::PopItemWidth();
}

// --- Scoreboard ---

static void RenderScoreboard(const RealmReport::MatchData& match) {
    if (match.id.empty()) return;

    int total = match.score_red + match.score_green + match.score_blue;
    if (total <= 0) total = 1;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float barH = 6.0f;
    float barW = ImGui::GetContentRegionAvail().x;
    float rW = barW * ((float)match.score_red / total);
    float bW = barW * ((float)match.score_blue / total);
    float gW = barW - rW - bW;

    // Score bar
    ImU32 cRed = GetTeamColorU32(RealmReport::TeamColor::Red);
    ImU32 cBlue = GetTeamColorU32(RealmReport::TeamColor::Blue);
    ImU32 cGreen = GetTeamColorU32(RealmReport::TeamColor::Green);

    dl->AddRectFilled(ImVec2(pos.x, pos.y), ImVec2(pos.x + rW, pos.y + barH), cRed, 2.0f);
    dl->AddRectFilled(ImVec2(pos.x + rW, pos.y), ImVec2(pos.x + rW + bW, pos.y + barH), cBlue);
    dl->AddRectFilled(ImVec2(pos.x + rW + bW, pos.y), ImVec2(pos.x + barW, pos.y + barH), cGreen, 2.0f);
    ImGui::Dummy(ImVec2(barW, barH + 2));

    // Score numbers in a row
    ImGui::TextColored(GetTeamColor(RealmReport::TeamColor::Red), "%s %d",
        match.world_red.c_str(), match.score_red);
    ImGui::SameLine(0, 15);
    ImGui::TextColored(GetTeamColor(RealmReport::TeamColor::Blue), "%s %d",
        match.world_blue.c_str(), match.score_blue);
    ImGui::SameLine(0, 15);
    ImGui::TextColored(GetTeamColor(RealmReport::TeamColor::Green), "%s %d",
        match.world_green.c_str(), match.score_green);

}

// --- Filter Bar (compact) ---

static void RenderFilterBar() {
    // Type filter badges — draw as colored toggleable badges
    auto TypeToggle = [](const char* label, bool* val, ImVec4 color) {
        if (*val) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(color.x, color.y, color.z, 0.6f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(color.x, color.y, color.z, 0.8f));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.2f, 0.2f, 0.5f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.3f, 0.3f, 0.6f));
        }
        if (ImGui::SmallButton(label)) *val = !*val;
        ImGui::PopStyleColor(2);
    };

    TypeToggle("Camps", &g_FilterCamps, GetTypeBadgeColor(RealmReport::ObjectiveType::Camp));
    ImGui::SameLine(0, 2);
    TypeToggle("Towers", &g_FilterTowers, GetTypeBadgeColor(RealmReport::ObjectiveType::Tower));
    ImGui::SameLine(0, 2);
    TypeToggle("Keeps", &g_FilterKeeps, GetTypeBadgeColor(RealmReport::ObjectiveType::Keep));
    ImGui::SameLine(0, 2);
    TypeToggle("Castle", &g_FilterCastles, GetTypeBadgeColor(RealmReport::ObjectiveType::Castle));
}

// --- Objectives grouped by map ---

static void SortObjectives(std::vector<RealmReport::Objective>& objs) {
    std::sort(objs.begin(), objs.end(), [](const RealmReport::Objective& a, const RealmReport::Objective& b) -> bool {
        int cmp = 0;
        switch (g_SortColumn) {
            case RealmReport::SortColumn::Name:
                cmp = a.name.compare(b.name);
                break;
            case RealmReport::SortColumn::Type: {
                int ta = (int)a.type, tb = (int)b.type;
                cmp = (ta < tb) ? -1 : (ta > tb) ? 1 : 0;
                break;
            }
            case RealmReport::SortColumn::Owner: {
                int oa = (int)a.owner, ob = (int)b.owner;
                cmp = (oa < ob) ? -1 : (oa > ob) ? 1 : 0;
                break;
            }
            case RealmReport::SortColumn::TimeSinceFlip:
                cmp = (a.seconds_since_flip < b.seconds_since_flip) ? -1 :
                      (a.seconds_since_flip > b.seconds_since_flip) ? 1 : 0;
                break;
            case RealmReport::SortColumn::YaksDelivered:
                cmp = (a.yaks_delivered < b.yaks_delivered) ? -1 :
                      (a.yaks_delivered > b.yaks_delivered) ? 1 : 0;
                break;
            case RealmReport::SortColumn::ClaimedBy:
                cmp = a.claimed_by.compare(b.claimed_by);
                break;
            case RealmReport::SortColumn::PPT:
                cmp = (a.points_tick < b.points_tick) ? -1 :
                      (a.points_tick > b.points_tick) ? 1 : 0;
                break;
            default:
                cmp = (a.seconds_since_flip < b.seconds_since_flip) ? -1 :
                      (a.seconds_since_flip > b.seconds_since_flip) ? 1 : 0;
                break;
        }
        return g_SortAscending ? (cmp < 0) : (cmp > 0);
    });
}

static void RenderObjectiveRow(ImDrawList* dl, const RealmReport::Objective& obj, int idx) {
    ImGui::PushID(idx);
    ImGui::TableNextRow();

    // Subtle row tint for recently flipped
    if (obj.seconds_since_flip >= 0 && obj.seconds_since_flip < 120) {
        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1,
            ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 0.3f, 0.2f, 0.10f)));
    } else if (obj.seconds_since_flip >= 0 && obj.seconds_since_flip < 300) {
        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1,
            ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 0.8f, 0.2f, 0.06f)));
    }

    // --- Column 1: [dot] [badge] Name ---
    ImGui::TableNextColumn();
    {
        ImVec2 cursorPos = ImGui::GetCursorScreenPos();
        float fontSize = ImGui::GetFontSize();
        float dotR = 4.5f;
        float dotCY = cursorPos.y + fontSize * 0.5f;

        // Owner dot
        DrawOwnerDot(dl, ImVec2(cursorPos.x + dotR + 1, dotCY), dotR, obj.owner);

        // Type badge
        float badgeX = cursorPos.x + dotR * 2 + 6;
        DrawTypeBadge(dl, ImVec2(badgeX, cursorPos.y), obj.type);

        // Name text
        float nameX = badgeX + fontSize + 4;
        ImGui::SetCursorScreenPos(ImVec2(nameX, cursorPos.y));
        ImGui::Text("%s", obj.name.c_str());
    }

    // Tooltip on name hover — full details
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextColored(GetTeamColor(obj.owner), "%s", RealmReport::TeamColorToString(obj.owner));
        ImGui::SameLine();
        ImGui::Text(" %s  -  %s", RealmReport::ObjectiveTypeToString(obj.type), obj.name.c_str());
        if (obj.points_tick > 0) ImGui::Text("PPT: +%d", obj.points_tick);
        if (obj.points_capture > 0) ImGui::Text("Capture: +%d", obj.points_capture);
        if (obj.yaks_delivered > 0) ImGui::Text("Yaks: %d  (Tier %d)", obj.yaks_delivered, obj.upgrade_tier);
        if (!obj.claimed_by.empty()) ImGui::Text("Claimed: %s", obj.claimed_by.c_str());
        if (obj.seconds_since_flip >= 0) ImGui::Text("Flipped: %s ago", FormatDuration(obj.seconds_since_flip).c_str());
        ImGui::EndTooltip();
    }

    // --- Column 2: Flipped ---
    ImGui::TableNextColumn();
    ImGui::TextColored(GetFlipTimeColor(obj.seconds_since_flip), "%s",
        FormatDuration(obj.seconds_since_flip).c_str());

    // --- Column 3: Tier ---
    ImGui::TableNextColumn();
    if (obj.type == RealmReport::ObjectiveType::Camp ||
        obj.type == RealmReport::ObjectiveType::Tower ||
        obj.type == RealmReport::ObjectiveType::Keep ||
        obj.type == RealmReport::ObjectiveType::Castle) {
        DrawTierPips(obj.upgrade_tier, obj.yaks_delivered);
    } else {
        ImGui::TextColored(ImVec4(0.3f, 0.3f, 0.3f, 1.0f), "-");
    }

    // --- Column 4: Claimed (tag only, full name in tooltip) ---
    ImGui::TableNextColumn();
    if (!obj.claimed_by.empty()) {
        // Extract tag from "[TAG] Name" format
        std::string display = obj.claimed_by;
        size_t open = obj.claimed_by.find('[');
        size_t close = obj.claimed_by.find(']');
        if (open != std::string::npos && close != std::string::npos && close > open) {
            display = obj.claimed_by.substr(open, close - open + 1);
        }
        ImGui::TextColored(ImVec4(0.75f, 0.75f, 0.75f, 1.0f), "%s", display.c_str());
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::Text("%s", obj.claimed_by.c_str());
            ImGui::EndTooltip();
        }
    } else {
        ImGui::TextColored(ImVec4(0.30f, 0.30f, 0.30f, 1.0f), "-");
    }

    ImGui::PopID();
}

static void RenderObjectiveRowFlat(ImDrawList* dl, const RealmReport::Objective& obj, const std::string& map_type, int idx) {
    ImGui::PushID(idx);
    ImGui::TableNextRow();

    if (obj.seconds_since_flip >= 0 && obj.seconds_since_flip < 120) {
        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1,
            ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 0.3f, 0.2f, 0.10f)));
    } else if (obj.seconds_since_flip >= 0 && obj.seconds_since_flip < 300) {
        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1,
            ImGui::ColorConvertFloat4ToU32(ImVec4(1.0f, 0.8f, 0.2f, 0.06f)));
    }

    // Map column
    ImGui::TableNextColumn();
    ImVec4 mapCol = GetMapHeaderColor(map_type);
    ImGui::TextColored(mapCol, "%s", MapShortName(map_type));

    // Objective column (same as RenderObjectiveRow column 1)
    ImGui::TableNextColumn();
    {
        ImVec2 cursorPos = ImGui::GetCursorScreenPos();
        float fontSize = ImGui::GetFontSize();
        float dotR = 4.5f;
        float dotCY = cursorPos.y + fontSize * 0.5f;
        DrawOwnerDot(dl, ImVec2(cursorPos.x + dotR + 1, dotCY), dotR, obj.owner);
        float badgeX = cursorPos.x + dotR * 2 + 6;
        DrawTypeBadge(dl, ImVec2(badgeX, cursorPos.y), obj.type);
        float nameX = badgeX + fontSize + 4;
        ImGui::SetCursorScreenPos(ImVec2(nameX, cursorPos.y));
        ImGui::Text("%s", obj.name.c_str());
    }
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextColored(GetTeamColor(obj.owner), "%s", RealmReport::TeamColorToString(obj.owner));
        ImGui::SameLine();
        ImGui::Text(" %s  -  %s", RealmReport::ObjectiveTypeToString(obj.type), obj.name.c_str());
        if (obj.points_tick > 0) ImGui::Text("PPT: +%d", obj.points_tick);
        if (obj.points_capture > 0) ImGui::Text("Capture: +%d", obj.points_capture);
        if (obj.yaks_delivered > 0) ImGui::Text("Yaks: %d  (Tier %d)", obj.yaks_delivered, obj.upgrade_tier);
        if (!obj.claimed_by.empty()) ImGui::Text("Claimed: %s", obj.claimed_by.c_str());
        if (obj.seconds_since_flip >= 0) ImGui::Text("Flipped: %s ago", FormatDuration(obj.seconds_since_flip).c_str());
        ImGui::EndTooltip();
    }

    // Flipped
    ImGui::TableNextColumn();
    ImGui::TextColored(GetFlipTimeColor(obj.seconds_since_flip), "%s",
        FormatDuration(obj.seconds_since_flip).c_str());

    // Tier
    ImGui::TableNextColumn();
    if (obj.type == RealmReport::ObjectiveType::Camp ||
        obj.type == RealmReport::ObjectiveType::Tower ||
        obj.type == RealmReport::ObjectiveType::Keep ||
        obj.type == RealmReport::ObjectiveType::Castle) {
        DrawTierPips(obj.upgrade_tier, obj.yaks_delivered);
    } else {
        ImGui::TextColored(ImVec4(0.3f, 0.3f, 0.3f, 1.0f), "-");
    }

    // Claimed
    ImGui::TableNextColumn();
    if (!obj.claimed_by.empty()) {
        std::string display = obj.claimed_by;
        size_t open = obj.claimed_by.find('[');
        size_t close = obj.claimed_by.find(']');
        if (open != std::string::npos && close != std::string::npos && close > open) {
            display = obj.claimed_by.substr(open, close - open + 1);
        }
        ImGui::TextColored(ImVec4(0.75f, 0.75f, 0.75f, 1.0f), "%s", display.c_str());
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::Text("%s", obj.claimed_by.c_str());
            ImGui::EndTooltip();
        }
    } else {
        ImGui::TextColored(ImVec4(0.30f, 0.30f, 0.30f, 1.0f), "-");
    }

    ImGui::PopID();
}

struct FlatObjective {
    RealmReport::Objective obj;
    std::string map_type;
};

static void SortFlatObjectives(std::vector<FlatObjective>& objs) {
    std::sort(objs.begin(), objs.end(), [](const FlatObjective& fa, const FlatObjective& fb) -> bool {
        const auto& a = fa.obj;
        const auto& b = fb.obj;
        int cmp = 0;
        switch (g_SortColumn) {
            case RealmReport::SortColumn::Map:
                cmp = fa.map_type.compare(fb.map_type);
                break;
            case RealmReport::SortColumn::Name:
                cmp = a.name.compare(b.name);
                break;
            case RealmReport::SortColumn::Type: {
                int ta = (int)a.type, tb = (int)b.type;
                cmp = (ta < tb) ? -1 : (ta > tb) ? 1 : 0;
                break;
            }
            case RealmReport::SortColumn::Owner: {
                int oa = (int)a.owner, ob = (int)b.owner;
                cmp = (oa < ob) ? -1 : (oa > ob) ? 1 : 0;
                break;
            }
            case RealmReport::SortColumn::TimeSinceFlip:
                cmp = (a.seconds_since_flip < b.seconds_since_flip) ? -1 :
                      (a.seconds_since_flip > b.seconds_since_flip) ? 1 : 0;
                break;
            case RealmReport::SortColumn::YaksDelivered:
                cmp = (a.yaks_delivered < b.yaks_delivered) ? -1 :
                      (a.yaks_delivered > b.yaks_delivered) ? 1 : 0;
                break;
            case RealmReport::SortColumn::ClaimedBy:
                cmp = a.claimed_by.compare(b.claimed_by);
                break;
            case RealmReport::SortColumn::PPT:
                cmp = (a.points_tick < b.points_tick) ? -1 :
                      (a.points_tick > b.points_tick) ? 1 : 0;
                break;
            default:
                cmp = (a.seconds_since_flip < b.seconds_since_flip) ? -1 :
                      (a.seconds_since_flip > b.seconds_since_flip) ? 1 : 0;
                break;
        }
        return g_SortAscending ? (cmp < 0) : (cmp > 0);
    });
}

static void RenderFlatObjectivesTable(const RealmReport::MatchData& match) {
    if (match.maps.empty()) return;

    static const char* map_order[] = { "Center", "RedHome", "BlueHome", "GreenHome" };
    static bool* map_filters[] = { &g_FilterEBG, &g_FilterRedBL, &g_FilterBlueBL, &g_FilterGreenBL };

    // Collect all objectives from enabled maps
    std::vector<FlatObjective> all;
    for (int m = 0; m < 4; m++) {
        if (!*map_filters[m]) continue;
        for (const auto& mp : match.maps) {
            if (mp.type != map_order[m]) continue;
            for (const auto& obj : mp.objectives) {
                if (!PassesTypeFilter(obj.type)) continue;
                if (obj.name.empty()) continue;
                all.push_back({obj, mp.type});
            }
        }
    }
    if (all.empty()) return;

    SortFlatObjectives(all);

    ImDrawList* dl = ImGui::GetWindowDrawList();

    ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                            ImGuiTableFlags_Sortable | ImGuiTableFlags_SizingStretchProp |
                            ImGuiTableFlags_NoPadOuterX;

    if (ImGui::BeginTable("##obj_flat", 5, flags)) {
        auto ColFlags = [](RealmReport::SortColumn col) -> ImGuiTableColumnFlags {
            return (g_SortColumn == col) ? ImGuiTableColumnFlags_DefaultSort : (ImGuiTableColumnFlags)0;
        };
        ImGui::TableSetupColumn("Map",       ImGuiTableColumnFlags_WidthFixed | ColFlags(RealmReport::SortColumn::Map), 50.0f, (ImGuiID)RealmReport::SortColumn::Map);
        ImGui::TableSetupColumn("Objective", ImGuiTableColumnFlags_WidthStretch | ColFlags(RealmReport::SortColumn::Name), 3.0f, (ImGuiID)RealmReport::SortColumn::Name);
        ImGui::TableSetupColumn("Flipped",   ImGuiTableColumnFlags_WidthFixed | ColFlags(RealmReport::SortColumn::TimeSinceFlip), 55.0f, (ImGuiID)RealmReport::SortColumn::TimeSinceFlip);
        ImGui::TableSetupColumn("Tier",      ImGuiTableColumnFlags_WidthFixed | ColFlags(RealmReport::SortColumn::YaksDelivered), 38.0f, (ImGuiID)RealmReport::SortColumn::YaksDelivered);
        ImGui::TableSetupColumn("Claimed",   ImGuiTableColumnFlags_WidthStretch | ColFlags(RealmReport::SortColumn::ClaimedBy), 1.5f, (ImGuiID)RealmReport::SortColumn::ClaimedBy);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs()) {
            if (specs->SpecsDirty && specs->SpecsCount > 0) {
                g_SortColumn = (RealmReport::SortColumn)specs->Specs[0].ColumnUserID;
                g_SortAscending = (specs->Specs[0].SortDirection == ImGuiSortDirection_Ascending);
                specs->SpecsDirty = false;
                SortFlatObjectives(all);
                RealmReport::WvWAPI::SetSortState((int)g_SortColumn, g_SortAscending);
                RealmReport::WvWAPI::SaveSelectedWorld();
            }
        }

        for (int i = 0; i < (int)all.size(); i++) {
            RenderObjectiveRowFlat(dl, all[i].obj, all[i].map_type, i);
        }

        ImGui::EndTable();
    }
}

static void RenderObjectivesTable(const RealmReport::MatchData& match) {
    if (match.maps.empty()) return;

    if (g_FlatList) {
        RenderFlatObjectivesTable(match);
        return;
    }

    // Map display order
    static const char* map_order[] = { "Center", "RedHome", "BlueHome", "GreenHome" };
    static bool* map_filters[] = { &g_FilterEBG, &g_FilterRedBL, &g_FilterBlueBL, &g_FilterGreenBL };

    ImDrawList* dl = ImGui::GetWindowDrawList();

    for (int m = 0; m < 4; m++) {
        if (!*map_filters[m]) continue;

        // Find the map data
        const RealmReport::WvWMap* wmap = nullptr;
        for (const auto& mp : match.maps) {
            if (mp.type == map_order[m]) { wmap = &mp; break; }
        }
        if (!wmap) continue;

        // Filter objectives
        std::vector<RealmReport::Objective> filtered;
        for (const auto& obj : wmap->objectives) {
            if (!PassesTypeFilter(obj.type)) continue;
            if (obj.name.empty()) continue;
            filtered.push_back(obj);
        }
        if (filtered.empty()) continue;

        SortObjectives(filtered);

        // Colored collapsing header per map
        ImVec4 hdrColor = GetMapHeaderColor(wmap->type);
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(hdrColor.x, hdrColor.y, hdrColor.z, 0.20f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(hdrColor.x, hdrColor.y, hdrColor.z, 0.35f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(hdrColor.x, hdrColor.y, hdrColor.z, 0.45f));

        char hdr_label[64];
        snprintf(hdr_label, sizeof(hdr_label), "%s  (%d)", MapShortName(wmap->type), (int)filtered.size());

        bool open = ImGui::CollapsingHeader(hdr_label, ImGuiTreeNodeFlags_DefaultOpen);
        ImGui::PopStyleColor(3);

        if (!open) continue;

        // Table within each map section
        ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                ImGuiTableFlags_Sortable | ImGuiTableFlags_SizingStretchProp |
                                ImGuiTableFlags_NoPadOuterX;

        char table_id[32];
        snprintf(table_id, sizeof(table_id), "##obj_%d", m);

        if (ImGui::BeginTable(table_id, 4, flags)) {
            auto ColFlags = [](RealmReport::SortColumn col) -> ImGuiTableColumnFlags {
                return (g_SortColumn == col) ? ImGuiTableColumnFlags_DefaultSort : (ImGuiTableColumnFlags)0;
            };
            ImGui::TableSetupColumn("Objective", ImGuiTableColumnFlags_WidthStretch | ColFlags(RealmReport::SortColumn::Name), 3.0f, (ImGuiID)RealmReport::SortColumn::Name);
            ImGui::TableSetupColumn("Flipped",   ImGuiTableColumnFlags_WidthFixed | ColFlags(RealmReport::SortColumn::TimeSinceFlip), 55.0f, (ImGuiID)RealmReport::SortColumn::TimeSinceFlip);
            ImGui::TableSetupColumn("Tier",      ImGuiTableColumnFlags_WidthFixed | ColFlags(RealmReport::SortColumn::YaksDelivered), 38.0f, (ImGuiID)RealmReport::SortColumn::YaksDelivered);
            ImGui::TableSetupColumn("Claimed",   ImGuiTableColumnFlags_WidthStretch | ColFlags(RealmReport::SortColumn::ClaimedBy), 1.5f, (ImGuiID)RealmReport::SortColumn::ClaimedBy);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();

            // Handle sort specs
            if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs()) {
                if (specs->SpecsDirty && specs->SpecsCount > 0) {
                    g_SortColumn = (RealmReport::SortColumn)specs->Specs[0].ColumnUserID;
                    g_SortAscending = (specs->Specs[0].SortDirection == ImGuiSortDirection_Ascending);
                    specs->SpecsDirty = false;
                    SortObjectives(filtered);
                    RealmReport::WvWAPI::SetSortState((int)g_SortColumn, g_SortAscending);
                    RealmReport::WvWAPI::SaveSelectedWorld();
                }
            }

            for (int i = 0; i < (int)filtered.size(); i++) {
                RenderObjectiveRow(dl, filtered[i], m * 100 + i);
            }

            ImGui::EndTable();
        }
        ImGui::Spacing();
    }
}

// --- Addon Lifecycle ---

void AddonLoad(AddonAPI_t* aApi) {
    APIDefs = aApi;
    ImGui::SetCurrentContext((ImGuiContext*)APIDefs->ImguiContext);
    ImGui::SetAllocatorFunctions((void* (*)(size_t, void*))APIDefs->ImguiMalloc,
                                 (void(*)(void*, void*))APIDefs->ImguiFree);

    // Initialize WvW API
    RealmReport::WvWAPI::Initialize();

    // Load saved settings
    bool hasWorld = RealmReport::WvWAPI::LoadSelectedWorld();
    g_FlipNotifications = RealmReport::WvWAPI::GetFlipNotifications();
    g_ToastDuration = RealmReport::WvWAPI::GetToastDuration();
    g_FlipSound = RealmReport::WvWAPI::GetFlipSoundEnabled();
    g_FlipSoundFile = RealmReport::WvWAPI::GetFlipSoundFile();
    g_PinnedOpacity = RealmReport::WvWAPI::GetPinnedOpacity();
    g_FlatList = RealmReport::WvWAPI::GetFlatList();
    {
        int col; bool asc;
        RealmReport::WvWAPI::GetSortState(col, asc);
        g_SortColumn = (RealmReport::SortColumn)col;
        g_SortAscending = asc;
    }
    RealmReport::WvWAPI::GetToastLayout(g_ToastAnchorX, g_ToastAnchorY, g_ToastW, g_ToastH);

    // Fetch world list
    RealmReport::WvWAPI::FetchWorldListAsync();

    // Polling is now managed automatically by ManageAutoPolling() based on WvW map presence

    // Register render functions
    APIDefs->GUI_Register(RT_Render, AddonRender);
    APIDefs->GUI_Register(RT_OptionsRender, AddonOptions);

    // Register keybind
    APIDefs->InputBinds_RegisterWithString("KB_REALM_TOGGLE", ProcessKeybind, "ALT+SHIFT+W");
    APIDefs->InputBinds_RegisterWithString("KB_REALM_PIN", ProcessKeybind, "(null)");

    // Register close-on-escape
    APIDefs->GUI_RegisterCloseOnEscape("Realm Report", &g_WindowVisible);

    APIDefs->Log(LOGL_INFO, "RealmReport", "Addon loaded successfully");
}

void AddonUnload() {
    RealmReport::WvWAPI::Shutdown();

    APIDefs->GUI_DeregisterCloseOnEscape("Realm Report");
    APIDefs->InputBinds_Deregister("KB_REALM_TOGGLE");
    APIDefs->InputBinds_Deregister("KB_REALM_PIN");
    APIDefs->GUI_Deregister(AddonOptions);
    APIDefs->GUI_Deregister(AddonRender);

    APIDefs = nullptr;
}

void ProcessKeybind(const char* aIdentifier, bool aIsRelease) {
    if (aIsRelease) return;

    if (strcmp(aIdentifier, "KB_REALM_TOGGLE") == 0) {
        if (g_WindowPinned) {
            // Unpin the window instead of hiding it
            g_WindowPinned = false;
        } else {
            g_WindowVisible = !g_WindowVisible;
        }
        if (APIDefs) {
            APIDefs->Log(LOGL_INFO, "RealmReport",
                g_WindowVisible ? "Window shown" : "Window hidden");
        }
    }
    if (strcmp(aIdentifier, "KB_REALM_PIN") == 0) {
        if (g_WindowVisible) {
            g_WindowPinned = !g_WindowPinned;
        }
    }
}

// --- Flip Sound ---

static void PlayFlipSound() {
    if (!g_FlipSound || g_FlipSoundFile.empty()) return;

    // Resolve full path from sounds/ directory
    std::string fullPath = RealmReport::WvWAPI::GetSoundsDirectory() + "/" + g_FlipSoundFile;
    // Normalize to backslashes for Windows
    std::replace(fullPath.begin(), fullPath.end(), '/', '\\');

    if (APIDefs) {
        APIDefs->Log(LOGL_INFO, "RealmReport",
            (std::string("PlayFlipSound: path='") + fullPath + "'").c_str());
    }

    // Determine file extension
    std::string ext;
    size_t dot = g_FlipSoundFile.find_last_of('.');
    if (dot != std::string::npos) {
        ext = g_FlipSoundFile.substr(dot);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    }

    if (ext == ".mp3") {
        // Use MCI for mp3 playback — unique alias to avoid conflicts with other addons
        static int s_mciCounter = 0;
        std::string alias = "RR_Snd_" + std::to_string(++s_mciCounter);
        // Close previous instance if any
        if (s_mciCounter > 1) {
            std::string prevAlias = "RR_Snd_" + std::to_string(s_mciCounter - 1);
            mciSendStringA(("close " + prevAlias).c_str(), NULL, 0, NULL);
        }
        std::string cmd = "open \"" + fullPath + "\" type mpegvideo alias " + alias;
        MCIERROR err = mciSendStringA(cmd.c_str(), NULL, 0, NULL);
        if (err != 0) {
            char errBuf[256] = {};
            mciGetErrorStringA(err, errBuf, sizeof(errBuf));
            if (APIDefs) {
                APIDefs->Log(LOGL_WARNING, "RealmReport",
                    (std::string("MCI open error: ") + errBuf).c_str());
            }
            return;
        }
        err = mciSendStringA(("play " + alias).c_str(), NULL, 0, NULL);
        if (err != 0) {
            char errBuf[256] = {};
            mciGetErrorStringA(err, errBuf, sizeof(errBuf));
            if (APIDefs) {
                APIDefs->Log(LOGL_WARNING, "RealmReport",
                    (std::string("MCI play error: ") + errBuf).c_str());
            }
        }
    } else {
        // Use PlaySound for wav (async, non-blocking)
        BOOL ok = PlaySoundA(fullPath.c_str(), NULL, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
        if (!ok && APIDefs) {
            APIDefs->Log(LOGL_WARNING, "RealmReport", "PlaySoundA failed");
        }
    }
}

// --- Toast Notification Rendering ---

static void ProcessFlipEvents() {
    if (!g_FlipNotifications) return;

    auto events = RealmReport::WvWAPI::PopFlipEvents();
    for (const auto& fe : events) {
        // Only show toasts for maps the user has enabled
        if (!PassesMapFilterByType(fe.map_type)) continue;

        Toast t;
        t.obj_name = fe.obj_name;
        t.type = fe.type;
        t.map_display = fe.map_display;
        t.old_owner = fe.old_owner;
        t.new_owner = fe.new_owner;
        t.spawn_time = fe.timestamp;
        g_Toasts.push_back(t);
    }
    if (!events.empty()) PlayFlipSound();
}

// Resolve anchor to actual screen position (defaults to bottom-right)
static void GetToastOrigin(float& outX, float& outY) {
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    outX = (g_ToastAnchorX < 0) ? (ds.x - g_ToastW - 8.0f) : g_ToastAnchorX;
    outY = (g_ToastAnchorY < 0) ? (ds.y - 8.0f) : g_ToastAnchorY;
}

static void RenderToastEditPlaceholder() {
    if (!g_ToastEditMode) return;

    float anchorX, anchorY;
    GetToastOrigin(anchorX, anchorY);

    // Render draggable handle FIRST so anchor updates before previews
    float handleH = 22.0f;
    ImGuiCond posCond = g_ToastResetPos ? ImGuiCond_Always : ImGuiCond_Appearing;
    g_ToastResetPos = false;
    ImGui::SetNextWindowPos(ImVec2(anchorX, anchorY - (g_ToastH + 4.0f) * 2 - handleH), posCond);
    ImGui::SetNextWindowSize(ImVec2(g_ToastW, handleH));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 2));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.20f, 0.20f, 0.08f, 0.9f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.8f, 0.8f, 0.2f, 0.9f));

    ImGuiWindowFlags handle_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
                                    ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoFocusOnAppearing |
                                    ImGuiWindowFlags_NoNav;

    if (ImGui::Begin("##toast_handle", nullptr, handle_flags)) {
        ImVec2 newPos = ImGui::GetWindowPos();
        // Derive anchor from handle position
        g_ToastAnchorX = newPos.x;
        g_ToastAnchorY = newPos.y + handleH + (g_ToastH + 4.0f) * 2;
        // Update live for preview
        anchorX = g_ToastAnchorX;
        anchorY = g_ToastAnchorY;

        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.5f, 1.0f), "Drag to move notifs");
    }
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);

    // Show 2 sample toasts following the handle
    for (int s = 0; s < 2; s++) {
        float posY = anchorY - (g_ToastH + 4.0f) * (2 - s);

        char wndId[32];
        snprintf(wndId, sizeof(wndId), "##toast_preview_%d", s);

        ImGui::SetNextWindowPos(ImVec2(anchorX, posY));
        ImGui::SetNextWindowSize(ImVec2(g_ToastW, g_ToastH));
        ImGui::SetNextWindowBgAlpha(0.75f);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.12f, 0.12f, 0.14f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.6f, 0.6f, 0.2f, 0.8f));

        ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar |
                                 ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                                 ImGuiWindowFlags_NoBringToFrontOnFocus |
                                 ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove;

        if (ImGui::Begin(wndId, nullptr, flags)) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 cursor = ImGui::GetCursorScreenPos();
            float fontSize = ImGui::GetFontSize();
            auto col = (s == 0) ? RealmReport::TeamColor::Red : RealmReport::TeamColor::Blue;
            auto otype = (s == 0) ? RealmReport::ObjectiveType::Tower : RealmReport::ObjectiveType::Keep;

            ImVec4 c = GetTeamColor(col);
            ImVec2 wp = ImGui::GetWindowPos();
            dl->AddRectFilled(ImVec2(wp.x, wp.y), ImVec2(wp.x + 4, wp.y + g_ToastH),
                ImGui::ColorConvertFloat4ToU32(c), 6.0f);

            float dotR = 4.0f;
            DrawOwnerDot(dl, ImVec2(cursor.x + dotR, cursor.y + fontSize * 0.5f), dotR, col);
            float badgeX = cursor.x + dotR * 2 + 5;
            DrawTypeBadge(dl, ImVec2(badgeX, cursor.y), otype);
            float nameX = badgeX + fontSize + 3;
            ImGui::SetCursorScreenPos(ImVec2(nameX, cursor.y));
            ImGui::Text("%s", s == 0 ? "Sample Tower" : "Sample Keep");
            ImGui::SetCursorScreenPos(ImVec2(cursor.x + dotR * 2 + 5, cursor.y + fontSize + 2));
            ImGui::TextColored(ImVec4(0.65f, 0.65f, 0.65f, 1.0f), "%s", s == 0 ? "EBG" : "Red BL");
            ImGui::SameLine(0, 6);
            ImGui::TextColored(ImVec4(0.25f, 0.80f, 0.25f, 1.0f), "from Green");
        }
        ImGui::End();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);
    }
}

static void RenderToasts() {
    if (g_Toasts.empty()) return;

    auto now = std::chrono::steady_clock::now();

    // Remove expired toasts
    g_Toasts.erase(
        std::remove_if(g_Toasts.begin(), g_Toasts.end(), [&](const Toast& t) {
            float age = std::chrono::duration<float>(now - t.spawn_time).count();
            return age > g_ToastDuration;
        }),
        g_Toasts.end()
    );

    if (g_Toasts.empty()) return;

    float anchorX, anchorY;
    GetToastOrigin(anchorX, anchorY);

    for (int i = (int)g_Toasts.size() - 1; i >= 0; i--) {
        const Toast& t = g_Toasts[i];
        float age = std::chrono::duration<float>(now - t.spawn_time).count();

        // Compute alpha (fade in briefly, stay, fade out)
        float alpha = 1.0f;
        if (age < 0.2f) {
            alpha = age / 0.2f; // fade in
        } else if (age > g_ToastDuration - TOAST_FADE_TIME) {
            alpha = (g_ToastDuration - age) / TOAST_FADE_TIME;
        }
        if (alpha <= 0.0f) continue;

        float posY = anchorY - (g_ToastH + 4.0f) * ((int)g_Toasts.size() - i);

        char wndId[32];
        snprintf(wndId, sizeof(wndId), "##toast_%d", i);

        ImGui::SetNextWindowPos(ImVec2(anchorX, posY));
        ImGui::SetNextWindowSize(ImVec2(g_ToastW, g_ToastH));
        ImGui::SetNextWindowBgAlpha(0.85f * alpha);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.12f, 0.12f, 0.14f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.3f, 0.3f, 0.3f, 0.5f * alpha));

        ImGuiWindowFlags toast_flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                                       ImGuiWindowFlags_NoFocusOnAppearing |
                                       ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav;

        if (ImGui::Begin(wndId, nullptr, toast_flags)) {
            // Click to dismiss
            if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                g_Toasts.erase(g_Toasts.begin() + i);
                ImGui::End();
                ImGui::PopStyleColor(2);
                ImGui::PopStyleVar(2);
                continue;
            }
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 cursor = ImGui::GetCursorScreenPos();
            float fontSize = ImGui::GetFontSize();

            // Left color bar (new owner color)
            ImVec4 newCol = GetTeamColor(t.new_owner);
            ImU32 barCol = ImGui::ColorConvertFloat4ToU32(ImVec4(newCol.x, newCol.y, newCol.z, alpha));
            ImVec2 winPos = ImGui::GetWindowPos();
            dl->AddRectFilled(ImVec2(winPos.x, winPos.y), ImVec2(winPos.x + 4, winPos.y + g_ToastH), barCol, 6.0f);

            // Owner dot + type badge + name (first line)
            float dotR = 4.0f;
            DrawOwnerDot(dl, ImVec2(cursor.x + dotR, cursor.y + fontSize * 0.5f), dotR, t.new_owner);
            float badgeX = cursor.x + dotR * 2 + 5;
            DrawTypeBadge(dl, ImVec2(badgeX, cursor.y), t.type);
            float nameX = badgeX + fontSize + 3;
            ImGui::SetCursorScreenPos(ImVec2(nameX, cursor.y));
            ImVec4 textCol(1.0f, 1.0f, 1.0f, alpha);
            ImGui::TextColored(textCol, "%s", t.obj_name.c_str());

            // Second line: map + "from [old owner]"
            ImVec4 dimCol(0.65f, 0.65f, 0.65f, alpha);
            ImGui::SetCursorScreenPos(ImVec2(cursor.x + dotR * 2 + 5, cursor.y + fontSize + 2));
            ImGui::TextColored(dimCol, "%s", t.map_display.c_str());
            ImGui::SameLine(0, 6);
            ImVec4 oldCol = GetTeamColor(t.old_owner);
            ImGui::TextColored(ImVec4(oldCol.x, oldCol.y, oldCol.z, alpha), "from %s",
                RealmReport::TeamColorToString(t.old_owner));
        }
        ImGui::End();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);
    }
}

// --- Mumble Link Map Detection ---

static void UpdateWvWMapState() {
    if (!APIDefs) return;

    // Throttle to ~1 check per second
    static auto lastCheck = std::chrono::steady_clock::time_point{};
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration<float>(now - lastCheck).count() < 1.0f) return;
    lastCheck = now;

    MumbleLinkedMem* mumble = (MumbleLinkedMem*)APIDefs->DataLink_Get(DL_MUMBLE_LINK);
    // Need at least 36 bytes: 28 (serverAddress) + 4 (mapId) + 4 (mapType)
    if (!mumble || mumble->context_len < 36) {
        g_IsOnWvWMap = false;
        return;
    }
    GW2Context* ctx = (GW2Context*)mumble->context;
    g_IsOnWvWMap = IsWvWMapType(ctx->mapType);
}

static void ManageAutoPolling() {
    if (RealmReport::WvWAPI::GetSelectedWorld() <= 0) return;

    if (g_IsOnWvWMap && !g_Paused) {
        if (!RealmReport::WvWAPI::IsPolling()) {
            RealmReport::WvWAPI::StartPolling();
        }
    } else {
        if (RealmReport::WvWAPI::IsPolling()) {
            RealmReport::WvWAPI::StopPolling();
        }
    }
}

// --- Main Render ---

void AddonRender() {
    // Check if player is on a WvW map
    UpdateWvWMapState();
    ManageAutoPolling();

    // Edit mode placeholder always renders (it's a config tool, not a notification)
    if (g_ToastEditMode) {
        RenderToastEditPlaceholder();
    } else if (g_IsOnWvWMap && !g_Paused) {
        ProcessFlipEvents();
        RenderToasts();
    } else {
        // Drain any pending flip events so they don't pile up
        RealmReport::WvWAPI::PopFlipEvents();
    }

    if (!g_WindowVisible) return;

    ImGui::SetNextWindowSizeConstraints(ImVec2(380, 200), ImVec2(FLT_MAX, FLT_MAX));
    ImGuiWindowFlags winFlags = ImGuiWindowFlags_NoCollapse;
    bool pushedAlpha = false;
    if (g_WindowPinned) {
        winFlags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize
                 |  ImGuiWindowFlags_NoInputs
                 |  ImGuiWindowFlags_NoBringToFrontOnFocus
                 |  ImGuiWindowFlags_NoFocusOnAppearing;
        ImGui::SetNextWindowBgAlpha(g_PinnedOpacity);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, g_PinnedOpacity);
        pushedAlpha = true;
    }
    if (!ImGui::Begin("Realm Report - WvW Objectives", &g_WindowVisible, winFlags))
    {
        ImGui::End();
        if (pushedAlpha) ImGui::PopStyleVar();
        return;
    }

    // Pin button (right-aligned, top of content area) — only when not pinned
    if (!g_WindowPinned) {
        float avail = ImGui::GetContentRegionAvail().x;
        ImGui::SameLine(avail - ImGui::CalcTextSize("Pin").x - ImGui::GetStyle().FramePadding.x * 2);
        if (ImGui::SmallButton("Pin")) {
            g_WindowPinned = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Lock position & click-through");
        }
    }

    // World selector
    RenderWorldSelector();

    // Reserve space for bottom status bar
    float statusBarH = ImGui::GetFrameHeightWithSpacing() + 4;

    if (RealmReport::WvWAPI::HasMatchData()) {
        RealmReport::MatchData match = RealmReport::WvWAPI::GetMatchData();

        // Scoreboard
        RenderScoreboard(match);

        // Filter bar + map toggles on same line
        RenderFilterBar();
        ImGui::SameLine(0, 12);
        ImGui::TextColored(ImVec4(0.45f, 0.45f, 0.45f, 1.0f), "|");
        ImGui::SameLine(0, 12);

        // Map toggles as compact colored text buttons
        auto MapToggle = [](const char* label, bool* val, ImVec4 color) {
            if (*val) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(color.x, color.y, color.z, 0.25f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(color.x, color.y, color.z, 0.40f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(color.x, color.y, color.z, 1.0f));
            } else {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.2f, 0.2f, 0.3f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.3f, 0.3f, 0.4f));
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.4f, 0.4f, 1.0f));
            }
            if (ImGui::SmallButton(label)) *val = !*val;
            ImGui::PopStyleColor(3);
        };

        MapToggle("EBG", &g_FilterEBG, ImVec4(0.85f, 0.75f, 0.40f, 1.0f));
        ImGui::SameLine(0, 2);
        MapToggle("Red", &g_FilterRedBL, GetTeamColor(RealmReport::TeamColor::Red));
        ImGui::SameLine(0, 2);
        MapToggle("Blue", &g_FilterBlueBL, GetTeamColor(RealmReport::TeamColor::Blue));
        ImGui::SameLine(0, 2);
        MapToggle("Green", &g_FilterGreenBL, GetTeamColor(RealmReport::TeamColor::Green));

        ImGui::Spacing();

        // Scrollable objectives area — leave room for status bar
        float scrollH = ImGui::GetContentRegionAvail().y - statusBarH;
        if (scrollH < 50) scrollH = 50;
        ImGuiWindowFlags scrollFlags = g_WindowPinned ? ImGuiWindowFlags_NoInputs : 0;
        ImGui::BeginChild("##obj_scroll", ImVec2(0, scrollH), false, scrollFlags);
        RenderObjectivesTable(match);
        ImGui::EndChild();
    } else if (RealmReport::WvWAPI::GetSelectedWorld() > 0) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Waiting for match data...");
    } else {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Select a world to begin tracking.");
    }

    // --- Bottom status bar ---
    ImGui::Separator();
    if (RealmReport::WvWAPI::IsError()) {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s",
            RealmReport::WvWAPI::GetStatusMessage().c_str());
    } else if (g_Paused) {
        ImGui::TextColored(ImVec4(0.85f, 0.65f, 0.2f, 1.0f), "Paused");
    } else if (RealmReport::WvWAPI::IsPolling() && RealmReport::WvWAPI::HasMatchData()) {
        int countdown = RealmReport::WvWAPI::GetSecondsUntilNextRefresh();
        ImGui::TextColored(ImVec4(0.55f, 0.55f, 0.55f, 1.0f), "Next refresh in %ds", countdown);
    } else {
        ImGui::TextColored(ImVec4(0.55f, 0.55f, 0.55f, 1.0f), "%s",
            RealmReport::WvWAPI::GetStatusMessage().c_str());
    }
    if (RealmReport::WvWAPI::GetSelectedWorld() > 0) {
        if (g_IsOnWvWMap) {
            // On WvW map: show Pause/Resume only
            float btnWidth = g_Paused ? 60.0f : 50.0f;
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - btnWidth);
            if (ImGui::SmallButton(g_Paused ? "Resume" : "Pause")) {
                g_Paused = !g_Paused;
            }
        } else {
            // Off WvW map: show manual Refresh only, with cooldown
            static std::chrono::steady_clock::time_point lastRefreshClick{};
            auto now = std::chrono::steady_clock::now();
            int cooldown = RealmReport::WvWAPI::GetPollIntervalSeconds();
            float elapsed = std::chrono::duration<float>(now - lastRefreshClick).count();
            bool onCooldown = (lastRefreshClick.time_since_epoch().count() != 0) && (elapsed < cooldown);

            if (onCooldown) {
                int remaining = cooldown - (int)elapsed;
                char label[32];
                snprintf(label, sizeof(label), "Refresh (%ds)", remaining);
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - 80);
                ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.4f);
                ImGui::SmallButton(label);
                ImGui::PopStyleVar();
            } else {
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - 55);
                if (ImGui::SmallButton("Refresh")) {
                    lastRefreshClick = now;
                    if (!RealmReport::WvWAPI::IsPolling()) {
                        RealmReport::WvWAPI::StartPolling();
                    }
                    RealmReport::WvWAPI::FetchNow();
                }
            }
        }
    }

    // Capture main window position before End()
    ImVec2 mainWinPos = ImGui::GetWindowPos();
    ImVec2 mainWinSize = ImGui::GetWindowSize();
    ImGui::End();
    if (pushedAlpha) {
        ImGui::PopStyleVar(); // Alpha
    }

    // Separate small overlay for Unpin button (clickable even when main window has NoInputs)
    if (g_WindowPinned && g_WindowVisible) {
        float unpinW = ImGui::CalcTextSize("Unpin").x + ImGui::GetStyle().FramePadding.x * 2 + 8;
        float unpinH = ImGui::GetFrameHeight() + 4;
        ImGui::SetNextWindowPos(ImVec2(mainWinPos.x + mainWinSize.x - unpinW - 2, mainWinPos.y + 1));
        ImGui::SetNextWindowSize(ImVec2(unpinW, unpinH));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 2));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
        ImGui::Begin("##RR_Unpin", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
            | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar
            | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
        if (ImGui::SmallButton("Unpin")) {
            g_WindowPinned = false;
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
    }
}

// --- Options/Settings Render ---

void AddonOptions() {
    ImGui::Text("Realm Report Settings");
    if (ImGui::SmallButton("Homepage")) {
        ShellExecuteA(NULL, "open", "https://pie.rocks.cc/", NULL, NULL, SW_SHOWNORMAL);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Buy me a coffee!")) {
        ShellExecuteA(NULL, "open", "https://ko-fi.com/pieorcake", NULL, NULL, SW_SHOWNORMAL);
    }
    ImGui::Separator();

    // Poll interval
    int interval = RealmReport::WvWAPI::GetPollIntervalSeconds();
    if (ImGui::SliderInt("Poll Interval (seconds)", &interval, 20, 120)) {
        RealmReport::WvWAPI::SetPollIntervalSeconds(interval);
        RealmReport::WvWAPI::SaveSelectedWorld();
    }

    ImGui::Spacing();

    // Pinned opacity
    {
        int pct = (int)(g_PinnedOpacity * 100.0f + 0.5f);
        if (ImGui::SliderInt("Opacity when Pinned", &pct, 10, 100, "%d%%")) {
            g_PinnedOpacity = pct / 100.0f;
            RealmReport::WvWAPI::SetPinnedOpacity(g_PinnedOpacity);
            RealmReport::WvWAPI::SaveSelectedWorld();
        }
    }

    ImGui::Spacing();

    // Display mode
    if (ImGui::Checkbox("Flat List (no map subheadings)", &g_FlatList)) {
        RealmReport::WvWAPI::SetFlatList(g_FlatList);
        RealmReport::WvWAPI::SaveSelectedWorld();
    }

    ImGui::Spacing();

    // Flip notifications toggle
    if (ImGui::Checkbox("Flip Notifications", &g_FlipNotifications)) {
        RealmReport::WvWAPI::SetFlipNotifications(g_FlipNotifications);
        RealmReport::WvWAPI::SaveSelectedWorld();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::Text("Show toast popups when objectives are flipped\non maps you have enabled");
        ImGui::EndTooltip();
    }

    if (g_FlipNotifications) {
        // Toast size
        ImGui::PushItemWidth(150);
        if (ImGui::SliderFloat("Toast Width", &g_ToastW, 150.0f, 400.0f, "%.0f")) {
            RealmReport::WvWAPI::SetToastLayout(g_ToastAnchorX, g_ToastAnchorY, g_ToastW, g_ToastH);
            RealmReport::WvWAPI::SaveSelectedWorld();
        }
        if (ImGui::SliderFloat("Toast Height", &g_ToastH, 32.0f, 80.0f, "%.0f")) {
            RealmReport::WvWAPI::SetToastLayout(g_ToastAnchorX, g_ToastAnchorY, g_ToastW, g_ToastH);
            RealmReport::WvWAPI::SaveSelectedWorld();
        }
        if (ImGui::SliderFloat("Toast Duration (sec)", &g_ToastDuration, 2.0f, 15.0f, "%.0f")) {
            RealmReport::WvWAPI::SetToastDuration(g_ToastDuration);
            RealmReport::WvWAPI::SaveSelectedWorld();
        }
        ImGui::PopItemWidth();

        // Position edit mode
        if (g_ToastEditMode) {
            if (ImGui::Button("Done Positioning")) {
                g_ToastEditMode = false;
                RealmReport::WvWAPI::SetToastLayout(g_ToastAnchorX, g_ToastAnchorY, g_ToastW, g_ToastH);
                RealmReport::WvWAPI::SaveSelectedWorld();
            }
        } else {
            if (ImGui::Button("Move Notifications")) {
                g_ToastEditMode = true;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset Position")) {
            g_ToastAnchorX = -1.0f;
            g_ToastAnchorY = -1.0f;
            g_ToastW = 230.0f;
            g_ToastH = 48.0f;
            g_ToastResetPos = true;
            RealmReport::WvWAPI::SetToastLayout(g_ToastAnchorX, g_ToastAnchorY, g_ToastW, g_ToastH);
            RealmReport::WvWAPI::SaveSelectedWorld();
        }

        ImGui::Spacing();

        // Flip sound
        if (ImGui::Checkbox("Play Sound on Flip", &g_FlipSound)) {
            RealmReport::WvWAPI::SetFlipSound(g_FlipSound, g_FlipSoundFile);
            RealmReport::WvWAPI::SaveSelectedWorld();
        }
        if (g_FlipSound) {
            static std::vector<std::string> soundFiles;
            static bool needScan = true;
            if (needScan) {
                soundFiles = RealmReport::WvWAPI::ScanSoundFiles();
                needScan = false;
            }

            const char* preview = g_FlipSoundFile.empty() ? "(none)" : g_FlipSoundFile.c_str();
            float comboW = ImGui::CalcTextSize(preview).x + ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x * 4;
            ImGui::PushItemWidth(comboW);
            if (ImGui::BeginCombo("##flip_sound_combo", preview)) {
                // Option to clear selection
                if (ImGui::Selectable("(none)", g_FlipSoundFile.empty())) {
                    g_FlipSoundFile.clear();
                    RealmReport::WvWAPI::SetFlipSound(g_FlipSound, g_FlipSoundFile);
                    RealmReport::WvWAPI::SaveSelectedWorld();
                }
                for (const auto& f : soundFiles) {
                    bool selected = (f == g_FlipSoundFile);
                    if (ImGui::Selectable(f.c_str(), selected)) {
                        g_FlipSoundFile = f;
                        RealmReport::WvWAPI::SetFlipSound(g_FlipSound, g_FlipSoundFile);
                        RealmReport::WvWAPI::SaveSelectedWorld();
                    }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::PopItemWidth();

            if (ImGui::SmallButton("Rescan")) { needScan = true; }
            ImGui::SameLine();
            if (ImGui::SmallButton("Test Sound")) {
                PlayFlipSound();
            }
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Place files in sounds/");
        }
    }

    ImGui::Spacing();
    ImGui::Text("Default keybind: ALT+SHIFT+W");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text("Data source: Official GW2 API (api.guildwars2.com)");
    ImGui::Text("No API key required - WvW data is public.");
}

// --- Export: GetAddonDef ---

extern "C" __declspec(dllexport) AddonDefinition_t* GetAddonDef() {
    AddonDef.Signature = 0x5ab950b6;
    AddonDef.APIVersion = NEXUS_API_VERSION;
    AddonDef.Name = "Realm Report";
    AddonDef.Version.Major = V_MAJOR;
    AddonDef.Version.Minor = V_MINOR;
    AddonDef.Version.Build = V_BUILD;
    AddonDef.Version.Revision = V_REVISION;
    AddonDef.Author = "PieOrCake.7635";
    AddonDef.Description = "WvW objective tracker - live overview of camps, towers, keeps across all maps";
    AddonDef.Load = AddonLoad;
    AddonDef.Unload = AddonUnload;
    AddonDef.Flags = AF_None;
    AddonDef.Provider = UP_GitHub;
    AddonDef.UpdateLink = "https://github.com/PieOrCake/realm_report";

    return &AddonDef;
}
