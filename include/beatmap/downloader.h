#pragma once
#include <string>
#include <functional>

struct DownloadStatus {
    enum class State { IDLE, DOWNLOADING, EXTRACTING, DONE, ERROR };
    State       state         = State::IDLE;
    std::string message;
    int         beatmapset_id = 0;
    float       progress      = 0.0f;
};

using DownloadCallback = std::function<void(const DownloadStatus&)>;

struct OsuDownloader {
    // ── OAuth credentials (for search only) ──────────────────────────────────
    std::string token;
    std::string client_id;
    std::string client_secret;
    double      token_expiry    = 0.0;

    bool LoadToken(const std::string& config_path = "config.ini");
    void SaveToken(const std::string& config_path = "config.ini");

    bool HasCredentials() const { return !client_id.empty() && !client_secret.empty(); }
    bool TokenValid() const;
    void RefreshTokenIfNeeded();

    // ── Download (no auth needed — uses mirror APIs) ──────────────────────────
    bool Download(int beatmapset_id, const std::string& songs_dir,
                  DownloadCallback on_update = nullptr);
    void Update();
    void Cancel();

    const DownloadStatus& Status() const { return status_; }
    bool Busy() const {
        return status_.state == DownloadStatus::State::DOWNLOADING
            || status_.state == DownloadStatus::State::EXTRACTING;
    }

private:
    DownloadStatus   status_;
    DownloadCallback callback_;
    std::string      songs_dir_;
    std::string      tmp_path_;
    std::string      extract_dir_;
    int              mirror_index_ = 0;

    FILE* pipe_       = nullptr;
    bool  extracting_ = false;

    FILE*       refresh_pipe_     = nullptr;
    std::string refresh_accum_;
    bool        refreshing_       = false;
    double      refresh_retry_at_ = 0.0;

    void SetState(DownloadStatus::State s, const std::string& msg = "");
    void StartExtract();
    void StartDownloadFromMirror(int beatmapset_id);
    void Cleanup();
    void PollRefresh();

    static std::string SanitizeDirName(const std::string& name);
};

extern OsuDownloader osu_downloader;
