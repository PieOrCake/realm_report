#include "TileCache.h"
#include <windows.h>
#include <wininet.h>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <thread>
#include <cmath>
#include <vector>

// continent coordinate → tile index at zoom level z
// GW2 continent coords span 0..32768 at the base level.
void TileCache::TileXY(int z, float cx, float cy, int& tx, int& ty) {
    float span = 32768.0f / (float)(1 << z);
    tx = (int)floorf(cx / span);
    ty = (int)floorf(cy / span);
}

std::string TileCache::TileId(int z, int x, int y) const {
    return "RRTILE_" + std::to_string(z) + "_" + std::to_string(x) + "_" + std::to_string(y);
}

std::string TileCache::TilePath(int z, int x, int y) const {
    return m_dataDir + "/" + std::to_string(z) + "_" + std::to_string(x) + "_" + std::to_string(y) + ".jpg";
}

void TileCache::Init(AddonAPI_t* api, const std::string& dataDir) {
    m_api     = api;
    m_dataDir = dataDir + "/map_tiles";
    try { std::filesystem::create_directories(m_dataDir); } catch (...) {}

    m_running = true;
    m_worker  = std::thread(&TileCache::DownloadWorker, this);
}

void TileCache::Shutdown() {
    {
        std::lock_guard<std::mutex> lk(m_dlMu);
        m_running = false;
    }
    m_dlCv.notify_all();
    if (m_worker.joinable()) m_worker.join();
}

void TileCache::ProcessReadyQueue() {
    std::lock_guard<std::mutex> lk(m_readyMu);
    while (!m_readyQueue.empty()) {
        m_onDisk[m_readyQueue.front()] = true;
        m_readyQueue.pop();
    }
}

Texture_t* TileCache::GetTile(int z, int x, int y) {
    TileKey key{z, x, y};

    // Already have a loaded texture?
    auto texIt = m_textures.find(key);
    if (texIt != m_textures.end()) return texIt->second;

    // File confirmed on disk (from ProcessReadyQueue or prior check)?
    if (m_onDisk.count(key) && m_onDisk[key]) {
        std::string path = TilePath(z, x, y);
        std::string id   = TileId(z, x, y);
        Texture_t* tex   = m_api->Textures_GetOrCreateFromFile(id.c_str(), path.c_str());
        if (tex) {
            m_textures[key] = tex;
            return tex;
        }
        // File may have been deleted; reset so we re-download
        m_onDisk[key]    = false;
        m_requested[key] = false;
    }

    // Not yet requested — check disk first, then enqueue download
    if (!m_requested.count(key) || !m_requested[key]) {
        m_requested[key] = true;
        std::string path = TilePath(z, x, y);
        if (std::filesystem::exists(path)) {
            m_onDisk[key] = true;
            // Next call will hit the m_onDisk branch above
        } else {
            {
                std::lock_guard<std::mutex> lk(m_dlMu);
                m_dlQueue.push(key);
            }
            m_dlCv.notify_one();
        }
    }

    return nullptr;
}

void TileCache::PrefetchRegion(float minX, float minY, float maxX, float maxY) {
    // Render-thread-only writes to m_requested / m_onDisk; no lock needed for those.
    // Lock m_dlMu only when pushing to m_dlQueue.
    std::vector<TileKey> toEnqueue;

    for (int z = 4; z <= 7; ++z) {
        int tx_min, ty_min, tx_max, ty_max;
        TileXY(z, minX, minY, tx_min, ty_min);
        TileXY(z, maxX, maxY, tx_max, ty_max);
        if (tx_min > tx_max) std::swap(tx_min, tx_max);
        if (ty_min > ty_max) std::swap(ty_min, ty_max);

        for (int tx = tx_min; tx <= tx_max; ++tx) {
            for (int ty = ty_min; ty <= ty_max; ++ty) {
                TileKey key{z, tx, ty};
                if (m_requested.count(key) && m_requested[key]) continue;
                m_requested[key] = true;

                if (std::filesystem::exists(TilePath(z, tx, ty))) {
                    m_onDisk[key] = true;
                } else {
                    toEnqueue.push_back(key);
                }
            }
        }
    }

    if (!toEnqueue.empty()) {
        std::lock_guard<std::mutex> lk(m_dlMu);
        for (auto& k : toEnqueue) m_dlQueue.push(k);
        m_dlCv.notify_all();
    }
}

void TileCache::DownloadWorker() {
    while (true) {
        TileKey key;
        {
            std::unique_lock<std::mutex> lk(m_dlMu);
            m_dlCv.wait(lk, [this]{ return !m_dlQueue.empty() || !m_running; });
            if (!m_running && m_dlQueue.empty()) break;
            key = m_dlQueue.front();
            m_dlQueue.pop();
        }

        if (DownloadTile(key)) {
            std::lock_guard<std::mutex> lk(m_readyMu);
            m_readyQueue.push(key);
        }

        // Rate limit: 100ms between downloads
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

bool TileCache::DownloadTile(const TileKey& key) {
    // URL: https://tiles.guildwars2.com/2/3/{z}/{x}/{y}.jpg
    std::string path = "/2/3/" + std::to_string(key.z) + "/" +
                       std::to_string(key.x) + "/" +
                       std::to_string(key.y) + ".jpg";
    std::string url  = "https://tiles.guildwars2.com" + path;

    HINTERNET hInternet = InternetOpenA("RealmReport/1.0",
        INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!hInternet) return false;

    DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE |
                  INTERNET_FLAG_SECURE | INTERNET_FLAG_IGNORE_CERT_CN_INVALID;
    HINTERNET hUrl = InternetOpenUrlA(hInternet, url.c_str(), NULL, 0, flags, 0);
    if (!hUrl) {
        InternetCloseHandle(hInternet);
        return false;
    }

    std::string data;
    char buffer[8192];
    DWORD bytesRead = 0;
    while (InternetReadFile(hUrl, buffer, sizeof(buffer), &bytesRead) && bytesRead > 0) {
        data.append(buffer, bytesRead);
    }

    InternetCloseHandle(hUrl);
    InternetCloseHandle(hInternet);

    if (data.empty()) return false;

    std::string diskPath = TilePath(key.z, key.x, key.y);
    std::ofstream out(diskPath, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;
    out.write(data.data(), (std::streamsize)data.size());
    return out.good();
}
