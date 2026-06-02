#include "beatmap/downloader.h"
#include "beatmap/library.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <sys/stat.h>
#include <sys/wait.h>

OsuDownloader osu_downloader;

// ─── Path helpers ─────────────────────────────────────────────────────────────

static bool FileExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

static std::string JoinPath(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (a.back() == '/') return a + b;
    return a + "/" + b;
}

// ─── String helpers ───────────────────────────────────────────────────────────

static std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// ─── OsuDownloader::SanitizeDirName ──────────────────────────────────────────
// Strips characters that are problematic in directory names.

std::string OsuDownloader::SanitizeDirName(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (unsigned char c : name) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' ||
            c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            out += '_';
        else
            out += (char)c;
    }
    // Trim trailing dots/spaces (Windows compat, good practice)
    while (!out.empty() && (out.back() == '.' || out.back() == ' '))
        out.pop_back();
    if (out.empty()) out = "beatmapset";
    return out;
}

// ─── OsuDownloader::LoadToken ─────────────────────────────────────────────────
// Reads config.ini looking for a line:  osu_token = <value>

bool OsuDownloader::LoadToken(const std::string& config_path) {
    std::ifstream f(config_path);
    if (!f.is_open()) return false;

    std::string line;
    while (std::getline(f, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = Trim(line.substr(0, eq));
        std::string val = Trim(line.substr(eq + 1));

        if (key == "osu_token" && !val.empty()) {
            token = val;
            return true;
        }
    }
    return false;
}

// ─── OsuDownloader::SaveToken ─────────────────────────────────────────────────
// Rewrites config.ini preserving all other keys, updating/adding osu_token.

void OsuDownloader::SaveToken(const std::string& config_path) {
    // Read existing lines
    std::vector<std::string> lines;
    {
        std::ifstream f(config_path);
        std::string line;
        while (std::getline(f, line))
            lines.push_back(line);
    }

    // Update or append
    bool found = false;
    for (auto& line : lines) {
        std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';') continue;
        size_t eq = trimmed.find('=');
        if (eq == std::string::npos) continue;
        if (Trim(trimmed.substr(0, eq)) == "osu_token") {
            line = "osu_token = " + token;
            found = true;
            break;
        }
    }
    if (!found)
        lines.push_back("osu_token = " + token);

    std::ofstream f(config_path);
    for (const auto& line : lines)
        f << line << "\n";
}

// ─── OsuDownloader::SetState ──────────────────────────────────────────────────

void OsuDownloader::SetState(DownloadStatus::State s, const std::string& msg) {
    status_.state   = s;
    status_.message = msg;
    if (callback_) callback_(status_);
}

// ─── OsuDownloader::Cleanup ───────────────────────────────────────────────────

void OsuDownloader::Cleanup() {
    if (pipe_) {
        pclose(pipe_);
        pipe_ = nullptr;
    }
    extracting_ = false;
}

// ─── OsuDownloader::Download ──────────────────────────────────────────────────

bool OsuDownloader::Download(int beatmapset_id,
                             const std::string& songs_dir,
                             DownloadCallback on_update) {
    if (Busy()) return false;
    if (token.empty()) {
        SetState(DownloadStatus::State::ERROR, "No token set. Paste your osu! Bearer token in Settings.");
        return false;
    }

    callback_         = on_update;
    songs_dir_        = songs_dir;
    status_.beatmapset_id = beatmapset_id;
    status_.progress  = 0.0f;

    // Temp file path: /tmp/ensou_<id>.osz
    tmp_path_ = "/tmp/ensou_" + std::to_string(beatmapset_id) + ".osz";

    // Remove stale temp file if it exists
    if (FileExists(tmp_path_))
        std::remove(tmp_path_.c_str());

    // Build curl command:
    //   curl -L -s -S
    //        -H "Authorization: Bearer <token>"
    //        -o <tmp_path>
    //        -w "%{http_code}"          <- prints HTTP code to stdout so we can check it
    //        "https://osu.ppy.sh/beatmapsets/<id>/download"
    //
    // We capture stdout (the HTTP code) via popen.
    // curl errors go to stderr which the user will see in the terminal.
    std::string cmd =
        "curl -L -s -S "
        "-H \"Authorization: Bearer " + token + "\" "
        "-o \"" + tmp_path_ + "\" "
        "-w \"%{http_code}\" "
        "\"https://osu.ppy.sh/beatmapsets/" + std::to_string(beatmapset_id) + "/download\" "
        "2>&1";

    pipe_ = popen(cmd.c_str(), "r");
    if (!pipe_) {
        SetState(DownloadStatus::State::ERROR, "Failed to launch curl.");
        return false;
    }

    extracting_ = false;
    SetState(DownloadStatus::State::DOWNLOADING,
             "Downloading beatmapset " + std::to_string(beatmapset_id) + "...");
    return true;
}

// ─── OsuDownloader::StartExtract ─────────────────────────────────────────────

void OsuDownloader::StartExtract() {
    // We don't know the title at this point (would need an API call), so use
    // the beatmapset ID as the folder name. The library scanner will read the
    // actual title from the .osu files inside.
    extract_dir_ = JoinPath(songs_dir_,
                            SanitizeDirName(std::to_string(status_.beatmapset_id)));

    // Create destination directory
    mkdir(extract_dir_.c_str(), 0755);

    // unzip -o (overwrite) -q (quiet) <osz> -d <dir>
    std::string cmd =
        "unzip -o -q \"" + tmp_path_ + "\" "
        "-d \"" + extract_dir_ + "\" 2>&1";

    pipe_ = popen(cmd.c_str(), "r");
    if (!pipe_) {
        SetState(DownloadStatus::State::ERROR, "Failed to launch unzip.");
        Cleanup();
        return;
    }

    extracting_ = true;
    SetState(DownloadStatus::State::EXTRACTING, "Extracting...");
}

// ─── OsuDownloader::Update ───────────────────────────────────────────────────
// Called every frame. Checks if the current pipe has finished.

void OsuDownloader::Update() {
    if (!pipe_) return;

    // Read any available output without blocking
    char buf[256];
    std::string output;
    // Use a non-blocking read by checking with feof first after a short drain
    // We read whatever is available then check if the process is done.
    while (fgets(buf, sizeof(buf), pipe_) != nullptr)
        output += buf;

    // fgets returns NULL on EOF (process finished) or error
    // Since we already drained above and got NULL, check if pipe is done
    if (feof(pipe_)) {
        int exit_code = pclose(pipe_);
        pipe_ = nullptr;

        if (!extracting_) {
            // Just finished the curl download phase
            // output should contain the HTTP status code written by -w "%{http_code}"
            std::string http_code = Trim(output);

            if (exit_code != 0) {
                SetState(DownloadStatus::State::ERROR,
                         "curl failed (exit " + std::to_string(exit_code) + "): " + output);
                Cleanup();
                return;
            }

            // Check HTTP code — 200 = OK, 401 = bad token, 403 = forbidden, 404 = not found
            if (http_code == "401" || http_code == "403") {
                SetState(DownloadStatus::State::ERROR,
                         "Download rejected (HTTP " + http_code + "). Check your token.");
                std::remove(tmp_path_.c_str());
                Cleanup();
                return;
            }
            if (http_code == "404") {
                SetState(DownloadStatus::State::ERROR,
                         "Beatmapset not found (HTTP 404).");
                std::remove(tmp_path_.c_str());
                Cleanup();
                return;
            }
            if (http_code != "200") {
                SetState(DownloadStatus::State::ERROR,
                         "Unexpected HTTP response: " + http_code);
                std::remove(tmp_path_.c_str());
                Cleanup();
                return;
            }

            if (!FileExists(tmp_path_)) {
                SetState(DownloadStatus::State::ERROR,
                         "Download appeared to succeed but file not found.");
                Cleanup();
                return;
            }

            // Move on to extraction
            StartExtract();

        } else {
            // Just finished the unzip extraction phase
            extracting_ = false;

            // Clean up the temp .osz
            std::remove(tmp_path_.c_str());

            if (exit_code != 0) {
                SetState(DownloadStatus::State::ERROR,
                         "unzip failed (exit " + std::to_string(exit_code) + "): " + output);
                Cleanup();
                return;
            }

            SetState(DownloadStatus::State::DONE,
                     "Downloaded to songs/" + std::to_string(status_.beatmapset_id) + "/");

            // Trigger a library rescan so the new maps show up immediately
            beatmap_library.Rescan();
        }
    }
}

// ─── OsuDownloader::Cancel ───────────────────────────────────────────────────

void OsuDownloader::Cancel() {
    if (!pipe_) return;
    pclose(pipe_);
    pipe_ = nullptr;

    // Remove partial download
    if (!tmp_path_.empty() && FileExists(tmp_path_))
        std::remove(tmp_path_.c_str());

    SetState(DownloadStatus::State::IDLE, "Cancelled.");
    Cleanup();
}