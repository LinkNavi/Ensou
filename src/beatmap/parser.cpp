#include "beatmap/parser.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

// ─── String helpers ───────────────────────────────────────────────────────────

static std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static std::string ToLower(std::string s) {
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

static bool StartsWith(const std::string& s, const char* prefix) {
    return s.rfind(prefix, 0) == 0;
}

// Split on first occurrence of delim, returning {left, right}.
// If delim not found, right is empty.
static std::pair<std::string,std::string> SplitFirst(const std::string& s, char delim) {
    size_t pos = s.find(delim);
    if (pos == std::string::npos) return { s, "" };
    return { Trim(s.substr(0, pos)), Trim(s.substr(pos + 1)) };
}

// Split string on delim into a vector of trimmed tokens.
static std::vector<std::string> Split(const std::string& s, char delim) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, delim))
        out.push_back(Trim(tok));
    return out;
}

static int   ToInt(const std::string& s)   { return std::stoi(s); }
static float ToFloat(const std::string& s) { return std::stof(s); }

// ─── Section detection ────────────────────────────────────────────────────────



static std::string GetSection(const std::string& line) {
    if (line.size() > 2 && line.front() == '[' && line.back() == ']')
        return ToLower(line.substr(1, line.size()-2));
    return "";
}

// ─── osu! hit object flags ────────────────────────────────────────────────────
// Bit 0: circle, Bit 1: slider, Bit 7: hold (mania)
static constexpr int OSU_TYPE_HOLD = 128;

// ─── Events parser ────────────────────────────────────────────────────────────
// Parses a single line from the [Events] section and extracts the background
// image path if present. osu! background event format:
//   0,0,"filename.jpg",0,0
// Type 0 = background. The filename is always the third comma-separated token,
// wrapped in double quotes.
static void ParseEventBackground(const std::string& line,
                                 const std::string& base_dir,
                                 std::string& out_bg) {
    if (!out_bg.empty()) return; // already found one
    auto parts = Split(line, ',');
    if (parts.size() < 3) return;
    if (Trim(parts[0]) != "0") return; // not a background event
    std::string name = Trim(parts[2]);
    // Strip surrounding quotes
    if (name.size() >= 2 && name.front() == '"' && name.back() == '"')
        name = name.substr(1, name.size() - 2);
    if (!name.empty())
        out_bg = base_dir + name;
}

// ─── Column mapping ───────────────────────────────────────────────────────────
// osu!mania stores column as x in [0, 512).
static int OsuXToColumn(int x, int key_count) {
    return (int)std::floor((float)x * (float)key_count / 512.0f);
}

// ─── .osu parser ─────────────────────────────────────────────────────────────

bool BeatmapParser::ParseOsu(const std::string& path, Beatmap& out, std::string& out_error) {
    std::ifstream f(path);
    if (!f.is_open()) {
        out_error = "Cannot open file: " + path;
        return false;
    }

    out = Beatmap{};
    out.source_path = path;

    // Derive base directory for resolving relative audio paths.
    std::string base_dir = path;
    {
        size_t slash = base_dir.find_last_of("/\\");
        if (slash != std::string::npos) base_dir = base_dir.substr(0, slash + 1);
        else base_dir = "./";
    }

    std::string line;
    std::string section;
    bool mode_found = false;

    while (std::getline(f, line)) {
        line = Trim(line);
        if (line.empty() || StartsWith(line, "//")) continue;

        // Section header
        std::string sec = GetSection(line);
        if (!sec.empty()) { section = sec; continue; }

        if (section == "general") {
            auto [k, v] = SplitFirst(line, ':');
            if (k == "AudioFilename")
                out.audio_path = base_dir + v;
            else if (k == "PreviewTime")
                out.preview_ms = ToInt(v);
            else if (k == "Mode") {
                mode_found = true;
                if (v != "3") {
                    out_error = "Not an osu!mania beatmap (Mode: " + v + ")";
                    return false;
                }
            }
        }
        else if (section == "events") {
            ParseEventBackground(line, base_dir, out.background_path);
        }
        else if (section == "metadata") {
            auto [k, v] = SplitFirst(line, ':');
            if      (k == "Title")          out.title          = v;
            else if (k == "TitleUnicode")   out.title_unicode  = v;
            else if (k == "Artist")         out.artist         = v;
            else if (k == "ArtistUnicode")  out.artist_unicode = v;
            else if (k == "Creator")        out.creator        = v;
            else if (k == "Version")        out.difficulty.name = v;
            else if (k == "Source")         ; // ignored for now
            else if (k == "Tags")           out.tags           = v;
        }
        else if (section == "difficulty") {
            auto [k, v] = SplitFirst(line, ':');
            if      (k == "CircleSize")       out.difficulty.key_count  = (int)ToFloat(v);
            else if (k == "HPDrainRate")      out.difficulty.hp_drain   = ToFloat(v);
            else if (k == "OverallDifficulty") out.difficulty.od        = ToFloat(v);
        }
        else if (section == "timingpoints") {
            // time,beatLength,meter,sampleSet,sampleIndex,volume,uninherited,effects
            auto parts = Split(line, ',');
            if (parts.size() < 8) continue;

            TimingPoint tp{};
            tp.time_ms   = (int)ToFloat(parts[0]);
            float bl     = ToFloat(parts[1]);
            tp.meter     = ToInt(parts[2]);
            tp.inherited = (parts[6] == "0"); // osu: 0 = inherited, 1 = uninherited

            if (!tp.inherited) {
                // Uninherited: beatLength is ms per beat
                tp.beat_length   = bl;
                tp.bpm           = 60000.0f / bl;
                tp.sv_multiplier = 1.0f;
            } else {
                // Inherited: beatLength is negative, encodes SV as -100/bl
                tp.beat_length   = 0.0f;
                tp.bpm           = 0.0f;
                tp.sv_multiplier = (bl < 0.0f) ? (-100.0f / bl) : 1.0f;
            }

            out.timing_points.push_back(tp);
        }
        else if (section == "hitobjects") {
            // x,y,time,type,hitSound[,endTime:hitSample | hitSample]
            auto parts = Split(line, ',');
            if (parts.size() < 5) continue;

            int x    = ToInt(parts[0]);
            int time = ToInt(parts[2]);
            int type = ToInt(parts[3]);

            Note note{};
            note.column  = OsuXToColumn(x, out.difficulty.key_count);
            note.time_ms = time;

            if (type & OSU_TYPE_HOLD) {
                note.type = NoteType::HOLD;
                // endTime is in parts[5] before the colon
                if (parts.size() >= 6) {
                    auto [end_str, _] = SplitFirst(parts[5], ':');
                    note.end_ms = ToInt(end_str);
                } else {
                    note.end_ms = time;
                }
            } else {
                note.type   = NoteType::NORMAL;
                note.end_ms = time;
            }

            out.notes.push_back(note);
        }
    }

    // Validate
    if (!mode_found) {
        out_error = "Not an osu!mania beatmap (no Mode key — defaults to osu! standard)";
        return false;
    }
    if (out.title.empty()) {
        out_error = "Missing metadata in: " + path;
        return false;
    }

    // Sort notes by time (osu files are usually sorted, but be safe)
    std::sort(out.notes.begin(), out.notes.end(),
        [](const Note& a, const Note& b){ return a.time_ms < b.time_ms; });

    // Sort timing points by time
    std::sort(out.timing_points.begin(), out.timing_points.end(),
        [](const TimingPoint& a, const TimingPoint& b){ return a.time_ms < b.time_ms; });

    out.loaded = true;
    return true;
}

// ─── .ensou parser ────────────────────────────────────────────────────────────
// Format is a superset of .osu with an additional [EnsouNotes] section.
//
// [EnsouNotes] replaces [HitObjects] for native maps. Each line:
//   column,time_ms,end_ms,note_type[,custom_data_hex]
//
// note_type is the numeric value of NoteType.
// custom_data_hex is an optional 16-char hex string (8 bytes).
//
// All other sections ([General], [Metadata], [Difficulty], [TimingPoints])
// are identical to .osu so we reuse the osu parser for those, then
// override the notes from [EnsouNotes].

bool BeatmapParser::ParseEnsou(const std::string& path, Beatmap& out, std::string& out_error) {
    // First pass: parse everything as if it were an osu file (gets us metadata
    // and timing points). We intentionally ignore the Mode check here.
    std::ifstream f(path);
    if (!f.is_open()) {
        out_error = "Cannot open file: " + path;
        return false;
    }

    out = Beatmap{};
    out.source_path = path;

    std::string base_dir = path;
    {
        size_t slash = base_dir.find_last_of("/\\");
        if (slash != std::string::npos) base_dir = base_dir.substr(0, slash + 1);
        else base_dir = "./";
    }

    std::string line;
    std::string section;
    bool has_ensou_notes = false;

    while (std::getline(f, line)) {
        line = Trim(line);
        if (line.empty() || StartsWith(line, "//")) continue;

        std::string sec = GetSection(line);
        if (!sec.empty()) { section = sec; continue; }

        if (section == "general") {
            auto [k, v] = SplitFirst(line, ':');
            if      (k == "AudioFilename") out.audio_path = base_dir + v;
            else if (k == "PreviewTime")   out.preview_ms = ToInt(v);
        }
        else if (section == "events") {
            ParseEventBackground(line, base_dir, out.background_path);
        }
        else if (section == "metadata") {
            auto [k, v] = SplitFirst(line, ':');
            if      (k == "Title")          out.title          = v;
            else if (k == "TitleUnicode")   out.title_unicode  = v;
            else if (k == "Artist")         out.artist         = v;
            else if (k == "ArtistUnicode")  out.artist_unicode = v;
            else if (k == "Creator")        out.creator        = v;
            else if (k == "Version")        out.difficulty.name = v;
            else if (k == "Tags")           out.tags           = v;
        }
        else if (section == "difficulty") {
            auto [k, v] = SplitFirst(line, ':');
            if      (k == "CircleSize")        out.difficulty.key_count = (int)ToFloat(v);
            else if (k == "HPDrainRate")       out.difficulty.hp_drain  = ToFloat(v);
            else if (k == "OverallDifficulty") out.difficulty.od        = ToFloat(v);
        }
        else if (section == "timingpoints") {
            auto parts = Split(line, ',');
            if (parts.size() < 8) continue;

            TimingPoint tp{};
            tp.time_ms   = (int)ToFloat(parts[0]);
            float bl     = ToFloat(parts[1]);
            tp.meter     = ToInt(parts[2]);
            tp.inherited = (parts[6] == "0");

            if (!tp.inherited) {
                tp.beat_length   = bl;
                tp.bpm           = 60000.0f / bl;
                tp.sv_multiplier = 1.0f;
            } else {
                tp.beat_length   = 0.0f;
                tp.bpm           = 0.0f;
                tp.sv_multiplier = (bl < 0.0f) ? (-100.0f / bl) : 1.0f;
            }
            out.timing_points.push_back(tp);
        }
        else if (section == "ensounotes") {
            // column,time_ms,end_ms,note_type[,custom_hex]
            has_ensou_notes = true;
            auto parts = Split(line, ',');
            if (parts.size() < 4) continue;

            Note note{};
            note.column   = ToInt(parts[0]);
            note.time_ms  = ToInt(parts[1]);
            note.end_ms   = ToInt(parts[2]);
            note.type     = (NoteType)(uint8_t)ToInt(parts[3]);

            if (parts.size() >= 5 && parts[4].size() == 16) {
                // Parse 8 bytes of hex custom data
                const std::string& hex = parts[4];
                for (int i = 0; i < 8; i++) {
                    std::string byte_str = hex.substr(i * 2, 2);
                    note.custom_data[i] = (uint8_t)std::stoul(byte_str, nullptr, 16);
                }
            }

            out.notes.push_back(note);
        }
        else if (section == "hitobjects" && !has_ensou_notes) {
            // Fall back to osu hit objects if no [EnsouNotes] section present
            auto parts = Split(line, ',');
            if (parts.size() < 5) continue;

            int x    = ToInt(parts[0]);
            int time = ToInt(parts[2]);
            int type = ToInt(parts[3]);

            Note note{};
            note.column  = OsuXToColumn(x, out.difficulty.key_count);
            note.time_ms = time;

            if (type & OSU_TYPE_HOLD) {
                note.type = NoteType::HOLD;
                if (parts.size() >= 6) {
                    auto [end_str, _] = SplitFirst(parts[5], ':');
                    note.end_ms = ToInt(end_str);
                } else {
                    note.end_ms = time;
                }
            } else {
                note.type   = NoteType::NORMAL;
                note.end_ms = time;
            }

            out.notes.push_back(note);
        }
    }

    if (out.title.empty()) {
        out_error = "Missing metadata in: " + path;
        return false;
    }

    std::sort(out.notes.begin(), out.notes.end(),
        [](const Note& a, const Note& b){ return a.time_ms < b.time_ms; });
    std::sort(out.timing_points.begin(), out.timing_points.end(),
        [](const TimingPoint& a, const TimingPoint& b){ return a.time_ms < b.time_ms; });

    out.loaded = true;
    return true;
}

// ─── Metadata-only fast path ──────────────────────────────────────────────────
// Reads only [General], [Metadata], [Difficulty] — stops as soon as it hits
// [TimingPoints] or [HitObjects] or [EnsouNotes].

bool BeatmapParser::ParseMetadataOnly(const std::string& path, Beatmap& out, std::string& out_error) {
    std::ifstream f(path);
    if (!f.is_open()) {
        out_error = "Cannot open file: " + path;
        return false;
    }

    out = Beatmap{};
    out.source_path = path;

    std::string base_dir = path;
    {
        size_t slash = base_dir.find_last_of("/\\");
        if (slash != std::string::npos) base_dir = base_dir.substr(0, slash + 1);
        else base_dir = "./";
    }

    std::string line;
    std::string section;
    bool mode_found  = false;
    bool mode_is_mania = false;

    while (std::getline(f, line)) {
        line = Trim(line);
        if (line.empty() || StartsWith(line, "//")) continue;

        std::string sec = GetSection(line);
        if (!sec.empty()) {
            // Stop reading once we hit data-heavy sections
            if (sec == "timingpoints" || sec == "hitobjects" || sec == "ensounotes")
                break;
            section = sec;
            continue;
        }

        if (section == "general") {
            auto [k, v] = SplitFirst(line, ':');
            if      (k == "AudioFilename") out.audio_path = base_dir + v;
            else if (k == "PreviewTime")   out.preview_ms = ToInt(v);
            else if (k == "Mode") { mode_found = true; mode_is_mania = (v == "3"); }
        }
        else if (section == "events") {
            ParseEventBackground(line, base_dir, out.background_path);
        }
        else if (section == "metadata") {
            auto [k, v] = SplitFirst(line, ':');
            if      (k == "Title")          out.title          = v;
            else if (k == "TitleUnicode")   out.title_unicode  = v;
            else if (k == "Artist")         out.artist         = v;
            else if (k == "ArtistUnicode")  out.artist_unicode = v;
            else if (k == "Creator")        out.creator        = v;
            else if (k == "Version")        out.difficulty.name = v;
            else if (k == "Tags")           out.tags           = v;
        }
        else if (section == "difficulty") {
            auto [k, v] = SplitFirst(line, ':');
            if      (k == "CircleSize")        out.difficulty.key_count = (int)ToFloat(v);
            else if (k == "HPDrainRate")       out.difficulty.hp_drain  = ToFloat(v);
            else if (k == "OverallDifficulty") out.difficulty.od        = ToFloat(v);
        }
    }

    // Reject non-mania .osu files (mode_found=false means no Mode key = osu! standard)
    if (mode_found && !mode_is_mania) {
        out_error = "Not an osu!mania beatmap";
        return false;
    }
    if (!mode_found) {
        out_error = "Not an osu!mania beatmap (no Mode key — defaults to osu! standard)";
        return false;
    }
    if (out.title.empty()) {
        out_error = "No title found in: " + path;
        return false;
    }

    out.loaded = false; // metadata only — notes not loaded
    return true;
}

// ─── Dispatch ─────────────────────────────────────────────────────────────────

bool BeatmapParser::Parse(const std::string& path, Beatmap& out, std::string& out_error) {
    // Determine format by extension
    std::string ext;
    size_t dot = path.rfind('.');
    if (dot != std::string::npos)
        ext = ToLower(path.substr(dot));

    if (ext == ".osu")    return ParseOsu(path, out, out_error);
    if (ext == ".ensou")  return ParseEnsou(path, out, out_error);

    out_error = "Unknown beatmap format: " + path;
    return false;
}