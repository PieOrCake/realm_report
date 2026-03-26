#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <chrono>

namespace RealmReport {

    // A GW2 world (server)
    struct World {
        int id = 0;
        std::string name;
    };

    // Objective types that matter for commanders
    enum class ObjectiveType {
        Camp,
        Tower,
        Keep,
        Castle,
        Spawn,
        Ruins,
        Mercenary,
        Unknown
    };

    // Team color
    enum class TeamColor {
        Red,
        Green,
        Blue,
        Neutral,
        Unknown
    };

    // A single WvW objective on a map
    struct Objective {
        std::string id;              // e.g. "38-6"
        std::string name;            // resolved from /v2/wvw/objectives
        ObjectiveType type = ObjectiveType::Unknown;
        TeamColor owner = TeamColor::Neutral;
        std::string last_flipped;    // ISO-8601 timestamp
        int seconds_since_flip = 0;  // computed
        int yaks_delivered = 0;
        std::string claimed_by;      // guild name (resolved) or empty
        int points_tick = 0;
        int points_capture = 0;
        int upgrade_tier = 0;        // derived from yaks_delivered
    };

    // A WvW map within a match
    struct WvWMap {
        std::string type;            // "Center", "RedHome", "BlueHome", "GreenHome"
        std::string display_name;    // "Eternal Battlegrounds", "Red Borderlands", etc.
        std::vector<Objective> objectives;
    };

    // A full match snapshot
    struct MatchData {
        std::string id;
        std::string start_time;
        std::string end_time;
        // Scores per team
        int score_red = 0;
        int score_green = 0;
        int score_blue = 0;
        // World names per team color
        std::string world_red;
        std::string world_green;
        std::string world_blue;
        // Maps
        std::vector<WvWMap> maps;
        // Timestamp of last successful fetch
        std::chrono::steady_clock::time_point last_updated;
    };

    // A detected objective flip (owner change)
    struct FlipEvent {
        std::string obj_name;
        std::string obj_id;
        ObjectiveType type;
        std::string map_type;       // "Center", "RedHome", etc.
        std::string map_display;    // "EBG", "Red BL", etc.
        TeamColor old_owner;
        TeamColor new_owner;
        std::chrono::steady_clock::time_point timestamp;
    };

    // Sort column for the objectives table
    enum class SortColumn {
        Map,
        Name,
        Type,
        Owner,
        TimeSinceFlip,
        YaksDelivered,
        ClaimedBy,
        PPT
    };

    class WvWAPI {
    public:
        // Initialize / shutdown
        static void Initialize();
        static void Shutdown();

        // World list
        static void FetchWorldListAsync();
        static const std::vector<World>& GetWorlds();
        static bool IsWorldListReady();

        // World selection
        static void SetSelectedWorld(int world_id);
        static int GetSelectedWorld();
        static void SaveSelectedWorld();
        static bool LoadSelectedWorld();
        static std::string GetDataDirectory();

        // Match data polling
        static void StartPolling();
        static void StopPolling();
        static bool IsPolling();

        // Get current match data (thread-safe copy)
        static MatchData GetMatchData();
        static bool HasMatchData();

        // Manually trigger a fetch (for immediate refresh)
        static void FetchNow();

        // Status
        static const std::string& GetStatusMessage();
        static bool IsError();

        // Polling interval
        static void SetPollIntervalSeconds(int seconds);
        static int GetPollIntervalSeconds();
        static int GetSecondsUntilNextRefresh();

        // Flip notifications setting
        static void SetFlipNotifications(bool enabled);
        static bool GetFlipNotifications();

        // Sort state
        static void SetSortState(int column, bool ascending);
        static void GetSortState(int& column, bool& ascending);

        // Display mode
        static void SetFlatList(bool enabled);
        static bool GetFlatList();

        // Toast duration
        static void SetToastDuration(float seconds);
        static float GetToastDuration();

        // Flip sound
        static void SetFlipSound(bool enabled, const std::string& filename);
        static bool GetFlipSoundEnabled();
        static std::string GetFlipSoundFile();
        static std::string GetSoundsDirectory();
        static std::vector<std::string> ScanSoundFiles();

        // Toast position/size
        static void SetToastLayout(float ax, float ay, float w, float h);
        static void GetToastLayout(float& ax, float& ay, float& w, float& h);

        // Flip events
        static std::vector<FlipEvent> PopFlipEvents();

    private:
        static void DetectFlips(const MatchData& old_data, const MatchData& new_data);
        // HTTP helper
        static std::string HttpGet(const std::string& url);

        // Background worker
        static void PollWorker();
        static void FetchMatchData();
        static void FetchObjectiveNames();
        static void FetchGuildName(const std::string& guild_id);
        static void ResolveObjectives(MatchData& match);

        // Objective name cache: objective_id -> name
        static std::unordered_map<std::string, std::string> s_objective_names;
        // Objective type cache: objective_id -> type string
        static std::unordered_map<std::string, std::string> s_objective_types;
        // Guild name cache: guild_id -> guild name/tag
        static std::unordered_map<std::string, std::string> s_guild_names;

        static std::vector<World> s_worlds;
        static std::atomic<bool> s_worlds_ready;
        static int s_selected_world;
        static MatchData s_match_data;
        static std::atomic<bool> s_has_match_data;
        static std::string s_status_message;
        static std::atomic<bool> s_is_error;
        static std::atomic<bool> s_polling;
        static std::atomic<bool> s_shutdown;
        static std::atomic<bool> s_fetch_now;
        static int s_poll_interval;
        static std::chrono::steady_clock::time_point s_last_fetch_time;
        static std::mutex s_mutex;
        static std::atomic<bool> s_objectives_cached;
        static std::vector<FlipEvent> s_flip_events;
        static bool s_flip_notifications;
        static float s_toast_anchor_x;
        static float s_toast_anchor_y;
        static float s_toast_w;
        static float s_toast_h;
        static bool s_flat_list;
        static float s_toast_duration;
        static bool s_flip_sound;
        static std::string s_flip_sound_path;
        static int s_sort_column;
        static bool s_sort_ascending;
    };

    // Utility
    const char* ObjectiveTypeToString(ObjectiveType type);
    const char* TeamColorToString(TeamColor color);
    ObjectiveType StringToObjectiveType(const std::string& s);
    TeamColor StringToTeamColor(const std::string& s);
    int ComputeUpgradeTier(int yaks_delivered);

} // namespace RealmReport
