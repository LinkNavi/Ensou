#include "beatmap/downloader.h"
#include "beatmap/library.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <sys/stat.h>
#include <sys/wait.h>
#include "raylib.h"

OsuDownloader osu_downloader;

// ─── Helpers ──────────────────────────────────────────────────────────────────

static bool FileExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

static std::string JoinPath(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    return (a.back() == '/') ? a + b : a + "/" + b;
}

static std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

static std::string UrlEncode(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (isalnum(c) || c=='-' || c=='_' || c=='.' || c=='~') out += (char)c;
        else { char buf[4]; snprintf(buf, sizeof(buf), "%%%02X", c); out += buf; }
    }
    return out;
}

static std::string JsonGetString(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return "";
    pos += needle.size();
    while (pos < json.size() && (json[pos]==' '||json[pos]=='\t'||json[pos]==':')) ++pos;
    if (pos >= json.size()) return "";
    if (json[pos] != '"') {
        size_t start = pos;
        while (pos < json.size() && json[pos]!=',' && json[pos]!='}') ++pos;
        return Trim(json.substr(start, pos - start));
    }
    ++pos;
    std::string result;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos+1 < json.size()) ++pos;
        result += json[pos++];
    }
    return result;
}

// ─── SanitizeDirName ─────────────────────────────────────────────────────────

std::string OsuDownloader::SanitizeDirName(const std::string& name) {
    std::string out;
    for (unsigned char c : name) {
        if (c=='/'||c=='\\'||c==':'||c=='*'||c=='?'||c=='"'||c=='<'||c=='>'||c=='|')
            out += '_';
        else out += (char)c;
    }
    while (!out.empty() && (out.back()=='.'||out.back()==' ')) out.pop_back();
    if (out.empty()) out = "beatmapset";
    return out;
}

// ─── LoadToken ────────────────────────────────────────────────────────────────

bool OsuDownloader::LoadToken(const std::string& config_path) {
    std::ifstream f(config_path);
    if (!f.is_open()) return false;
    std::string line;
    while (std::getline(f, line)) {
        line = Trim(line);
        if (line.empty() || line[0]=='#' || line[0]==';') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = Trim(line.substr(0, eq));
        std::string v = Trim(line.substr(eq + 1));
        if      (k == "osu_client_id")     client_id     = v;
        else if (k == "osu_client_secret") client_secret = v;
        else if (k == "osu_token")         token         = v;
        else if (k == "osu_token_expiry" && !v.empty())
            token_expiry = std::stod(v);
    }
    return HasCredentials();
}

// ─── SaveToken ────────────────────────────────────────────────────────────────

void OsuDownloader::SaveToken(const std::string& config_path) {
    std::vector<std::string> lines;
    { std::ifstream f(config_path); std::string l; while (std::getline(f, l)) lines.push_back(l); }

    const char* managed[] = { "osu_client_id", "osu_client_secret", "osu_token", "osu_token_expiry" };
    lines.erase(std::remove_if(lines.begin(), lines.end(), [&](const std::string& l) {
        std::string t = Trim(l);
        size_t eq = t.find('=');
        if (eq == std::string::npos) return false;
        std::string k = Trim(t.substr(0, eq));
        for (const char* m : managed) if (k == m) return true;
        return false;
    }), lines.end());

    lines.push_back("osu_client_id = "     + client_id);
    lines.push_back("osu_client_secret = " + client_secret);
    lines.push_back("osu_token = "         + token);
    lines.push_back("osu_token_expiry = "  + std::to_string(token_expiry));

    std::ofstream f(config_path);
    for (const auto& l : lines) f << l << "\n";
}

// ─── TokenValid ───────────────────────────────────────────────────────────────

bool OsuDownloader::TokenValid() const {
    if (token.empty()) return false;
    return GetTime() < token_expiry - 60.0;
}

// ─── RefreshTokenIfNeeded ─────────────────────────────────────────────────────

void OsuDownloader::RefreshTokenIfNeeded() {
    if (!HasCredentials())             return;
    if (TokenValid())                  return;
    if (refreshing_)                   return;
    if (GetTime() < refresh_retry_at_) return;

    refresh_accum_.clear();
    refreshing_ = true;

    std::string body =
        "client_id="      + UrlEncode(client_id)     +
        "&client_secret=" + UrlEncode(client_secret) +
        "&grant_type=client_credentials"
        "&scope=public";

    std::string cmd =
        "curl -s -X POST \"https://osu.ppy.sh/oauth/token\" "
        "-H \"Content-Type: application/x-www-form-urlencoded\" "
        "-d \"" + body + "\" 2>&1";

    TraceLog(LOG_INFO, "OsuDownloader: refreshing token...");
    refresh_pipe_ = popen(cmd.c_str(), "r");
    if (!refresh_pipe_) { refreshing_ = false; refresh_retry_at_ = GetTime() + 30.0; }
}

// ─── PollRefresh ─────────────────────────────────────────────────────────────

void OsuDownloader::PollRefresh() {
    if (!refresh_pipe_) return;
    char buf[1024];
    while (fgets(buf, sizeof(buf), refresh_pipe_)) refresh_accum_ += buf;
    if (!feof(refresh_pipe_)) return;

    pclose(refresh_pipe_);
    refresh_pipe_ = nullptr;
    refreshing_   = false;

    std::string new_token  = JsonGetString(refresh_accum_, "access_token");
    std::string expires_in = JsonGetString(refresh_accum_, "expires_in");

    if (!new_token.empty()) {
        token        = new_token;
        double exp   = expires_in.empty() ? 86400.0 : std::stod(expires_in);
        token_expiry = GetTime() + exp;
        refresh_retry_at_ = 0.0;
        SaveToken("config.ini");
        TraceLog(LOG_INFO, "OsuDownloader: token refreshed, expires in %.0f s", exp);
    } else {
        refresh_retry_at_ = GetTime() + 30.0;
        TraceLog(LOG_WARNING, "OsuDownloader: token refresh failed: %s", refresh_accum_.c_str());
    }
}

// ─── SetState / Cleanup ───────────────────────────────────────────────────────

void OsuDownloader::SetState(DownloadStatus::State s, const std::string& msg) {
    status_.state   = s;
    status_.message = msg;
    if (callback_) callback_(status_);
}

void OsuDownloader::Cleanup() {
    if (pipe_) { pclose(pipe_); pipe_ = nullptr; }
    extracting_ = false;
}

// ─── Download ─────────────────────────────────────────────────────────────────
// Uses mirror APIs that don't require user auth — no session cookie needed.
// Mirror priority: nerinyan.moe → beatconnect.io
// Both are free, stable, and serve the same .osz files.

bool OsuDownloader::Download(int beatmapset_id,
                              const std::string& songs_dir,
                              DownloadCallback on_update) {
    if (Busy()) return false;

    callback_             = on_update;
    songs_dir_            = songs_dir;
    status_.beatmapset_id = beatmapset_id;
    status_.progress      = 0.0f;
    mirror_index_         = 0;

    tmp_path_ = "/tmp/ensou_" + std::to_string(beatmapset_id) + ".osz";
    if (FileExists(tmp_path_)) std::remove(tmp_path_.c_str());

    StartDownloadFromMirror(beatmapset_id);
    return true;
}

void OsuDownloader::StartDownloadFromMirror(int beatmapset_id) {
    // Mirror list — tried in order on failure
    static const char* mirrors[] = {
        "https://api.nerinyan.moe/d/",      // nerinyan: /d/{id}
        "https://beatconnect.io/b/",        // beatconnect: /b/{id}
    };
    static const int mirror_count = 2;

    if (mirror_index_ >= mirror_count) {
        SetState(DownloadStatus::State::ERROR,
                 "All mirrors failed. Check your internet connection.");
        return;
    }

    std::string url = std::string(mirrors[mirror_index_]) + std::to_string(beatmapset_id);

    std::string cmd =
        "curl -L -s -S "
        "--max-time 120 "
        "-A \"Ensou/0.1\" "
        "-o \"" + tmp_path_ + "\" "
        "-w \"%{http_code}\" "
        "\"" + url + "\" 2>&1";

    pipe_ = popen(cmd.c_str(), "r");
    if (!pipe_) {
        SetState(DownloadStatus::State::ERROR, "Failed to launch curl.");
        return;
    }

    extracting_ = false;
    SetState(DownloadStatus::State::DOWNLOADING,
             "Downloading from mirror " + std::to_string(mirror_index_ + 1) + "...");
    TraceLog(LOG_INFO, "OsuDownloader: trying mirror %d: %s", mirror_index_, url.c_str());
}

// ─── StartExtract ─────────────────────────────────────────────────────────────

void OsuDownloader::StartExtract() {
    extract_dir_ = JoinPath(songs_dir_,
                            SanitizeDirName(std::to_string(status_.beatmapset_id)));
    mkdir(extract_dir_.c_str(), 0755);

    std::string cmd =
        "unzip -o -q \"" + tmp_path_ + "\" -d \"" + extract_dir_ + "\" 2>&1";

    pipe_ = popen(cmd.c_str(), "r");
    if (!pipe_) { SetState(DownloadStatus::State::ERROR, "Failed to launch unzip."); Cleanup(); return; }

    extracting_ = true;
    SetState(DownloadStatus::State::EXTRACTING, "Extracting...");
}

// ─── Update ───────────────────────────────────────────────────────────────────

void OsuDownloader::Update() {
    PollRefresh();
    RefreshTokenIfNeeded();

    if (!pipe_) return;

    char buf[256];
    std::string output;
    while (fgets(buf, sizeof(buf), pipe_)) output += buf;

    if (!feof(pipe_)) return;

    int raw = pclose(pipe_);
    pipe_ = nullptr;
    int exit_code = WIFEXITED(raw) ? WEXITSTATUS(raw) : -1;

    if (!extracting_) {
        std::string http_code = Trim(output);
        TraceLog(LOG_INFO, "OsuDownloader: mirror %d HTTP %s exit %d",
                 mirror_index_, http_code.c_str(), exit_code);

        // On any failure, try next mirror
        bool failed = false;
        std::string fail_reason;

        if (exit_code != 0) {
            failed = true; fail_reason = "curl error (exit " + std::to_string(exit_code) + ")";
        } else if (http_code == "404") {
            failed = true; fail_reason = "not found on mirror " + std::to_string(mirror_index_+1);
        } else if (http_code != "200") {
            failed = true; fail_reason = "HTTP " + http_code;
        } else {
            // Verify file is a valid zip (check magic bytes)
            struct stat st;
            if (stat(tmp_path_.c_str(), &st) != 0 || st.st_size < 4) {
                failed = true; fail_reason = "downloaded file too small";
            } else {
                FILE* chk = fopen(tmp_path_.c_str(), "rb");
                uint8_t magic[4] = {};
                if (chk) { fread(magic, 1, 4, chk); fclose(chk); }
                // ZIP magic: PK\x03\x04
                if (magic[0]!='P' || magic[1]!='K' || magic[2]!=0x03 || magic[3]!=0x04) {
                    failed = true; fail_reason = "not a valid zip file from mirror " + std::to_string(mirror_index_+1);
                }
            }
        }

        if (failed) {
            std::remove(tmp_path_.c_str());
            mirror_index_++;
            TraceLog(LOG_WARNING, "OsuDownloader: mirror failed (%s), trying next...", fail_reason.c_str());
            StartDownloadFromMirror(status_.beatmapset_id);
            return;
        }

        StartExtract();

    } else {
        extracting_ = false;
        std::remove(tmp_path_.c_str());

        if (exit_code != 0) {
            SetState(DownloadStatus::State::ERROR,
                     "unzip failed (exit " + std::to_string(exit_code) + "): " + Trim(output));
            Cleanup(); return;
        }

        SetState(DownloadStatus::State::DONE,
                 "Downloaded to songs/" + std::to_string(status_.beatmapset_id) + "/");
        beatmap_library.Rescan();
    }
}

// ─── Cancel ───────────────────────────────────────────────────────────────────

void OsuDownloader::Cancel() {
    if (!pipe_) return;
    pclose(pipe_); pipe_ = nullptr;
    if (!tmp_path_.empty() && FileExists(tmp_path_)) std::remove(tmp_path_.c_str());
    SetState(DownloadStatus::State::IDLE, "Cancelled.");
    Cleanup();
}
