#include "screen_beatmaps.h"
#include <fcntl.h>
#include "ui.h"
#include "ui/navbar.h"
#include "screens.h"
#include "beatmap/downloader.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>

extern Screen           current_screen;
extern ScreenTransition screen_transition;

// ─── Widget IDs (200-299) ─────────────────────────────────────────────────────
enum BeatmapIDs {
    BTN_SEARCH       = 200,
    BTN_STATUS_ALL,
    BTN_STATUS_RANKED,
    BTN_STATUS_LOVED,
    BTN_STATUS_QUALIFIED,
    BTN_KEYS_ANY,
    BTN_KEYS_4K,
    BTN_KEYS_7K,
    BTN_LOAD_MORE,
    BTN_DETAIL_BACK,
    BTN_DETAIL_DOWNLOAD,
    BTN_DETAIL_CANCEL,
    // Cards: 220 + index (up to 40 cards)
    CARD_BASE        = 220,
};

// ─── Data structures ──────────────────────────────────────────────────────────

struct BeatmapDiff {
    std::string version;
    float       star_rating = 0.0f;
    float       cs          = 0.0f;   // key count for mania
    int         total_length = 0;     // seconds
};

struct BeatmapSet {
    int         id          = 0;
    std::string title;
    std::string title_unicode;
    std::string artist;
    std::string artist_unicode;
    std::string creator;
    float       bpm         = 0.0f;
    std::string status;     // "ranked", "loved", "qualified", "pending", etc.
    std::string cover_url;
    std::vector<BeatmapDiff> diffs; // mania only
};

// ─── Search state machine ─────────────────────────────────────────────────────
enum class SearchState { IDLE, SEARCHING, RESULTS, ERROR };

// ─── Sub-view ─────────────────────────────────────────────────────────────────
enum class BeatmapView { LIST, DETAIL };

// ─── Minimal JSON helpers ─────────────────────────────────────────────────────

// Returns the raw value string (no quotes) for a given string key, or "".
static std::string FindJsonString(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return "";
    pos += needle.size();
    // skip whitespace and colon
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == ':')) ++pos;
    if (pos >= json.size()) return "";
    if (json[pos] == '"') {
        // quoted string
        ++pos;
        std::string result;
        while (pos < json.size() && json[pos] != '"') {
            if (json[pos] == '\\' && pos + 1 < json.size()) {
                char esc = json[pos + 1];
                if      (esc == '"')  result += '"';
                else if (esc == '\\') result += '\\';
                else if (esc == '/')  result += '/';
                else if (esc == 'n')  result += '\n';
                else if (esc == 't')  result += '\t';
                else if (esc == 'r')  result += '\r';
                else                  result += esc;
                pos += 2;
            } else {
                result += json[pos++];
            }
        }
        return result;
    } else if (json[pos] == 'n' && json.substr(pos, 4) == "null") {
        return "";
    }
    return "";
}

// Returns float for a numeric key, or 0.
static float FindJsonFloat(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return 0.0f;
    pos += needle.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == ':')) ++pos;
    if (pos >= json.size()) return 0.0f;
    if (json[pos] == '"') {
        // sometimes numbers come as strings
        ++pos;
        size_t end = json.find('"', pos);
        if (end == std::string::npos) return 0.0f;
        return (float)atof(json.substr(pos, end - pos).c_str());
    }
    return (float)atof(json.c_str() + pos);
}

static int FindJsonInt(const std::string& json, const std::string& key) {
    return (int)FindJsonFloat(json, key);
}

// Find the raw JSON value (object, array, string, number) for a key.
// Returns the substring starting at the value, up to (but not including) the
// matching closing bracket/quote, or "" on failure.
// For objects/arrays, returns the full balanced substring including delimiters.
static std::string FindJsonValue(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return "";
    pos += needle.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == ':')) ++pos;
    if (pos >= json.size()) return "";

    char open = json[pos];
    if (open == '{' || open == '[') {
        char close = (open == '{') ? '}' : ']';
        int depth = 0;
        size_t start = pos;
        bool in_str = false;
        while (pos < json.size()) {
            char c = json[pos];
            if (in_str) {
                if (c == '\\') { ++pos; }
                else if (c == '"') in_str = false;
            } else {
                if (c == '"')  in_str = true;
                else if (c == open)  ++depth;
                else if (c == close) { --depth; if (depth == 0) { ++pos; return json.substr(start, pos - start); } }
            }
            ++pos;
        }
        return "";
    }
    // For strings / numbers just return FindJsonString / raw
    if (open == '"') {
        ++pos;
        std::string result;
        while (pos < json.size() && json[pos] != '"') {
            if (json[pos] == '\\' && pos + 1 < json.size()) { ++pos; }
            result += json[pos++];
        }
        return result;
    }
    // number / bool / null
    size_t start = pos;
    while (pos < json.size() && json[pos] != ',' && json[pos] != '}' && json[pos] != ']' && json[pos] != '\n') ++pos;
    return json.substr(start, pos - start);
}

// Iterate top-level objects inside a JSON array string (the "[...]" itself).
// Calls cb(object_json) for each top-level object.
static void IterJsonArray(const std::string& arr, const std::function<void(const std::string&)>& cb) {
    if (arr.empty() || arr[0] != '[') return;
    size_t pos = 1;
    while (pos < arr.size()) {
        // skip whitespace and commas
        while (pos < arr.size() && (arr[pos] == ' ' || arr[pos] == '\t' ||
               arr[pos] == '\r' || arr[pos] == '\n' || arr[pos] == ',')) ++pos;
        if (pos >= arr.size() || arr[pos] == ']') break;

        if (arr[pos] == '{') {
            int depth = 0;
            size_t start = pos;
            bool in_str = false;
            while (pos < arr.size()) {
                char c = arr[pos];
                if (in_str) {
                    if (c == '\\') ++pos;
                    else if (c == '"') in_str = false;
                } else {
                    if (c == '"')  in_str = true;
                    else if (c == '{') ++depth;
                    else if (c == '}') { --depth; if (depth == 0) { ++pos; break; } }
                }
                ++pos;
            }
            cb(arr.substr(start, pos - start));
        } else {
            ++pos;
        }
    }
}

// ─── URL encode ───────────────────────────────────────────────────────────────
static std::string UrlEncode(const std::string& s) {
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            char buf[4];
            snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

// ─── Parse a full search response ─────────────────────────────────────────────
static std::vector<BeatmapSet> ParseSearchResponse(const std::string& json,
                                                    std::string& out_cursor) {
    std::vector<BeatmapSet> results;

    out_cursor = FindJsonString(json, "cursor_string");

    std::string sets_arr = FindJsonValue(json, "beatmapsets");
    if (sets_arr.empty()) return results;

    IterJsonArray(sets_arr, [&](const std::string& obj) {
        BeatmapSet bs;
        bs.id             = FindJsonInt(obj, "id");
        bs.title          = FindJsonString(obj, "title");
        bs.title_unicode  = FindJsonString(obj, "title_unicode");
        bs.artist         = FindJsonString(obj, "artist");
        bs.artist_unicode = FindJsonString(obj, "artist_unicode");
        bs.creator        = FindJsonString(obj, "creator");
        bs.bpm            = FindJsonFloat(obj, "bpm");
        bs.status         = FindJsonString(obj, "status");

        // covers.list
        std::string covers = FindJsonValue(obj, "covers");
        if (!covers.empty()) {
            bs.cover_url = FindJsonString(covers, "list");
            if (bs.cover_url.empty())
                bs.cover_url = FindJsonString(covers, "card");
        }

        // beatmaps array — keep only mania diffs
        std::string bmaps_arr = FindJsonValue(obj, "beatmaps");
        if (!bmaps_arr.empty()) {
            IterJsonArray(bmaps_arr, [&](const std::string& bobj) {
                std::string mode = FindJsonString(bobj, "mode");
                int mode_int     = FindJsonInt(bobj, "mode_int");
                if (mode != "mania" && mode_int != 3) return;

                BeatmapDiff d;
                d.version      = FindJsonString(bobj, "version");
                d.star_rating  = FindJsonFloat(bobj, "difficulty_rating");
                d.cs           = FindJsonFloat(bobj, "cs");
                d.total_length = FindJsonInt(bobj, "total_length");
                bs.diffs.push_back(d);
            });
        }

        // Sort diffs by star rating ascending
        std::sort(bs.diffs.begin(), bs.diffs.end(),
                  [](const BeatmapDiff& a, const BeatmapDiff& b) {
                      return a.star_rating < b.star_rating;
                  });

        if (bs.id != 0)
            results.push_back(std::move(bs));
    });

    return results;
}

// ─── Color helpers for status badges ─────────────────────────────────────────
static Color StatusColor(const std::string& status) {
    if (status == "ranked")    return UI_ACCENT;
    if (status == "loved")     return Color{255, 102, 170, 255};
    if (status == "qualified") return Color{100, 180, 255, 255};
    if (status == "approved")  return Color{100, 220, 130, 255};
    return UI_TEXT_MUTED;
}

static Color StarColor(float stars) {
    if (stars < 2.5f) return Color{100, 200, 255, 255};
    if (stars < 4.0f) return UI_ACCENT;
    if (stars < 5.5f) return Color{255, 200, 80,  255};
    if (stars < 7.0f) return Color{255, 130, 80,  255};
    return Color{255, 80,  80,  255};
}

// ─── Truncate text to fit width ───────────────────────────────────────────────
static std::string TruncateText(Font font, const std::string& text,
                                 float max_width, float font_size, float spacing) {
    Vector2 sz = MeasureTextEx(font, text.c_str(), font_size, spacing);
    if (sz.x <= max_width) return text;

    std::string t = text;
    while (!t.empty()) {
        t.pop_back();
        std::string candidate = t + "...";
        sz = MeasureTextEx(font, candidate.c_str(), font_size, spacing);
        if (sz.x <= max_width) return candidate;
    }
    return "...";
}

// ─── Format seconds as m:ss ───────────────────────────────────────────────────


// ─── Screen state (all static) ────────────────────────────────────────────────

static SearchState          search_state   = SearchState::IDLE;
static BeatmapView          view           = BeatmapView::LIST;

// Search inputs
static char                 search_buf[256] = {};
static bool                 search_editing  = false;
static int                  status_filter   = 0; // 0=all,1=ranked,2=loved,3=qualified
static int                  keys_filter     = 0; // 0=any,1=4K,2=7K

// Async search
static FILE*                search_pipe     = nullptr;
static std::string          search_accum;   // accumulated popen output

// Results
static std::vector<BeatmapSet> results;
static std::string          cursor_string;
static bool                 has_more        = false;
static std::string          error_msg;

// Scroll
static float                scroll_y        = 0.0f;
static float                scroll_target   = 0.0f;

// Detail view
static int                  selected_idx    = -1;
static Anim                 detail_slide;   // 0=list, 1=detail

// Spinner
static float                spinner_angle   = 0.0f;

// ─── Build search URL ─────────────────────────────────────────────────────────
static std::string BuildSearchUrl(const std::string& query,
                                  int status_f, int /*keys_f*/,
                                  const std::string& cursor) {
    std::string url = "https://osu.ppy.sh/api/v2/beatmapsets/search?m=3";

    if (!query.empty())
        url += "&q=" + UrlEncode(query);

    if (status_f == 1) url += "&s=ranked";
    else if (status_f == 2) url += "&s=loved";
    else if (status_f == 3) url += "&s=qualified";
    // 0 = all: omit s param (returns everything)

    if (!cursor.empty())
        url += "&cursor_string=" + UrlEncode(cursor);

    return url;
}

// ─── Start async search ───────────────────────────────────────────────────────
static void StartSearch(bool append = false) {
    if (search_pipe) { pclose(search_pipe); search_pipe = nullptr; }
    search_accum.clear();

    if (!append) {
        results.clear();
        cursor_string.clear();
        scroll_y = scroll_target = 0.0f;
    }

    std::string url = BuildSearchUrl(search_buf, status_filter, keys_filter,
                                     append ? cursor_string : "");

    std::string cmd =
        "curl -s -S "
        "-H \"Authorization: Bearer " + osu_downloader.token + "\" "
        "-H \"Content-Type: application/json\" "
        "-H \"Accept: application/json\" "
        "\"" + url + "\" 2>&1";

    search_pipe = popen(cmd.c_str(), "r");
    if (!search_pipe) {
        search_state = SearchState::ERROR;
        error_msg    = "Failed to launch curl for search.";
        return;
    }

    // Make the pipe non-blocking so DrainSearchPipe never stalls a frame
    int fd = fileno(search_pipe);
    if (fd >= 0)
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

    TraceLog(LOG_INFO, "BeatmapSearch: starting search, url=%s", url.c_str());
    TraceLog(LOG_INFO, "BeatmapSearch: curl cmd=%s", cmd.c_str());
    search_state = SearchState::SEARCHING;
    spinner_angle = 0.0f;
}

// ─── Drain search pipe each frame ─────────────────────────────────────────────
static void DrainSearchPipe() {
    if (!search_pipe) return;
    TraceLog(LOG_INFO, "BeatmapSearch: draining pipe...");

    // Read whatever bytes are available right now without blocking.
    // On a non-blocking fd, fgets returns NULL with errno==EAGAIN when there
    // is nothing yet — we just come back next frame.
    char buf[4096];
    while (true) {
        errno = 0;
        if (fgets(buf, sizeof(buf), search_pipe) == nullptr) {
            // EAGAIN / EWOULDBLOCK means no data yet — try again next frame
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            // Any other NULL (including EOF) falls through to the feof check
            break;
        }
        search_accum += buf;
    }

    if (feof(search_pipe)) {
        pclose(search_pipe);
        search_pipe = nullptr;

        if (search_accum.empty()) {
            search_state = SearchState::ERROR;
            error_msg    = "Empty response from server.";
            return;
        }

        // Quick sanity check — must look like JSON
        size_t brace = search_accum.find('{');
        if (brace == std::string::npos) {
            search_state = SearchState::ERROR;
            error_msg    = "Unexpected response: " + search_accum.substr(0, 120);
            return;
        }

        TraceLog(LOG_INFO, "BeatmapSearch: raw response (%d bytes): %.512s",
                 (int)search_accum.size(), search_accum.c_str());

        std::string json = search_accum.substr(brace);

        std::string new_cursor;
        std::vector<BeatmapSet> new_results = ParseSearchResponse(json, new_cursor);

        // Apply key filter client-side (API m=3 already filters mania, but
        // we may want to further filter by specific key count)
        if (keys_filter != 0) {
            float target_cs = (keys_filter == 1) ? 4.0f : 7.0f;
            for (auto& bs : new_results) {
                bs.diffs.erase(
                    std::remove_if(bs.diffs.begin(), bs.diffs.end(),
                        [target_cs](const BeatmapDiff& d) {
                            return fabsf(d.cs - target_cs) > 0.5f;
                        }),
                    bs.diffs.end());
            }
            new_results.erase(
                std::remove_if(new_results.begin(), new_results.end(),
                    [](const BeatmapSet& bs) { return bs.diffs.empty(); }),
                new_results.end());
        }

        for (auto& bs : new_results)
            results.push_back(std::move(bs));

        cursor_string = new_cursor;
        has_more      = !new_cursor.empty();
        search_state  = SearchState::RESULTS;

        TraceLog(LOG_INFO, "BeatmapSearch: parsed %d results, cursor='%s', has_more=%s",
                 (int)results.size(), cursor_string.c_str(), has_more ? "true" : "false");
    }
}

// ─── Draw a single result card ────────────────────────────────────────────────
// Returns true if clicked.
static bool DrawResultCard(int id, Rectangle bounds, const BeatmapSet& bs, bool selected) {
    auto& ws  = ui.states[id % MAX_WIDGET_STATES];
    bool  hov = CheckCollisionPointRec(GetMousePosition(), bounds);
    bool  clk = hov && IsMouseButtonPressed(MOUSE_LEFT_BUTTON);

    ws.hover.target = (hov || selected) ? 1.0f : 0.0f;
    ws.hover.speed  = 12.0f;
    if (clk) ws.scale.Set(0.97f);
    ws.scale.target = 1.0f;

    float sc = (ws.scale.pos > 0.01f) ? ws.scale.pos : 1.0f;
    float dw = bounds.width  * (1.0f - sc);
    float dh = bounds.height * (1.0f - sc);
    Rectangle r = { bounds.x + dw * 0.5f, bounds.y + dh * 0.5f,
                    bounds.width - dw,     bounds.height - dh };

    // Card background
    Color bg = selected
        ? UI_SURFACE2
        : ColorLerp(UI_SURFACE, UI_SURFACE2, ws.hover.value);
    DrawRectRounded(r, 10.0f, bg);
    DrawRectRoundedBorder(r, 10.0f, 1.5f,
        selected ? ColorAlpha(UI_ACCENT, 0.55f)
                 : ColorLerp(UI_BORDER, ColorAlpha(UI_ACCENT, 0.35f), ws.hover.value));

    // Accent left bar
    float bw = 3.0f + ws.hover.value * 1.5f;
    DrawRectRounded({r.x, r.y + 8.0f, bw, r.height - 16.0f}, bw,
        selected ? UI_ACCENT : ColorAlpha(UI_ACCENT, ws.hover.value));

    // Cover placeholder — colored rect with first letter
    float cover_x = r.x + 12.0f;
    float cover_y = r.y + 10.0f;
    float cover_w = 60.0f;
    float cover_h = r.height - 20.0f;
    Color cover_col = ColorAlpha(StatusColor(bs.status), 0.25f + ws.hover.value * 0.1f);
    DrawRectRounded({cover_x, cover_y, cover_w, cover_h}, 6.0f, cover_col);
    DrawRectRoundedBorder({cover_x, cover_y, cover_w, cover_h}, 6.0f, 1.0f,
                          ColorAlpha(StatusColor(bs.status), 0.4f));
    if (!bs.title.empty()) {
        char letter[2] = { (char)toupper((unsigned char)bs.title[0]), '\0' };
        DrawTextCentered(ui.font_heading, letter,
                         cover_x + cover_w * 0.5f, cover_y + cover_h * 0.5f - 14.0f,
                         22.0f, 0.5f, ColorAlpha(StatusColor(bs.status), 0.9f));
    }

    // Text area
    float tx = cover_x + cover_w + 12.0f;
    float available_w = r.x + r.width - tx - 160.0f; // leave room for right side

    std::string title_disp  = TruncateText(ui.font_heading, bs.title,  available_w, 16.0f, 0.3f);
    std::string artist_disp = TruncateText(ui.font_body,    bs.artist, available_w, 12.0f, 0.3f);
    std::string creator_disp = "mapped by " + bs.creator;

    DrawTextEx(ui.font_heading, title_disp.c_str(),
               {tx, r.y + 10.0f}, 16.0f, 0.3f,
               selected ? UI_ACCENT : ColorLerp(UI_TEXT, UI_ACCENT, ws.hover.value * 0.3f));
    DrawTextEx(ui.font_body, artist_disp.c_str(),
               {tx, r.y + 30.0f}, 12.0f, 0.3f, UI_TEXT_MUTED);
    DrawTextEx(ui.font_body, creator_disp.c_str(),
               {tx, r.y + 48.0f}, 11.0f, 0.3f, ColorAlpha(UI_TEXT_MUTED, 0.7f));

    // Right side info
    float rx = r.x + r.width - 12.0f;

    // Status badge
    std::string status_upper = bs.status;
    for (auto& c : status_upper) c = (char)toupper((unsigned char)c);
    Color sc_col = StatusColor(bs.status);
    Vector2 badge_sz = MeasureTextEx(ui.font_body, status_upper.c_str(), 10.0f, 1.0f);
    float badge_w = badge_sz.x + 12.0f;
    float badge_x = rx - badge_w;
    DrawRectRounded({badge_x, r.y + 10.0f, badge_w, 16.0f}, 4.0f, ColorAlpha(sc_col, 0.18f));
    DrawRectRoundedBorder({badge_x, r.y + 10.0f, badge_w, 16.0f}, 4.0f, 1.0f, ColorAlpha(sc_col, 0.5f));
    DrawTextCentered(ui.font_body, status_upper.c_str(),
                     badge_x + badge_w * 0.5f, r.y + 11.0f, 10.0f, 1.0f, sc_col);

    // BPM
    char bpm_buf[32];
    snprintf(bpm_buf, sizeof(bpm_buf), "%.0f BPM", bs.bpm);
    DrawTextRight(ui.font_body, bpm_buf, rx, r.y + 32.0f, 12.0f, 0.3f, UI_TEXT_MUTED);

    // Top star rating
    if (!bs.diffs.empty()) {
        float top_stars = bs.diffs.back().star_rating;
        char star_buf[32];
        snprintf(star_buf, sizeof(star_buf), "%.2f*", top_stars);
        DrawTextRight(ui.font_body, star_buf, rx, r.y + 48.0f, 12.0f, 0.3f, StarColor(top_stars));
    }

    // Key count badges
    bool has_4k = false, has_7k = false;
    for (const auto& d : bs.diffs) {
        if (fabsf(d.cs - 4.0f) < 0.5f) has_4k = true;
        if (fabsf(d.cs - 7.0f) < 0.5f) has_7k = true;
    }
    float kbx = rx;
    if (has_7k) {
        float kbw = 28.0f;
        kbx -= kbw + 2.0f;
        DrawRectRounded({kbx, r.y + 64.0f, kbw, 14.0f}, 3.0f, ColorAlpha(UI_ACCENT, 0.15f));
        DrawRectRoundedBorder({kbx, r.y + 64.0f, kbw, 14.0f}, 3.0f, 1.0f, ColorAlpha(UI_ACCENT, 0.4f));
        DrawTextCentered(ui.font_body, "7K", kbx + kbw * 0.5f, r.y + 65.0f, 9.0f, 0.5f, UI_ACCENT);
    }
    if (has_4k) {
        float kbw = 28.0f;
        kbx -= kbw + 2.0f;
        DrawRectRounded({kbx, r.y + 64.0f, kbw, 14.0f}, 3.0f, ColorAlpha(UI_ACCENT, 0.15f));
        DrawRectRoundedBorder({kbx, r.y + 64.0f, kbw, 14.0f}, 3.0f, 1.0f, ColorAlpha(UI_ACCENT, 0.4f));
        DrawTextCentered(ui.font_body, "4K", kbx + kbw * 0.5f, r.y + 65.0f, 9.0f, 0.5f, UI_ACCENT);
    }

    return clk;
}

// ─── Draw detail panel ────────────────────────────────────────────────────────
static void DrawDetailPanel(float x, float y, float w, float h, const BeatmapSet& bs) {
    // Background panel
    DrawRectRounded({x, y, w, h}, 12.0f, UI_SURFACE);
    DrawRectRoundedBorder({x, y, w, h}, 12.0f, 1.5f, UI_BORDER);

    float px = x + 20.0f;
    float py = y + 20.0f;

    // Cover placeholder (larger)
    float cover_w = w - 40.0f;
    float cover_h = 120.0f;
    Color cover_col = ColorAlpha(StatusColor(bs.status), 0.3f);
    DrawRectRounded({px, py, cover_w, cover_h}, 8.0f, cover_col);
    DrawRectRoundedBorder({px, py, cover_w, cover_h}, 8.0f, 1.5f,
                          ColorAlpha(StatusColor(bs.status), 0.5f));

    // Big first letter
    if (!bs.title.empty()) {
        char letter[2] = { (char)toupper((unsigned char)bs.title[0]), '\0' };
        DrawTextCentered(ui.font_heading, letter,
                         px + cover_w * 0.5f, py + cover_h * 0.5f - 24.0f,
                         48.0f, 0.5f, ColorAlpha(StatusColor(bs.status), 0.7f));
    }

    py += cover_h + 16.0f;

    // Title
    std::string title_disp = TruncateText(ui.font_heading, bs.title, w - 40.0f, 18.0f, 0.3f);
    DrawTextEx(ui.font_heading, title_disp.c_str(), {px, py}, 18.0f, 0.3f, UI_TEXT);
    py += 24.0f;

    // Artist
    std::string artist_disp = TruncateText(ui.font_body, bs.artist, w - 40.0f, 13.0f, 0.3f);
    DrawTextEx(ui.font_body, artist_disp.c_str(), {px, py}, 13.0f, 0.3f, UI_TEXT_MUTED);
    py += 18.0f;

    // Creator
    std::string creator_str = "by " + bs.creator;
    DrawTextEx(ui.font_body, creator_str.c_str(), {px, py}, 12.0f, 0.3f,
               ColorAlpha(UI_TEXT_MUTED, 0.7f));
    py += 20.0f;

    // Info row: BPM + status
    char bpm_buf[32];
    snprintf(bpm_buf, sizeof(bpm_buf), "%.0f BPM", bs.bpm);
    DrawTextEx(ui.font_body, bpm_buf, {px, py}, 12.0f, 0.3f, UI_TEXT_MUTED);

    std::string status_upper = bs.status;
    for (auto& c : status_upper) c = (char)toupper((unsigned char)c);
    Color sc_col = StatusColor(bs.status);
    Vector2 badge_sz = MeasureTextEx(ui.font_body, status_upper.c_str(), 10.0f, 1.0f);
    float badge_w = badge_sz.x + 12.0f;
    float badge_x = x + w - 20.0f - badge_w;
    DrawRectRounded({badge_x, py - 1.0f, badge_w, 16.0f}, 4.0f, ColorAlpha(sc_col, 0.18f));
    DrawRectRoundedBorder({badge_x, py - 1.0f, badge_w, 16.0f}, 4.0f, 1.0f, ColorAlpha(sc_col, 0.5f));
    DrawTextCentered(ui.font_body, status_upper.c_str(),
                     badge_x + badge_w * 0.5f, py, 10.0f, 1.0f, sc_col);
    py += 22.0f;

    // Divider
    DrawLineEx({px, py}, {x + w - 20.0f, py}, 1.0f, UI_BORDER);
    py += 10.0f;

    // Difficulties header
    DrawTextEx(ui.font_body, "DIFFICULTIES", {px, py}, 11.0f, 2.0f, UI_TEXT_MUTED);
    py += 18.0f;

    // Diff list (scrollable area — fixed height, clip manually)
    float diff_area_h = 160.0f;
    float diff_item_h = 28.0f;
    float diff_y = py;

    // Simple scissor-like: just draw up to what fits
    int max_diffs = (int)(diff_area_h / diff_item_h);
    int shown = 0;
    for (const auto& d : bs.diffs) {
        if (shown >= max_diffs) break;

        float iy = diff_y + shown * diff_item_h;
        bool row_hov = CheckCollisionPointRec(GetMousePosition(),
                                              {px, iy, w - 40.0f, diff_item_h - 2.0f});
        Color row_bg = row_hov ? ColorAlpha(UI_ACCENT, 0.07f) : ColorAlpha(UI_SURFACE2, 0.5f);
        DrawRectRounded({px, iy, w - 40.0f, diff_item_h - 2.0f}, 4.0f, row_bg);

        // Star color dot
        Color dot_col = StarColor(d.star_rating);
        DrawCircleV({px + 10.0f, iy + diff_item_h * 0.5f - 1.0f}, 4.0f, dot_col);

        // Version name
        std::string ver_disp = TruncateText(ui.font_body, d.version, w - 180.0f, 12.0f, 0.3f);
        DrawTextEx(ui.font_body, ver_disp.c_str(),
                   {px + 20.0f, iy + diff_item_h * 0.5f - 7.0f}, 12.0f, 0.3f, UI_TEXT);

        // Star rating
        char star_buf[16];
        snprintf(star_buf, sizeof(star_buf), "%.2f*", d.star_rating);
        DrawTextRight(ui.font_body, star_buf,
                      x + w - 80.0f, iy + diff_item_h * 0.5f - 7.0f, 11.0f, 0.3f, dot_col);

        // Key count
        char key_buf[8];
        snprintf(key_buf, sizeof(key_buf), "%.0fK", d.cs);
        DrawTextRight(ui.font_body, key_buf,
                      x + w - 20.0f, iy + diff_item_h * 0.5f - 7.0f, 11.0f, 0.3f, UI_TEXT_MUTED);

        ++shown;
    }

    if ((int)bs.diffs.size() > max_diffs) {
        char more_buf[32];
        snprintf(more_buf, sizeof(more_buf), "+%d more...", (int)bs.diffs.size() - max_diffs);
        DrawTextEx(ui.font_body, more_buf,
                   {px, diff_y + max_diffs * diff_item_h}, 11.0f, 0.3f,
                   ColorAlpha(UI_TEXT_MUTED, 0.6f));
    }

    py += diff_area_h + 8.0f;

    // Divider
    DrawLineEx({px, py}, {x + w - 20.0f, py}, 1.0f, UI_BORDER);
    py += 12.0f;

    // Download status
    const auto& dl = osu_downloader.Status();
    bool is_this_set = (dl.beatmapset_id == bs.id);

    if (is_this_set && dl.state != DownloadStatus::State::IDLE) {
        Color msg_col = (dl.state == DownloadStatus::State::ERROR)  ? UI_DANGER
                      : (dl.state == DownloadStatus::State::DONE)   ? UI_ACCENT
                      : UI_TEXT_MUTED;

        std::string msg_disp = TruncateText(ui.font_body, dl.message, w - 40.0f, 12.0f, 0.3f);
        DrawTextEx(ui.font_body, msg_disp.c_str(), {px, py}, 12.0f, 0.3f, msg_col);
        py += 18.0f;

        // Progress bar for downloading/extracting
        if (dl.state == DownloadStatus::State::DOWNLOADING ||
            dl.state == DownloadStatus::State::EXTRACTING) {
            // Indeterminate spinner bar
            float bar_w = w - 40.0f;
            DrawRectRounded({px, py, bar_w, 4.0f}, 2.0f, UI_SURFACE2);
            float t = (float)GetTime();
            float seg_start = fmodf(t * 0.6f, 1.0f);
            float seg_len   = 0.35f;
            float seg_end   = seg_start + seg_len;
            if (seg_end <= 1.0f) {
                DrawRectRounded({px + bar_w * seg_start, py, bar_w * seg_len, 4.0f},
                                2.0f, UI_ACCENT);
            } else {
                DrawRectRounded({px + bar_w * seg_start, py, bar_w * (1.0f - seg_start), 4.0f},
                                2.0f, UI_ACCENT);
                DrawRectRounded({px, py, bar_w * (seg_end - 1.0f), 4.0f},
                                2.0f, UI_ACCENT);
            }
            py += 12.0f;
        }
    }

    // Buttons
    float btn_y = y + h - 60.0f;
    float btn_w = (w - 52.0f) * 0.5f;

    // BACK button
    if (UIButtonGhost(BTN_DETAIL_BACK, {px, btn_y, btn_w, 36.0f}, "< BACK", 13.0f)) {
        view = BeatmapView::LIST;
        detail_slide.Set(0.0f);
        selected_idx = -1;
    }

    // DOWNLOAD / CANCEL button
    bool busy = osu_downloader.Busy() && is_this_set;
    bool done = is_this_set && dl.state == DownloadStatus::State::DONE;

    if (busy) {
        if (UIButton(BTN_DETAIL_CANCEL,
                     {px + btn_w + 12.0f, btn_y, btn_w, 36.0f},
                     "CANCEL", 13.0f)) {
            osu_downloader.Cancel();
        }
    } else if (done) {
        // Show a muted "Downloaded" label instead
        DrawRectRounded({px + btn_w + 12.0f, btn_y, btn_w, 36.0f}, 10.0f,
                        ColorAlpha(UI_ACCENT, 0.12f));
        DrawRectRoundedBorder({px + btn_w + 12.0f, btn_y, btn_w, 36.0f}, 10.0f, 1.5f,
                              ColorAlpha(UI_ACCENT, 0.4f));
        DrawTextCentered(ui.font_body, "DOWNLOADED",
                         px + btn_w + 12.0f + btn_w * 0.5f, btn_y + 11.0f,
                         13.0f, 0.5f, UI_ACCENT);
    } else {
        if (UIButtonAccent(BTN_DETAIL_DOWNLOAD,
                           {px + btn_w + 12.0f, btn_y, btn_w, 36.0f},
                           "DOWNLOAD", 13.0f)) {
            osu_downloader.Download(bs.id, "songs");
        }
    }
}

// ─── Draw spinner ─────────────────────────────────────────────────────────────
static void DrawSpinner(float cx, float cy, float radius, float angle) {
    int segments = 12;
    for (int i = 0; i < segments; i++) {
        float a = angle + (float)i / segments * 2.0f * PI;
        float alpha = (float)i / segments;
        float x = cx + cosf(a) * radius;
        float y = cy + sinf(a) * radius;
        DrawCircleV({x, y}, 3.0f + alpha * 2.0f, ColorAlpha(UI_ACCENT, alpha));
    }
}

// ─── Main update/draw function ────────────────────────────────────────────────
void UpdateDrawBeatmaps() {
    int sw = GetScreenWidth();
    int sh = GetScreenHeight();

    // Poll downloader every frame
    osu_downloader.Update();

    // Drain search pipe
    DrainSearchPipe();

    // Animate
    detail_slide.speed = 14.0f;
    detail_slide.Update();
    spinner_angle += GetFrameTime() * 4.0f;

    // Scroll wheel
    float wheel = GetMouseWheelMove();
    if (view == BeatmapView::LIST && wheel != 0.0f) {
        scroll_target -= wheel * 80.0f;
    }
    scroll_y += (scroll_target - scroll_y) * GetFrameTime() * 14.0f;

    // ── Background ────────────────────────────────────────────────────────────
    DrawRectangle(0, 0, sw, sh, UI_BG);
    DrawNavBar("BEATMAPS");

    float top = (float)NAVBAR_H;

    // ── No token guard ────────────────────────────────────────────────────────
    if (osu_downloader.token.empty()) {
        DrawTextCentered(ui.font_heading, "NO TOKEN SET", (float)sw * 0.5f,
                         (float)sh * 0.5f - 30.0f, 24.0f, 1.0f, UI_TEXT_MUTED);
        DrawTextCentered(ui.font_body,
                         "Set your osu! Bearer token in Settings to search for maps.",
                         (float)sw * 0.5f, (float)sh * 0.5f + 10.0f,
                         14.0f, 0.5f, UI_TEXT_MUTED);
        ui.DrawParticles();
        screen_transition.Draw();
        return;
    }

    // ── Top bar ───────────────────────────────────────────────────────────────
    float bar_h  = 52.0f;
    float bar_y  = top + 8.0f;
    float pad    = 16.0f;

    DrawRectangle(0, (int)top, sw, (int)(bar_h + 16.0f), UI_SURFACE);
    DrawLineEx({0, top + bar_h + 16.0f}, {(float)sw, top + bar_h + 16.0f}, 1.0f, UI_BORDER);

    // Search box
    float search_x = pad;
    float search_w = (float)sw - pad * 2.0f - 44.0f; // leave room for search button

    // Status filter buttons (inline, right of search)
    // Measure them first
    const char* status_labels[] = { "ALL", "RANKED", "LOVED", "QUALIFIED" };
    float filter_btn_h = 28.0f;
    float filter_btn_y = bar_y + (bar_h - filter_btn_h) * 0.5f;

    // Key filter buttons
    const char* key_labels[] = { "ANY", "4K", "7K" };
    float key_btn_w = 44.0f;
    float key_btn_h = 28.0f;
    float key_btn_y = filter_btn_y;

    // Search icon button
    float search_btn_w = 36.0f;
    float search_btn_h = 36.0f;
    float search_btn_x = (float)sw - pad - search_btn_w;
    float search_btn_y = bar_y + (bar_h - search_btn_h) * 0.5f;

    // Layout: [search box] [status filters] [key filters] [search btn]
    // Compute widths of filter groups
    float status_total = 0.0f;
    float status_widths[4];
    for (int i = 0; i < 4; i++) {
        status_widths[i] = MeasureTextEx(ui.font_body, status_labels[i], 12.0f, 0.5f).x + 20.0f;
        status_total += status_widths[i] + 4.0f;
    }
    float key_total = 3.0f * (key_btn_w + 4.0f);

    // Actual search box width
    search_w = (float)sw - pad * 2.0f
               - status_total - 8.0f
               - key_total    - 8.0f
               - search_btn_w - 8.0f;
    search_w = fmaxf(search_w, 120.0f);

    Rectangle search_box = { search_x, bar_y + (bar_h - 36.0f) * 0.5f, search_w, 36.0f };
    bool hov_search = CheckCollisionPointRec(GetMousePosition(), search_box);
    if (hov_search && IsMouseButtonPressed(MOUSE_LEFT_BUTTON))
        search_editing = true;
    if (search_editing && IsKeyPressed(KEY_ESCAPE))
        search_editing = false;

    DrawRectRounded(search_box, 8.0f,
        search_editing ? ColorAlpha(UI_ACCENT, 0.1f) : UI_SURFACE2);
    DrawRectRoundedBorder(search_box, 8.0f, 1.5f,
        search_editing ? UI_ACCENT
        : (hov_search ? ColorLerp(UI_BORDER, UI_ACCENT, 0.4f) : UI_BORDER));

    if (search_editing) {
        int ch;
        while ((ch = GetCharPressed()) != 0) {
            int len = (int)strlen(search_buf);
            if (ch >= 32 && len < (int)sizeof(search_buf) - 1) {
                search_buf[len]     = (char)ch;
                search_buf[len + 1] = '\0';
            }
        }
        if (IsKeyPressed(KEY_BACKSPACE)) {
            int len = (int)strlen(search_buf);
            if (len > 0) search_buf[len - 1] = '\0';
        }
        if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
            search_editing = false;
            StartSearch();
        }
    }

    const char* search_display = (strlen(search_buf) == 0 && !search_editing)
        ? "Search beatmaps..."
        : search_buf;
    Color search_text_col = (strlen(search_buf) == 0 && !search_editing)
        ? UI_TEXT_MUTED : UI_TEXT;
    DrawTextEx(ui.font_body, search_display,
               {search_box.x + 10.0f, search_box.y + 10.0f},
               14.0f, 0.3f, search_text_col);

    if (search_editing && (int)(GetTime() * 2.0) % 2 == 0) {
        float tw = MeasureTextEx(ui.font_body, search_buf, 14.0f, 0.3f).x;
        DrawRectangle((int)(search_box.x + 10.0f + tw), (int)(search_box.y + 8.0f),
                      2, 20, UI_ACCENT);
    }

    // Status filter buttons
    float fx = search_x + search_w + 8.0f;
    for (int i = 0; i < 4; i++) {
        Rectangle fb = { fx, filter_btn_y, status_widths[i], filter_btn_h };
        if (status_filter == i) {
            UIButtonAccent(BTN_STATUS_ALL + i, fb, status_labels[i], 12.0f);
        } else {
            if (UIButtonGhost(BTN_STATUS_ALL + i, fb, status_labels[i], 12.0f)) {
                status_filter = i;
                if (search_state == SearchState::RESULTS || search_state == SearchState::ERROR)
                    StartSearch();
            }
        }
        fx += status_widths[i] + 4.0f;
    }

    // Key filter buttons
    fx += 4.0f;
    for (int i = 0; i < 3; i++) {
        Rectangle kb = { fx, key_btn_y, key_btn_w, key_btn_h };
        if (keys_filter == i) {
            UIButtonAccent(BTN_KEYS_ANY + i, kb, key_labels[i], 12.0f);
        } else {
            if (UIButtonGhost(BTN_KEYS_ANY + i, kb, key_labels[i], 12.0f)) {
                keys_filter = i;
                if (search_state == SearchState::RESULTS || search_state == SearchState::ERROR)
                    StartSearch();
            }
        }
        fx += key_btn_w + 4.0f;
    }

    // Search button
    if (UIButtonAccent(BTN_SEARCH,
                       {search_btn_x, search_btn_y, search_btn_w, search_btn_h},
                       "GO", 12.0f)) {
        search_editing = false;
        StartSearch();
    }

    // ── Content area ──────────────────────────────────────────────────────────
    float content_top = top + bar_h + 16.0f + 8.0f;
    float content_h   = (float)sh - content_top;

    // Clamp scroll
    float card_h     = 84.0f;
    float card_gap   = 6.0f;
    float list_pad   = 16.0f;
    float total_list_h = (float)results.size() * (card_h + card_gap)
                         + (has_more ? 56.0f : 0.0f)
                         + list_pad * 2.0f;
    float max_scroll = fmaxf(0.0f, total_list_h - content_h);
    scroll_target = fmaxf(0.0f, fminf(scroll_target, max_scroll));

    // ── Detail panel slide ────────────────────────────────────────────────────
    float detail_w = fminf(420.0f, (float)sw * 0.38f);
    float list_w   = (float)sw - (detail_slide.value > 0.01f ? detail_w + 8.0f : 0.0f);

    // ── LIST VIEW ─────────────────────────────────────────────────────────────
    BeginScissorMode(0, (int)content_top, (int)list_w, (int)content_h);

    if (search_state == SearchState::IDLE) {
        DrawTextCentered(ui.font_heading, "SEARCH FOR MAPS",
                         list_w * 0.5f, (float)sh * 0.5f - 20.0f,
                         22.0f, 1.0f, UI_TEXT_MUTED);
        DrawTextCentered(ui.font_body,
                         "Type in the search box above and press GO",
                         list_w * 0.5f, (float)sh * 0.5f + 14.0f,
                         13.0f, 0.5f, ColorAlpha(UI_TEXT_MUTED, 0.6f));
    } else if (search_state == SearchState::SEARCHING && results.empty()) {
        DrawSpinner(list_w * 0.5f, (float)sh * 0.5f, 20.0f, spinner_angle);
        DrawTextCentered(ui.font_body, "Searching...",
                         list_w * 0.5f, (float)sh * 0.5f + 36.0f,
                         13.0f, 0.5f, UI_TEXT_MUTED);
    } else if (search_state == SearchState::ERROR) {
        DrawTextCentered(ui.font_heading, "SEARCH ERROR",
                         list_w * 0.5f, (float)sh * 0.5f - 20.0f,
                         20.0f, 1.0f, UI_DANGER);
        DrawTextCentered(ui.font_body, error_msg.c_str(),
                         list_w * 0.5f, (float)sh * 0.5f + 14.0f,
                         12.0f, 0.3f, UI_TEXT_MUTED);
    } else {
        // Draw cards
        float cy = content_top + list_pad - scroll_y;
        float card_w = list_w - list_pad * 2.0f;

        for (int i = 0; i < (int)results.size(); i++) {
            Rectangle card_bounds = { list_pad, cy, card_w, card_h };

            // Cull off-screen cards
            if (cy + card_h < content_top || cy > (float)sh) {
                cy += card_h + card_gap;
                continue;
            }

            bool is_selected = (selected_idx == i && view == BeatmapView::DETAIL);
            if (DrawResultCard(CARD_BASE + (i % (MAX_WIDGET_STATES - CARD_BASE % MAX_WIDGET_STATES)),
                               card_bounds, results[i], is_selected)) {
                selected_idx = i;
                view = BeatmapView::DETAIL;
                detail_slide.Set(1.0f);
            }
            cy += card_h + card_gap;
        }

        // Searching spinner overlay (when loading more)
        if (search_state == SearchState::SEARCHING && !results.empty()) {
            DrawSpinner(list_w * 0.5f, cy + 20.0f, 12.0f, spinner_angle);
        }

        // Load more button
        if (has_more && search_state != SearchState::SEARCHING) {
            Rectangle lm_btn = { list_w * 0.5f - 80.0f, cy + 8.0f, 160.0f, 36.0f };
            if (UIButtonGhost(BTN_LOAD_MORE, lm_btn, "LOAD MORE", 13.0f)) {
                StartSearch(true);
            }
        }

        // No results
        if (results.empty() && search_state == SearchState::RESULTS) {
            DrawTextCentered(ui.font_heading, "NO RESULTS",
                             list_w * 0.5f, (float)sh * 0.5f - 20.0f,
                             22.0f, 1.0f, UI_TEXT_MUTED);
            DrawTextCentered(ui.font_body, "Try a different search or filter.",
                             list_w * 0.5f, (float)sh * 0.5f + 14.0f,
                             13.0f, 0.5f, ColorAlpha(UI_TEXT_MUTED, 0.6f));
        }
    }

    EndScissorMode();

    // ── DETAIL PANEL ──────────────────────────────────────────────────────────
    if (detail_slide.value > 0.01f) {
        float panel_x = list_w + 4.0f;
        float panel_y = content_top + 4.0f;
        float panel_w = detail_w - 8.0f;
        float panel_h = content_h - 8.0f;

        // Slide in from right
        float slide_offset = (1.0f - detail_slide.value) * (detail_w + 20.0f);
        panel_x += slide_offset;

        if (selected_idx >= 0 && selected_idx < (int)results.size()) {
            DrawDetailPanel(panel_x, panel_y, panel_w, panel_h, results[selected_idx]);
        }
    }

    // ── Particles + transition ────────────────────────────────────────────────
    ui.DrawParticles();
    screen_transition.Draw();
}
