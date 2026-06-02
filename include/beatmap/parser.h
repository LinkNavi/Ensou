#pragma once
#include "beatmap/beatmap.h"
#include <string>

// ─── BeatmapParser ────────────────────────────────────────────────────────────
// All parse functions return true on success and populate the given Beatmap.
// On failure they return false and leave an error message in out_error.
//
// ParseMetadataOnly is fast — it stops after reading the header sections
// ([General], [Metadata], [Difficulty]) without loading notes or timing points.
// Use it when building the song select list.
//
// ParseOsu / ParseEnsou do a full load including notes and timing points.

namespace BeatmapParser {

// Full parse of an .osu file (mania mode only).
// Non-mania beatmaps will fail with an appropriate error message.
bool ParseOsu(const std::string& path, Beatmap& out, std::string& out_error);

// Full parse of a native .ensou file.
bool ParseEnsou(const std::string& path, Beatmap& out, std::string& out_error);

// Metadata-only parse — works for both .osu and .ensou.
// Populates: title, artist, creator, tags, preview_ms, difficulty (name,
// key_count, star_rating, hp_drain, od), source_path, audio_path.
// Does NOT populate notes or timing_points.
bool ParseMetadataOnly(const std::string& path, Beatmap& out, std::string& out_error);

// Convenience: dispatch to ParseOsu or ParseEnsou based on file extension.
bool Parse(const std::string& path, Beatmap& out, std::string& out_error);

} // namespace BeatmapParser