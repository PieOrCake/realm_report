#pragma once
#include <string>
#include <unordered_map>
#include <queue>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include "nexus/Nexus.h"

struct TileKey {
    int z, x, y;
    bool operator==(const TileKey& o) const { return z == o.z && x == o.x && y == o.y; }
};

struct TileKeyHash {
    size_t operator()(const TileKey& k) const {
        size_t h = std::hash<int>{}(k.z);
        h ^= std::hash<int>{}(k.x) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<int>{}(k.y) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

class TileCache {
public:
    void Init(AddonAPI_t* api, const std::string& dataDir);
    void Shutdown();

    // Call from render thread each frame — marks newly-downloaded tiles as available
    void ProcessReadyQueue();

    // Returns GPU texture or nullptr (enqueues download in background if not on disk)
    Texture_t* GetTile(int z, int x, int y);

    // Enqueue all tiles covering the given continent rect at zoom levels 4..7
    // Safe to call from the render thread.
    void PrefetchRegion(float cont_minX, float cont_minY,
                        float cont_maxX, float cont_maxY);

private:
    void DownloadWorker();
    std::string TilePath(int z, int x, int y) const;
    std::string TileId(int z, int x, int y) const;
    static void TileXY(int z, float cx, float cy, int& tx, int& ty);
    bool DownloadTile(const TileKey& key);

    AddonAPI_t* m_api    = nullptr;
    std::string m_dataDir;  // full path including "map_tiles" subdir

    // Render-thread-only state (no locking needed)
    std::unordered_map<TileKey, Texture_t*, TileKeyHash> m_textures;
    std::unordered_map<TileKey, bool, TileKeyHash>        m_onDisk;
    std::unordered_map<TileKey, bool, TileKeyHash>        m_requested;

    // Ready queue: download thread writes, render thread reads in ProcessReadyQueue
    std::mutex            m_readyMu;
    std::queue<TileKey>   m_readyQueue;

    // Download queue: render thread writes, download thread reads
    std::mutex              m_dlMu;
    std::condition_variable m_dlCv;
    std::queue<TileKey>     m_dlQueue;
    bool                    m_running = false;
    std::thread             m_worker;

    // Active WinINet session handle — Shutdown() closes it to interrupt a live download.
    // Atomic void* so Shutdown and DownloadTile can exchange it without a separate mutex.
    std::atomic<void*>      m_activeSession{nullptr};
};
