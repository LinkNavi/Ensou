#pragma once
#include <string>
#include <vector>
#include <cstdint>

// ─── Note types ───────────────────────────────────────────────────────────────
// Standard osu!mania types are covered by NORMAL and HOLD.
// Custom types start at CUSTOM_0 — the .ensou format can use any of these.
enum class NoteType : uint8_t {
    // ── Standard ──────────────────────────────────────────────────────────────
    NORMAL      = 0,   // Tap note
    HOLD        = 1,   // Hold note (has duration)

    // ── Custom ────────────────────────────────────────────────────────────────
    // Add new types here. The parser will emit these when reading .ensou files.
    // Gameplay screen switches on NoteType, so new entries just need a new case.
    SWAP    = 64,
    MINE    = 65,

};

inline bool NoteIsHold(NoteType t) { return t == NoteType::HOLD; }
inline bool NoteIsCustom(NoteType t) { return (uint8_t)t >= 64; }

// ─── Note ─────────────────────────────────────────────────────────────────────
struct Note {
    int      column;        // 0-based lane index
    int      time_ms;       // Hit time in milliseconds from song start
    int      end_ms;        // For HOLD: release time. For others: same as time_ms
    NoteType type;

    // Custom note payload — ignored for standard types.
    // .ensou files can store arbitrary per-note data here.
    uint8_t  custom_data[8] = {};
};

// ─── Timing point ─────────────────────────────────────────────────────────────
struct TimingPoint {
    int   time_ms;       // Offset from song start in ms
    float bpm;           // Beats per minute (uninherited points only)
    float beat_length;   // ms per beat (= 60000 / bpm)
    float sv_multiplier; // Scroll velocity multiplier (inherited points, default 1.0)
    int   meter;         // Beats per measure (time signature numerator)
    bool  inherited;     // true = SV-only point, false = BPM point
};

// ─── Difficulty metadata ───────────────────────────────────────────────────────
struct DifficultyInfo {
    std::string name;           // e.g. "Easy", "Hard", "Insane"
    int         key_count = 4;  // Number of lanes
    float       star_rating = 0.0f;
    float       hp_drain    = 5.0f;
    float       od          = 8.0f; // Overall difficulty (timing windows)
};

// ─── Beatmap ──────────────────────────────────────────────────────────────────
// One Beatmap = one difficulty of one song.
// Multiple difficulties for the same song are separate Beatmap objects
// grouped by their shared audio file / folder.
struct Beatmap {
    // ── Identity ──────────────────────────────────────────────────────────────
    std::string source_path;    // Absolute path to the .osu or .ensou file
    std::string audio_path;     // Absolute path to the audio file
    std::string background_path; // Absolute path to the background/cover image

    // ── Metadata ──────────────────────────────────────────────────────────────
    std::string title;
    std::string title_unicode;
    std::string artist;
    std::string artist_unicode;
    std::string creator;        // Mapper name
    std::string tags;

    // ── Difficulty ────────────────────────────────────────────────────────────
    DifficultyInfo difficulty;

    // ── Timing ────────────────────────────────────────────────────────────────
    int preview_ms = 0;         // Song preview start time in ms
    std::vector<TimingPoint> timing_points;

    // ── Notes ─────────────────────────────────────────────────────────────────
    std::vector<Note> notes;    // Sorted ascending by time_ms

    // ── Helpers ───────────────────────────────────────────────────────────────

    // BPM at a given time (walks timing points)
    float BpmAt(int time_ms) const {
        float bpm = 120.0f;
        for (const auto& tp : timing_points) {
            if (tp.time_ms > time_ms) break;
            if (!tp.inherited) bpm = tp.bpm;
        }
        return bpm;
    }

    // Scroll velocity multiplier at a given time
    float SVAt(int time_ms) const {
        float sv = 1.0f;
        for (const auto& tp : timing_points) {
            if (tp.time_ms > time_ms) break;
            if (tp.inherited) sv = tp.sv_multiplier;
        }
        return sv;
    }

    // Dominant BPM (the one that covers the most time)
    float DominantBpm() const {
        if (timing_points.empty()) return 120.0f;
        float best_bpm = timing_points[0].bpm;
        float best_dur = 0.0f;
        for (int i = 0; i < (int)timing_points.size(); i++) {
            if (timing_points[i].inherited) continue;
            int end = (i + 1 < (int)timing_points.size())
                        ? timing_points[i + 1].time_ms
                        : (notes.empty() ? timing_points[i].time_ms : notes.back().time_ms);
            float dur = (float)(end - timing_points[i].time_ms);
            if (dur > best_dur) {
                best_dur = dur;
                best_bpm = timing_points[i].bpm;
            }
        }
        return best_bpm;
    }

    // Total length in ms (last note end)
    int LengthMs() const {
        if (notes.empty()) return 0;
        int last = 0;
        for (const auto& n : notes)
            if (n.end_ms > last) last = n.end_ms;
        return last;
    }

    bool loaded = false;   // true once notes/timing_points are populated
};
