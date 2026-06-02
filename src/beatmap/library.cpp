#include "beatmap/library.h"
#include "beatmap/parser.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

BeatmapLibrary beatmap_library;

// ─── Path helpers ─────────────────────────────────────────────────────────────

static bool IsDir(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

static std::string JoinPath(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    if (dir.back() == '/' || dir.back() == '\\') return dir + name;
    return dir + "/" + name;
}

static std::string FileExt(const std::string& path) {
    size_t dot = path.rfind('.');
    if (dot == std::string::npos) return "";
    std::string ext = path.substr(dot);
    for (char& c : ext) c = (char)tolower((unsigned char)c);
    return ext;
}

static bool IsBeatmapFile(const std::string& name) {
    std::string ext = FileExt(name);
    return ext == ".osu" || ext == ".ensou";
}

// ─── Scan helpers ─────────────────────────────────────────────────────────────

// Collect all beatmap file paths inside a single song folder (non-recursive).
static std::vector<std::string> CollectBeatmapFiles(const std::string& folder) {
    std::vector<std::string> files;
    DIR* d = opendir(folder.c_str());
    if (!d) return files;

    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr) {
        std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        if (IsBeatmapFile(name))
            files.push_back(JoinPath(folder, name));
    }
    closedir(d);

    std::sort(files.begin(), files.end());
    return files;
}

// Collect all immediate subdirectories of root (song folders).
static std::vector<std::string> CollectSongFolders(const std::string& root) {
    std::vector<std::string> folders;
    DIR* d = opendir(root.c_str());
    if (!d) return folders;

    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr) {
        std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        std::string full = JoinPath(root, name);
        if (IsDir(full))
            folders.push_back(full);
    }
    closedir(d);

    std::sort(folders.begin(), folders.end());
    return folders;
}

// ─── BeatmapLibrary::Scan ─────────────────────────────────────────────────────

int BeatmapLibrary::Scan(const std::string& dir) {
    songs_dir = dir;
    groups.clear();
    scanned = false;

    std::vector<std::string> folders = CollectSongFolders(dir);

    for (const std::string& folder : folders) {
        std::vector<std::string> files = CollectBeatmapFiles(folder);
        if (files.empty()) continue;

        BeatmapGroup group;
        group.folder = folder;

        for (const std::string& file : files) {
            Beatmap meta;
            std::string err;
            if (!BeatmapParser::ParseMetadataOnly(file, meta, err))
                continue; // skip unreadable / non-mania files silently

            // Use the first successfully parsed diff to fill group-level fields
            if (group.title.empty()) {
                group.title      = meta.title_unicode.empty() ? meta.title : meta.title_unicode;
                group.artist     = meta.artist_unicode.empty() ? meta.artist : meta.artist_unicode;
                group.creator    = meta.creator;
                group.audio_path = meta.audio_path;
                group.preview_ms = meta.preview_ms;
            }
            // Take the first non-empty background we find across all diffs
            if (group.background_path.empty() && !meta.background_path.empty())
                group.background_path = meta.background_path;

            if (meta.difficulty.star_rating > group.top_stars)
                group.top_stars = meta.difficulty.star_rating;

            group.diffs.push_back(std::move(meta));
        }

        if (group.diffs.empty()) continue;

        // Sort diffs by key count then star rating
        std::sort(group.diffs.begin(), group.diffs.end(),
            [](const Beatmap& a, const Beatmap& b) {
                if (a.difficulty.key_count != b.difficulty.key_count)
                    return a.difficulty.key_count < b.difficulty.key_count;
                return a.difficulty.star_rating < b.difficulty.star_rating;
            });

        groups.push_back(std::move(group));
    }

    scanned = true;
    return (int)groups.size();
}

int BeatmapLibrary::Rescan() {
    return Scan(songs_dir);
}

// ─── BeatmapLibrary::LoadDiff ─────────────────────────────────────────────────

bool BeatmapLibrary::LoadDiff(BeatmapGroup& group, int diff_index, std::string& out_error) {
    if (diff_index < 0 || diff_index >= (int)group.diffs.size()) {
        out_error = "Diff index out of range";
        return false;
    }

    Beatmap& diff = group.diffs[diff_index];
    if (diff.loaded) return true; // already loaded

    // Preserve metadata fields that ParseMetadataOnly populated, then do a
    // full parse into a temporary and move the notes/timing data across.
    Beatmap full;
    if (!BeatmapParser::Parse(diff.source_path, full, out_error))
        return false;

    diff.notes         = std::move(full.notes);
    diff.timing_points = std::move(full.timing_points);
    diff.loaded        = true;
    return true;
}

// ─── BeatmapLibrary::UnloadDiff ───────────────────────────────────────────────

void BeatmapLibrary::UnloadDiff(BeatmapGroup& group, int diff_index) {
    if (diff_index < 0 || diff_index >= (int)group.diffs.size()) return;
    Beatmap& diff = group.diffs[diff_index];
    diff.notes.clear();
    diff.notes.shrink_to_fit();
    diff.timing_points.clear();
    diff.timing_points.shrink_to_fit();
    diff.loaded = false;
}

void BeatmapLibrary::UnloadAll() {
    for (auto& group : groups)
        for (int i = 0; i < (int)group.diffs.size(); i++)
            UnloadDiff(group, i);
}

// ─── BeatmapLibrary::Search ───────────────────────────────────────────────────

static std::string ToLower(std::string s) {
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

std::vector<BeatmapGroup*> BeatmapLibrary::Search(const std::string& query) {
    std::vector<BeatmapGroup*> results;
    if (query.empty()) {
        for (auto& g : groups) results.push_back(&g);
        return results;
    }

    std::string q = ToLower(query);
    for (auto& g : groups) {
        bool match = ToLower(g.title).find(q)  != std::string::npos
                  || ToLower(g.artist).find(q) != std::string::npos
                  || ToLower(g.creator).find(q) != std::string::npos;
        if (match) results.push_back(&g);
    }
    return results;
}

// ─── BeatmapLibrary::ForEach ──────────────────────────────────────────────────

void BeatmapLibrary::ForEach(std::function<bool(BeatmapGroup&)> fn) {
    for (auto& g : groups)
        if (!fn(g)) break;
}

// ─── BeatmapLibrary stats ─────────────────────────────────────────────────────

int BeatmapLibrary::TotalDiffs() const {
    int n = 0;
    for (const auto& g : groups) n += (int)g.diffs.size();
    return n;
}

int BeatmapLibrary::TotalLoaded() const {
    int n = 0;
    for (const auto& g : groups)
        for (const auto& d : g.diffs)
            if (d.loaded) n++;
    return n;
}