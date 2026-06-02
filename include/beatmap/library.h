#pragma once
#include "beatmap/beatmap.h"
#include <string>
#include <vector>
#include <functional>

// ─── BeatmapGroup ─────────────────────────────────────────────────────────────
// All difficulties of the same song, grouped by their shared folder.
struct BeatmapGroup {
    std::string folder;         // Absolute path to the song folder
    std::string title;          // From the first difficulty's metadata
    std::string artist;
    std::string creator;
    std::string audio_path;
    std::string background_path; // Absolute path to the background/cover image
    int         preview_ms = 0;
    float       top_stars  = 0.0f; // Highest star rating across all diffs

    std::vector<Beatmap> diffs; // Metadata-only until LoadDiff() is called
};

// ─── BeatmapLibrary ───────────────────────────────────────────────────────────
// Scans a root directory for song folders, each of which may contain one or
// more .osu / .ensou files. Metadata is loaded eagerly on Scan(); notes and
// timing points are loaded lazily on demand via LoadDiff().
//
// Expected layout:
//   songs/
//     Song Title - Artist/
//       Easy.osu
//       Hard.osu
//       song.mp3
//     Another Song/
//       map.ensou
//       audio.ogg
struct BeatmapLibrary {
    std::vector<BeatmapGroup> groups;
    std::string               songs_dir;
    bool                      scanned = false;

    // ── Scanning ──────────────────────────────────────────────────────────────

    // Scan songs_dir for all .osu and .ensou files.
    // Populates groups with metadata-only Beatmaps.
    // Returns the number of groups found.
    int Scan(const std::string& dir);

    // Re-scan, clearing existing data first.
    int Rescan();

    // ── Loading ───────────────────────────────────────────────────────────────

    // Fully load a specific difficulty (notes + timing points).
    // Safe to call multiple times — skips if already loaded.
    // Returns false and writes to out_error on failure.
    bool LoadDiff(BeatmapGroup& group, int diff_index, std::string& out_error);

    // Unload notes/timing data for a diff to free memory, keeping metadata.
    void UnloadDiff(BeatmapGroup& group, int diff_index);

    // Unload all fully-loaded diffs across all groups.
    void UnloadAll();

    // ── Searching / filtering ─────────────────────────────────────────────────

    // Returns pointers to groups whose title or artist contain the query
    // (case-insensitive). Pointers are valid until the next Scan/Rescan.
    std::vector<BeatmapGroup*> Search(const std::string& query);

    // Iterate all groups, calling fn for each. Stops early if fn returns false.
    void ForEach(std::function<bool(BeatmapGroup&)> fn);

    // ── Stats ─────────────────────────────────────────────────────────────────

    int TotalGroups()    const { return (int)groups.size(); }
    int TotalDiffs()     const;
    int TotalLoaded()    const;
};

// Global instance — accessible from any screen.
extern BeatmapLibrary beatmap_library;