#include "WvWAPI.h"

#include <windows.h>
#include <wininet.h>
#include <fstream>
#include <sstream>
#include <thread>
#include <algorithm>
#include <filesystem>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace RealmReport {

    // --- Utility functions ---

    const char* ObjectiveTypeToString(ObjectiveType type) {
        switch (type) {
            case ObjectiveType::Camp:      return "Camp";
            case ObjectiveType::Tower:     return "Tower";
            case ObjectiveType::Keep:      return "Keep";
            case ObjectiveType::Castle:    return "Castle";
            case ObjectiveType::Spawn:     return "Spawn";
            case ObjectiveType::Ruins:     return "Ruins";
            case ObjectiveType::Mercenary: return "Mercenary";
            default:                       return "Unknown";
        }
    }

    const char* TeamColorToString(TeamColor color) {
        switch (color) {
            case TeamColor::Red:     return "Red";
            case TeamColor::Green:   return "Green";
            case TeamColor::Blue:    return "Blue";
            case TeamColor::Neutral: return "Neutral";
            default:                 return "Unknown";
        }
    }

    ObjectiveType StringToObjectiveType(const std::string& s) {
        if (s == "Camp")      return ObjectiveType::Camp;
        if (s == "Tower")     return ObjectiveType::Tower;
        if (s == "Keep")      return ObjectiveType::Keep;
        if (s == "Castle")    return ObjectiveType::Castle;
        if (s == "Spawn")     return ObjectiveType::Spawn;
        if (s == "Ruins")     return ObjectiveType::Ruins;
        if (s == "Mercenary") return ObjectiveType::Mercenary;
        return ObjectiveType::Unknown;
    }

    TeamColor StringToTeamColor(const std::string& s) {
        if (s == "Red")     return TeamColor::Red;
        if (s == "Green")   return TeamColor::Green;
        if (s == "Blue")    return TeamColor::Blue;
        if (s == "Neutral") return TeamColor::Neutral;
        return TeamColor::Unknown;
    }

    int ComputeUpgradeTier(int yaks_delivered) {
        if (yaks_delivered >= 140) return 3;
        if (yaks_delivered >= 60)  return 2;
        if (yaks_delivered >= 20)  return 1;
        return 0;
    }

    // --- Static member initialization ---

    std::unordered_map<std::string, std::string> WvWAPI::s_objective_names;
    std::unordered_map<std::string, std::string> WvWAPI::s_objective_types;
    std::unordered_map<std::string, std::string> WvWAPI::s_guild_names;
    std::vector<World> WvWAPI::s_worlds;
    std::atomic<bool> WvWAPI::s_worlds_ready{false};
    int WvWAPI::s_selected_world = 0;
    MatchData WvWAPI::s_match_data;
    std::atomic<bool> WvWAPI::s_has_match_data{false};
    std::string WvWAPI::s_status_message = "Idle";
    std::atomic<bool> WvWAPI::s_is_error{false};
    std::atomic<bool> WvWAPI::s_polling{false};
    std::atomic<bool> WvWAPI::s_shutdown{false};
    std::atomic<bool> WvWAPI::s_fetch_now{false};
    int WvWAPI::s_poll_interval = 30;
    std::chrono::steady_clock::time_point WvWAPI::s_last_fetch_time{};
    std::mutex WvWAPI::s_mutex;
    std::atomic<bool> WvWAPI::s_objectives_cached{false};
    std::vector<FlipEvent> WvWAPI::s_flip_events;
    bool WvWAPI::s_flip_notifications = true;
    float WvWAPI::s_toast_anchor_x = -1.0f;
    float WvWAPI::s_toast_anchor_y = -1.0f;
    float WvWAPI::s_toast_w = 230.0f;
    float WvWAPI::s_toast_h = 48.0f;
    bool WvWAPI::s_flat_list = false;
    float WvWAPI::s_toast_duration = 5.0f;
    bool WvWAPI::s_flip_sound = false;
    std::string WvWAPI::s_flip_sound_path;
    int WvWAPI::s_sort_column = (int)SortColumn::TimeSinceFlip;
    bool WvWAPI::s_sort_ascending = true;

    // --- Helper: get DLL directory ---

    static std::string GetDllDir() {
        char dllPath[MAX_PATH];
        HMODULE hModule = NULL;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)GetDllDir, &hModule)) {
            if (GetModuleFileNameA(hModule, dllPath, MAX_PATH)) {
                std::string path(dllPath);
                size_t lastSlash = path.find_last_of("\\/");
                if (lastSlash != std::string::npos) {
                    return path.substr(0, lastSlash);
                }
            }
        }
        return "";
    }

    std::string WvWAPI::GetDataDirectory() {
        std::string dir = GetDllDir();
        if (!dir.empty()) {
            std::replace(dir.begin(), dir.end(), '\\', '/');
        }
        return dir + "/RealmReport";
    }

    static bool EnsureDataDirectory() {
        std::string dir = WvWAPI::GetDataDirectory();
        try {
            std::filesystem::create_directories(dir);
            return true;
        } catch (...) {
            return false;
        }
    }

    // --- HTTP GET using WinINet ---

    static std::string CacheBust(const std::string& url) {
        char sep = (url.find('?') == std::string::npos) ? '?' : '&';
        return url + sep + "_cb=" + std::to_string(GetTickCount64());
    }

    std::string WvWAPI::HttpGet(const std::string& url) {
        HINTERNET hInternet = InternetOpenA("RealmReport/1.0",
            INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
        if (!hInternet) return "";

        DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE |
                      INTERNET_FLAG_SECURE | INTERNET_FLAG_IGNORE_CERT_CN_INVALID;

        HINTERNET hUrl = InternetOpenUrlA(hInternet, url.c_str(), NULL, 0, flags, 0);
        if (!hUrl) {
            InternetCloseHandle(hInternet);
            return "";
        }

        std::string result;
        char buffer[8192];
        DWORD bytesRead;
        while (InternetReadFile(hUrl, buffer, sizeof(buffer), &bytesRead) && bytesRead > 0) {
            result.append(buffer, bytesRead);
        }

        InternetCloseHandle(hUrl);
        InternetCloseHandle(hInternet);
        return result;
    }

    // --- Initialize / Shutdown ---

    void WvWAPI::Initialize() {
        s_shutdown.store(false);
        s_polling.store(false);
        s_has_match_data.store(false);
        s_worlds_ready.store(false);
        s_objectives_cached.store(false);
        s_fetch_now.store(false);
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            s_status_message = "Initializing...";
            s_is_error.store(false);
        }
    }

    void WvWAPI::Shutdown() {
        s_shutdown.store(true);
        s_polling.store(false);
    }

    // --- World Restructuring team names (not available via API) ---

    static std::vector<World> GetWRTeams() {
        return {
            // NA teams (11xxx)
            {11001, "Moogooloo"},
            {11002, "Rall's Rest"},
            {11003, "Domain of Torment"},
            {11004, "Yohlon Haven"},
            {11005, "Tombs of Drascir"},
            {11006, "Hall of Judgment"},
            {11007, "Throne of Balthazar"},
            {11008, "Dwayna's Temple"},
            {11009, "Abaddon's Prison"},
            {11010, "Cathedral of Blood"},
            {11011, "Lutgardis Conservatory"},
            {11012, "Mosswood"},
            // EU teams (12xxx)
            {12001, "Skrittsburgh"},
            {12002, "Fortune's Vale"},
            {12003, "Silent Woods"},
            {12004, "Ettin's Back"},
            {12005, "Domain of Anguish"},
            {12006, "Palawadan"},
            {12007, "Bloodstone Gulch"},
            {12008, "Frost Citadel"},
            {12009, "Dragrimmar"},
            {12010, "Grenth's Door"},
            {12011, "Mirror of Lyssa"},
            {12012, "Melandru's Dome"},
            {12013, "Kormir's Library"},
            {12014, "Great House Aviary"},
            {12015, "Bava Nisos"},
        };
    }

    // --- World List ---

    void WvWAPI::FetchWorldListAsync() {
        std::thread([]() {
            {
                std::lock_guard<std::mutex> lock(s_mutex);
                s_status_message = "Fetching world list...";
            }

            std::string response = HttpGet(CacheBust("https://api.guildwars2.com/v2/worlds?ids=all"));
            if (response.empty()) {
                std::lock_guard<std::mutex> lock(s_mutex);
                s_status_message = "Failed to fetch world list";
                s_is_error.store(true);
                return;
            }

            try {
                json j = json::parse(response);
                if (!j.is_array()) {
                    std::lock_guard<std::mutex> lock(s_mutex);
                    s_status_message = "Invalid world list response";
                    s_is_error.store(true);
                    return;
                }

                std::vector<World> worlds;
                for (const auto& w : j) {
                    World world;
                    world.id = w.value("id", 0);
                    world.name = w.value("name", "");
                    if (world.id > 0 && !world.name.empty()) {
                        worlds.push_back(world);
                    }
                }

                // Append WR teams
                auto wr_teams = GetWRTeams();
                worlds.insert(worlds.end(), wr_teams.begin(), wr_teams.end());

                // Sort by name
                std::sort(worlds.begin(), worlds.end(),
                    [](const World& a, const World& b) { return a.name < b.name; });

                {
                    std::lock_guard<std::mutex> lock(s_mutex);
                    s_worlds = std::move(worlds);
                    s_status_message = "World list loaded (" + std::to_string(s_worlds.size()) + " worlds)";
                    s_is_error.store(false);
                }
                s_worlds_ready.store(true);

            } catch (const std::exception& e) {
                std::lock_guard<std::mutex> lock(s_mutex);
                s_status_message = std::string("Error parsing world list: ") + e.what();
                s_is_error.store(true);
            }
        }).detach();
    }

    const std::vector<World>& WvWAPI::GetWorlds() {
        return s_worlds;
    }

    bool WvWAPI::IsWorldListReady() {
        return s_worlds_ready.load();
    }

    // --- World Selection Persistence ---

    void WvWAPI::SetSelectedWorld(int world_id) {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_selected_world = world_id;
    }

    int WvWAPI::GetSelectedWorld() {
        std::lock_guard<std::mutex> lock(s_mutex);
        return s_selected_world;
    }

    void WvWAPI::SetFlipNotifications(bool enabled) {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_flip_notifications = enabled;
    }

    bool WvWAPI::GetFlipNotifications() {
        std::lock_guard<std::mutex> lock(s_mutex);
        return s_flip_notifications;
    }

    void WvWAPI::SetSortState(int column, bool ascending) {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_sort_column = column;
        s_sort_ascending = ascending;
    }

    void WvWAPI::GetSortState(int& column, bool& ascending) {
        std::lock_guard<std::mutex> lock(s_mutex);
        column = s_sort_column;
        ascending = s_sort_ascending;
    }

    void WvWAPI::SetToastDuration(float seconds) {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_toast_duration = seconds;
    }

    float WvWAPI::GetToastDuration() {
        std::lock_guard<std::mutex> lock(s_mutex);
        return s_toast_duration;
    }

    void WvWAPI::SetFlipSound(bool enabled, const std::string& filename) {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_flip_sound = enabled;
        s_flip_sound_path = filename;
    }

    bool WvWAPI::GetFlipSoundEnabled() {
        std::lock_guard<std::mutex> lock(s_mutex);
        return s_flip_sound;
    }

    std::string WvWAPI::GetFlipSoundFile() {
        std::lock_guard<std::mutex> lock(s_mutex);
        return s_flip_sound_path;
    }

    std::string WvWAPI::GetSoundsDirectory() {
        return GetDataDirectory() + "/sounds";
    }

    std::vector<std::string> WvWAPI::ScanSoundFiles() {
        std::vector<std::string> files;
        std::string dir = GetSoundsDirectory();
        try {
            if (!std::filesystem::exists(dir)) {
                std::filesystem::create_directories(dir);
            }
            for (const auto& entry : std::filesystem::directory_iterator(dir)) {
                if (!entry.is_regular_file()) continue;
                std::string ext = entry.path().extension().string();
                // lowercase compare
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext == ".wav" || ext == ".mp3") {
                    files.push_back(entry.path().filename().string());
                }
            }
        } catch (...) {}
        std::sort(files.begin(), files.end());
        return files;
    }

    void WvWAPI::SetFlatList(bool enabled) {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_flat_list = enabled;
    }

    bool WvWAPI::GetFlatList() {
        std::lock_guard<std::mutex> lock(s_mutex);
        return s_flat_list;
    }

    void WvWAPI::SetToastLayout(float ax, float ay, float w, float h) {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_toast_anchor_x = ax;
        s_toast_anchor_y = ay;
        s_toast_w = w;
        s_toast_h = h;
    }

    void WvWAPI::GetToastLayout(float& ax, float& ay, float& w, float& h) {
        std::lock_guard<std::mutex> lock(s_mutex);
        ax = s_toast_anchor_x;
        ay = s_toast_anchor_y;
        w = s_toast_w;
        h = s_toast_h;
    }

    void WvWAPI::SaveSelectedWorld() {
        EnsureDataDirectory();
        std::string path = GetDataDirectory() + "/settings.json";
        json j;
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            j["selected_world"] = s_selected_world;
            j["poll_interval"] = s_poll_interval;
            j["flip_notifications"] = s_flip_notifications;
            j["toast_anchor_x"] = s_toast_anchor_x;
            j["toast_anchor_y"] = s_toast_anchor_y;
            j["toast_w"] = s_toast_w;
            j["toast_h"] = s_toast_h;
            j["flat_list"] = s_flat_list;
            j["toast_duration"] = s_toast_duration;
            j["flip_sound"] = s_flip_sound;
            j["flip_sound_path"] = s_flip_sound_path;
            j["sort_column"] = s_sort_column;
            j["sort_ascending"] = s_sort_ascending;
        }
        std::ofstream file(path);
        if (file.is_open()) {
            file << j.dump(2);
            file.flush();
        }
    }

    bool WvWAPI::LoadSelectedWorld() {
        std::string path = GetDataDirectory() + "/settings.json";
        std::ifstream file(path);
        if (!file.is_open()) return false;

        try {
            json j;
            file >> j;
            std::lock_guard<std::mutex> lock(s_mutex);
            if (j.contains("selected_world")) {
                s_selected_world = j["selected_world"].get<int>();
            }
            if (j.contains("poll_interval")) {
                s_poll_interval = j["poll_interval"].get<int>();
                if (s_poll_interval < 20) s_poll_interval = 20;
                if (s_poll_interval > 120) s_poll_interval = 120;
            }
            if (j.contains("flip_notifications")) {
                s_flip_notifications = j["flip_notifications"].get<bool>();
            }
            if (j.contains("toast_anchor_x")) s_toast_anchor_x = j["toast_anchor_x"].get<float>();
            if (j.contains("toast_anchor_y")) s_toast_anchor_y = j["toast_anchor_y"].get<float>();
            if (j.contains("toast_w")) s_toast_w = j["toast_w"].get<float>();
            if (j.contains("toast_h")) s_toast_h = j["toast_h"].get<float>();
            if (j.contains("flat_list")) s_flat_list = j["flat_list"].get<bool>();
            if (j.contains("flip_sound")) s_flip_sound = j["flip_sound"].get<bool>();
            if (j.contains("flip_sound_path")) s_flip_sound_path = j["flip_sound_path"].get<std::string>();
            if (j.contains("toast_duration")) {
                s_toast_duration = j["toast_duration"].get<float>();
                if (s_toast_duration < 2.0f) s_toast_duration = 2.0f;
                if (s_toast_duration > 15.0f) s_toast_duration = 15.0f;
            }
            if (j.contains("sort_column")) s_sort_column = j["sort_column"].get<int>();
            if (j.contains("sort_ascending")) s_sort_ascending = j["sort_ascending"].get<bool>();
            return s_selected_world > 0;
        } catch (...) {
            return false;
        }
    }

    // --- Objective Name Resolution ---

    void WvWAPI::FetchObjectiveNames() {
        if (s_objectives_cached.load()) return;

        std::string response = HttpGet(CacheBust("https://api.guildwars2.com/v2/wvw/objectives?ids=all"));
        if (response.empty()) return;

        try {
            json j = json::parse(response);
            if (!j.is_array()) return;

            std::lock_guard<std::mutex> lock(s_mutex);
            for (const auto& obj : j) {
                std::string id = obj.value("id", "");
                std::string name = obj.value("name", "");
                std::string type = obj.value("type", "");
                if (!id.empty()) {
                    if (!name.empty()) s_objective_names[id] = name;
                    if (!type.empty()) s_objective_types[id] = type;
                }
            }
            s_objectives_cached.store(true);
        } catch (...) {}
    }

    // --- Guild Name Resolution ---

    void WvWAPI::FetchGuildName(const std::string& guild_id) {
        if (guild_id.empty()) return;

        // Check cache
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            if (s_guild_names.find(guild_id) != s_guild_names.end()) return;
        }

        std::string response = HttpGet(CacheBust("https://api.guildwars2.com/v2/guild/" + guild_id));
        if (response.empty()) return;

        try {
            json j = json::parse(response);
            std::string name = j.value("name", "");
            std::string tag = j.value("tag", "");
            std::string display = name;
            if (!tag.empty()) {
                display = "[" + tag + "] " + name;
            }
            std::lock_guard<std::mutex> lock(s_mutex);
            s_guild_names[guild_id] = display;
        } catch (...) {
            std::lock_guard<std::mutex> lock(s_mutex);
            s_guild_names[guild_id] = guild_id.substr(0, 8) + "...";
        }
    }

    // --- Parse ISO-8601 to seconds ago ---

    static int ParseTimeSinceFlip(const std::string& iso_time) {
        if (iso_time.empty()) return -1;

        // Parse ISO-8601: "2024-03-15T10:30:00Z"
        struct tm tm_val = {};
        // Try parsing with sscanf for portability
        int year, month, day, hour, minute, second;
        if (sscanf(iso_time.c_str(), "%d-%d-%dT%d:%d:%dZ",
                   &year, &month, &day, &hour, &minute, &second) == 6) {
            tm_val.tm_year = year - 1900;
            tm_val.tm_mon = month - 1;
            tm_val.tm_mday = day;
            tm_val.tm_hour = hour;
            tm_val.tm_min = minute;
            tm_val.tm_sec = second;
            tm_val.tm_isdst = 0;

            // Convert to UTC time_t
            // _mkgmtime on Windows (mingw)
            time_t flip_time = _mkgmtime(&tm_val);
            if (flip_time == (time_t)-1) return -1;

            time_t now;
            time(&now);
            // Get current UTC time
            struct tm* utc_now = gmtime(&now);
            time_t now_utc = _mkgmtime(utc_now);

            int diff = (int)difftime(now_utc, flip_time);
            return (diff >= 0) ? diff : 0;
        }
        return -1;
    }

    // --- Resolve objectives within match data ---

    void WvWAPI::ResolveObjectives(MatchData& match) {
        std::lock_guard<std::mutex> lock(s_mutex);
        for (auto& map : match.maps) {
            for (auto& obj : map.objectives) {
                // Resolve name
                auto it = s_objective_names.find(obj.id);
                if (it != s_objective_names.end()) {
                    obj.name = it->second;
                }
                // Resolve type from cache if not set from match data
                if (obj.type == ObjectiveType::Unknown) {
                    auto type_it = s_objective_types.find(obj.id);
                    if (type_it != s_objective_types.end()) {
                        obj.type = StringToObjectiveType(type_it->second);
                    }
                }
                // Resolve guild name
                if (!obj.claimed_by.empty()) {
                    auto guild_it = s_guild_names.find(obj.claimed_by);
                    if (guild_it != s_guild_names.end()) {
                        obj.claimed_by = guild_it->second;
                    }
                }
                // Compute derived fields
                obj.seconds_since_flip = ParseTimeSinceFlip(obj.last_flipped);
                obj.upgrade_tier = ComputeUpgradeTier(obj.yaks_delivered);
            }
        }
    }

    // --- Map type to display name ---

    static std::string MapTypeToDisplayName(const std::string& type) {
        if (type == "Center")    return "Eternal Battlegrounds";
        if (type == "RedHome")   return "Red Borderlands";
        if (type == "BlueHome")  return "Blue Borderlands";
        if (type == "GreenHome") return "Green Borderlands";
        return type;
    }

    // --- Flip Detection ---

    static const char* MapTypeToShortName(const std::string& type) {
        if (type == "Center")    return "EBG";
        if (type == "RedHome")   return "Red BL";
        if (type == "BlueHome")  return "Blue BL";
        if (type == "GreenHome") return "Green BL";
        return "???";
    }

    void WvWAPI::DetectFlips(const MatchData& old_data, const MatchData& new_data) {
        if (old_data.id.empty() || old_data.id != new_data.id) return;

        // Build lookup: obj_id -> owner from old data
        std::unordered_map<std::string, TeamColor> old_owners;
        std::unordered_map<std::string, std::string> obj_map_types;
        for (const auto& map : old_data.maps) {
            for (const auto& obj : map.objectives) {
                old_owners[obj.id] = obj.owner;
                obj_map_types[obj.id] = map.type;
            }
        }

        // Compare with new data
        auto now = std::chrono::steady_clock::now();
        std::vector<FlipEvent> new_flips;

        for (const auto& map : new_data.maps) {
            for (const auto& obj : map.objectives) {
                auto it = old_owners.find(obj.id);
                if (it == old_owners.end()) continue;
                if (it->second == obj.owner) continue;
                if (obj.name.empty()) continue;
                // Skip spawns/ruins/mercenary
                if (obj.type == ObjectiveType::Spawn ||
                    obj.type == ObjectiveType::Ruins ||
                    obj.type == ObjectiveType::Mercenary ||
                    obj.type == ObjectiveType::Unknown) continue;

                FlipEvent fe;
                fe.obj_name = obj.name;
                fe.obj_id = obj.id;
                fe.type = obj.type;
                fe.map_type = map.type;
                fe.map_display = MapTypeToShortName(map.type);
                fe.old_owner = it->second;
                fe.new_owner = obj.owner;
                fe.timestamp = now;
                new_flips.push_back(fe);
            }
        }

        if (!new_flips.empty()) {
            std::lock_guard<std::mutex> lock(s_mutex);
            s_flip_events.insert(s_flip_events.end(), new_flips.begin(), new_flips.end());
        }
    }

    std::vector<FlipEvent> WvWAPI::PopFlipEvents() {
        std::lock_guard<std::mutex> lock(s_mutex);
        std::vector<FlipEvent> out;
        out.swap(s_flip_events);
        return out;
    }

    // --- Fetch Match Data ---

    void WvWAPI::FetchMatchData() {
        int world_id;
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            world_id = s_selected_world;
        }

        if (world_id <= 0) {
            std::lock_guard<std::mutex> lock(s_mutex);
            s_status_message = "No world selected";
            return;
        }

        // Ensure objective names are cached
        FetchObjectiveNames();

        {
            std::lock_guard<std::mutex> lock(s_mutex);
            s_status_message = "Fetching match data...";
            s_is_error.store(false);
        }

        std::string url = CacheBust("https://api.guildwars2.com/v2/wvw/matches?world=" + std::to_string(world_id));
        std::string response = HttpGet(url);

        if (response.empty()) {
            std::lock_guard<std::mutex> lock(s_mutex);
            s_status_message = "Failed to fetch match data (no response)";
            s_is_error.store(true);
            return;
        }

        try {
            json j = json::parse(response);

            // Check for API error
            if (j.is_object() && j.contains("text")) {
                std::lock_guard<std::mutex> lock(s_mutex);
                s_status_message = "API error: " + j["text"].get<std::string>();
                s_is_error.store(true);
                return;
            }

            MatchData match;
            match.id = j.value("id", "");
            match.start_time = j.value("start_time", "");
            match.end_time = j.value("end_time", "");

            // Scores
            if (j.contains("scores")) {
                match.score_red = j["scores"].value("red", 0);
                match.score_green = j["scores"].value("green", 0);
                match.score_blue = j["scores"].value("blue", 0);
            }

            // Resolve world names for the three teams
            if (j.contains("worlds")) {
                auto resolve_world = [](int wid) -> std::string {
                    std::lock_guard<std::mutex> lock(s_mutex);
                    for (const auto& w : s_worlds) {
                        if (w.id == wid) return w.name;
                    }
                    return std::to_string(wid);
                };
                match.world_red = resolve_world(j["worlds"].value("red", 0));
                match.world_green = resolve_world(j["worlds"].value("green", 0));
                match.world_blue = resolve_world(j["worlds"].value("blue", 0));
            }

            // Collect guild IDs to resolve
            std::vector<std::string> guild_ids_to_fetch;

            // Parse maps
            if (j.contains("maps") && j["maps"].is_array()) {
                for (const auto& map_json : j["maps"]) {
                    WvWMap wvw_map;
                    wvw_map.type = map_json.value("type", "");
                    wvw_map.display_name = MapTypeToDisplayName(wvw_map.type);

                    if (map_json.contains("objectives") && map_json["objectives"].is_array()) {
                        for (const auto& obj_json : map_json["objectives"]) {
                            Objective obj;
                            obj.id = obj_json.value("id", "");
                            obj.owner = StringToTeamColor(obj_json.value("owner", "Neutral"));
                            obj.last_flipped = obj_json.value("last_flipped", "");
                            obj.yaks_delivered = obj_json.value("yaks_delivered", 0);
                            obj.points_tick = obj_json.value("points_tick", 0);
                            obj.points_capture = obj_json.value("points_capture", 0);

                            // Type from match data
                            std::string type_str = obj_json.value("type", "");
                            obj.type = StringToObjectiveType(type_str);

                            // Guild claim
                            if (obj_json.contains("claimed_by") && !obj_json["claimed_by"].is_null()) {
                                obj.claimed_by = obj_json["claimed_by"].get<std::string>();
                                // Queue for resolution
                                guild_ids_to_fetch.push_back(obj.claimed_by);
                            }

                            wvw_map.objectives.push_back(obj);
                        }
                    }
                    match.maps.push_back(wvw_map);
                }
            }

            // Fetch guild names (deduplicated)
            {
                std::sort(guild_ids_to_fetch.begin(), guild_ids_to_fetch.end());
                guild_ids_to_fetch.erase(
                    std::unique(guild_ids_to_fetch.begin(), guild_ids_to_fetch.end()),
                    guild_ids_to_fetch.end());
                for (const auto& gid : guild_ids_to_fetch) {
                    FetchGuildName(gid);
                }
            }

            // Resolve names and compute derived fields
            ResolveObjectives(match);

            match.last_updated = std::chrono::steady_clock::now();

            // Detect flips before overwriting old data
            if (s_has_match_data.load()) {
                MatchData old_data;
                {
                    std::lock_guard<std::mutex> lock(s_mutex);
                    old_data = s_match_data;
                }
                DetectFlips(old_data, match);
            }

            {
                std::lock_guard<std::mutex> lock(s_mutex);
                s_match_data = match;
                s_has_match_data.store(true);

                s_last_fetch_time = std::chrono::steady_clock::now();
                s_status_message = "Ready";
                s_is_error.store(false);
            }

        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lock(s_mutex);
            s_status_message = std::string("Parse error: ") + e.what();
            s_is_error.store(true);
        }
    }

    // --- Background Polling ---

    void WvWAPI::PollWorker() {
        int backoff_multiplier = 1;
        while (!s_shutdown.load()) {
            int world_id;
            {
                std::lock_guard<std::mutex> lock(s_mutex);
                world_id = s_selected_world;
            }

            if (world_id > 0) {
                FetchMatchData();
            }

            // Wait for poll interval, with exponential backoff on failure
            // If the last fetch failed, ignore fetch_now to avoid hammering the API
            bool had_error = s_is_error.load();
            if (had_error) {
                backoff_multiplier = (std::min)(backoff_multiplier * 2, 8);
            } else {
                backoff_multiplier = 1;
            }
            int wait_ms;
            {
                std::lock_guard<std::mutex> lock(s_mutex);
                wait_ms = s_poll_interval * 1000 * backoff_multiplier;
            }
            int elapsed = 0;
            while (elapsed < wait_ms && !s_shutdown.load()) {
                if (!had_error && s_fetch_now.load()) break;
                Sleep(500);
                elapsed += 500;
            }

            if (s_fetch_now.load()) {
                s_fetch_now.store(false);
            }
        }
    }

    void WvWAPI::StartPolling() {
        if (s_polling.load()) return;
        s_polling.store(true);
        s_shutdown.store(false);
        std::thread(PollWorker).detach();
    }

    void WvWAPI::StopPolling() {
        s_shutdown.store(true);
        s_polling.store(false);
    }

    bool WvWAPI::IsPolling() {
        return s_polling.load();
    }

    void WvWAPI::FetchNow() {
        s_fetch_now.store(true);
    }

    // --- Accessors ---

    MatchData WvWAPI::GetMatchData() {
        std::lock_guard<std::mutex> lock(s_mutex);
        return s_match_data;
    }

    bool WvWAPI::HasMatchData() {
        return s_has_match_data.load();
    }

    const std::string& WvWAPI::GetStatusMessage() {
        return s_status_message;
    }

    bool WvWAPI::IsError() {
        return s_is_error.load();
    }

    void WvWAPI::SetPollIntervalSeconds(int seconds) {
        if (seconds < 20) seconds = 20;
        if (seconds > 120) seconds = 120;
        std::lock_guard<std::mutex> lock(s_mutex);
        s_poll_interval = seconds;
    }

    int WvWAPI::GetPollIntervalSeconds() {
        std::lock_guard<std::mutex> lock(s_mutex);
        return s_poll_interval;
    }

    int WvWAPI::GetSecondsUntilNextRefresh() {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_last_fetch_time.time_since_epoch().count() == 0) return 0;
        auto elapsed = std::chrono::steady_clock::now() - s_last_fetch_time;
        int elapsed_s = (int)std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
        int remaining = s_poll_interval - elapsed_s;
        return remaining > 0 ? remaining : 0;
    }

} // namespace RealmReport
