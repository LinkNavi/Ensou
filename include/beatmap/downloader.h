#pragma once
#include <string>
#include <functional>

// ─── OsuDownloader ────────────────────────────────────────────────────────────
// Downloads a beatmapset from osu! using the user's Bearer token and extracts
// it into the songs/ directory.
//
// Flow:
//   1. Shell out curl with Authorization: Bearer <token> to download the .osz
//   2. Extract the .osz (it's a zip) into songs/<sanitized title>/
//   3. Trigger a library rescan so the new maps appear immediately
//
// The token is loaded from / saved to config.ini automatically.
// The user only needs to paste it once.
//
// How to get a token:
//   - Open https://osu.ppy.sh/home/account/edit
//   - Scroll to "OAuth" section
//   - Create a new OAuth application (callback URL can be anything)
//   - Use the Client Credentials flow to get a token, OR
//   - Open browser devtools on osu.ppy.sh, go to Network tab, make any request,
//     and copy the Authorization header value (without "Bearer ")

struct DownloadStatus {
    enum class State {
        IDLE,
        DOWNLOADING,   // curl in progress
        EXTRACTING,    // unzip in progress
        DONE,          // success
        ERROR,         // something went wrong
    };

    State       state    = State::IDLE;
    std::string message;           // human-readable status or error
    int         beatmapset_id = 0; // which set is being downloaded
    float       progress = 0.0f;  // 0..1, best-effort from curl --progress-bar
};

// Callback type: called on state changes (from the main thread on next Update())
using DownloadCallback = std::function<void(const DownloadStatus&)>;

struct OsuDownloader {
    // ── Token management ──────────────────────────────────────────────────────

    std::string token;             // Bearer token (without "Bearer " prefix)

    // Load token from config.ini. Returns true if a token was found.
    bool LoadToken(const std::string& config_path = "config.ini");

    // Save token to config.ini.
    void SaveToken(const std::string& config_path = "config.ini");

    // ── Downloading ───────────────────────────────────────────────────────────

    // Start an async download of the given beatmapset ID into songs_dir.
    // Returns false immediately if a download is already in progress or the
    // token is empty.
    // on_update is called (on the next Update() tick) whenever status changes.
    bool Download(int beatmapset_id,
                  const std::string& songs_dir,
                  DownloadCallback on_update = nullptr);

    // Poll for completion / status changes. Call this every frame.
    // Fires the callback if state has changed since last call.
    void Update();

    // Cancel an in-progress download (kills the curl process).
    void Cancel();

    // ── Status ────────────────────────────────────────────────────────────────

    const DownloadStatus& Status() const { return status_; }
    bool Busy() const {
        return status_.state == DownloadStatus::State::DOWNLOADING
            || status_.state == DownloadStatus::State::EXTRACTING;
    }

private:
    DownloadStatus  status_;
    DownloadCallback callback_;
    std::string     songs_dir_;
    std::string     tmp_path_;     // path to the downloaded .osz temp file
    std::string     extract_dir_;  // destination folder under songs/

    // popen handles — one active at a time
    FILE*  pipe_      = nullptr;
    bool   extracting_ = false;

    void SetState(DownloadStatus::State s, const std::string& msg = "");
    void StartExtract();
    void Cleanup();

    // Sanitize a beatmapset title for use as a directory name
    static std::string SanitizeDirName(const std::string& name);
};

// Global instance
extern OsuDownloader osu_downloader;