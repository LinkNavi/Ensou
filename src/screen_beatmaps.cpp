#include "screen_beatmaps.h"
#include "beatmap/downloader.h"
#include "beatmap/library.h"
#include "screens.h"
#include "ui.h"
#include "ui/navbar.h"
#include <fcntl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <vector>

extern Screen current_screen;
extern ScreenTransition screen_transition;

// ─── Widget IDs (200-299)
// ─────────────────────────────────────────────────────
enum BeatmapIDs {
  BTN_SEARCH = 200,
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
  CARD_BASE = 220,
};

// ─── Data structures
// ──────────────────────────────────────────────────────────
struct BeatmapDiff {
  std::string version;
  float star_rating = 0.0f;
  float cs = 0.0f;
  int total_length = 0;
};

struct BeatmapSet {
  int id = 0;
  std::string title, title_unicode;
  std::string artist, artist_unicode;
  std::string creator;
  float bpm = 0.0f;
  std::string status;
  std::string cover_url;
  std::vector<BeatmapDiff> diffs;
};

// ─── Search state
// ─────────────────────────────────────────────────────────────
enum class SearchState { IDLE, SEARCHING, RESULTS, ERROR };
enum class BeatmapView { LIST, DETAIL };

// ─── Async cover loading
// ──────────────────────────────────────────────────────
static Texture2D s_cover = {};
static int s_cover_set_id = -1; // which set the texture is for
static FILE *s_cover_pipe = nullptr;
static std::string s_cover_tmp; // tmp file path
static int s_loading_cover_id = -1;

static bool CheckInstalled(int beatmapset_id) {
  struct stat st;
  std::string path = "songs/" + std::to_string(beatmapset_id);
  return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

static void StartLoadCover(int set_id, const std::string &url) {
  if (url.empty() || set_id == s_cover_set_id || set_id == s_loading_cover_id)
    return;

  // Cancel any in-progress cover load
  if (s_cover_pipe) {
    pclose(s_cover_pipe);
    s_cover_pipe = nullptr;
  }

  if (s_cover.id > 0) {
    UnloadTexture(s_cover);
    s_cover = {};
  }
  s_cover_set_id = -1;
  s_loading_cover_id = set_id;

  s_cover_tmp = "/tmp/ensou_cover_" + std::to_string(set_id) + ".jpg";

  std::string cmd = "curl -s -L --max-time 10 -o \"" + s_cover_tmp + "\" \"" +
                    url + "\" 2>&1";
  s_cover_pipe = popen(cmd.c_str(), "r");
  if (!s_cover_pipe) {
    s_loading_cover_id = -1;
  }
}

static void PollCoverLoad() {
  if (!s_cover_pipe) return;
  char buf[256];
  while (fgets(buf, sizeof(buf), s_cover_pipe)) {}
  if (!feof(s_cover_pipe)) return;

  pclose(s_cover_pipe);
  s_cover_pipe = nullptr;

  TraceLog(LOG_INFO, "PollCoverLoad: loading '%s'", s_cover_tmp.c_str());
  Image img = LoadImage(s_cover_tmp.c_str());
  TraceLog(LOG_INFO, "PollCoverLoad: after LoadImage data=%p format=%d w=%d h=%d",
           img.data, img.format, img.width, img.height);

  if (img.data == nullptr || img.format == 0) {
    TraceLog(LOG_WARNING, "PollCoverLoad: direct load failed, trying convert...");
    if (img.data) UnloadImage(img);
    img = {};
  std::string conv = s_cover_tmp + "_conv.bmp";
std::string cmd = "magick \"" + s_cover_tmp + "\" -colorspace sRGB \"" + conv + "\" 2>/dev/null";
    int ret = system(cmd.c_str());
    TraceLog(LOG_INFO, "PollCoverLoad: convert exit=%d", ret);
    FILE* log = fopen("/tmp/ensou_convert.log", "r");
    if (log) {
      char lbuf[256]; std::string err;
      while (fgets(lbuf, sizeof(lbuf), log)) err += lbuf;
      fclose(log);
      if (!err.empty())
        TraceLog(LOG_WARNING, "PollCoverLoad: convert stderr: %s", err.c_str());
    }
    img = LoadImage(conv.c_str());
    TraceLog(LOG_INFO, "PollCoverLoad: after convert LoadImage data=%p format=%d w=%d h=%d",
             img.data, img.format, img.width, img.height);
    if (img.data == nullptr || img.format == 0) {
      TraceLog(LOG_ERROR, "PollCoverLoad: convert fallback also failed");
      if (img.data) UnloadImage(img);
      s_loading_cover_id = -1;
      return;
    }
  }

  ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
  TraceLog(LOG_INFO, "PollCoverLoad: after ImageFormat format=%d", img.format);
  Texture2D tex = LoadTextureFromImage(img);
  UnloadImage(img);
  TraceLog(LOG_INFO, "PollCoverLoad: texture id=%d", tex.id);
  if (tex.id > 0) {
    SetTextureFilter(tex, TEXTURE_FILTER_BILINEAR);
    if (s_cover.id > 0) UnloadTexture(s_cover);
    s_cover        = tex;
    s_cover_set_id = s_loading_cover_id;
  }
  s_loading_cover_id = -1;
}static void UnloadCover() {
  if (s_cover_pipe) {
    pclose(s_cover_pipe);
    s_cover_pipe = nullptr;
  }
  if (s_cover.id > 0) {
    UnloadTexture(s_cover);
    s_cover = {};
  }
  s_cover_set_id = -1;
  s_loading_cover_id = -1;
}

// ─── Minimal JSON helpers
// ─────────────────────────────────────────────────────
static std::string FindJsonString(const std::string &json,
                                  const std::string &key) {
  std::string needle = "\"" + key + "\"";
  size_t pos = json.find(needle);
  if (pos == std::string::npos)
    return "";
  pos += needle.size();
  while (pos < json.size() &&
         (json[pos] == ' ' || json[pos] == '\t' || json[pos] == ':'))
    ++pos;
  if (pos >= json.size() || json[pos] != '"')
    return "";
  ++pos;
  std::string result;
  while (pos < json.size() && json[pos] != '"') {
    if (json[pos] == '\\' && pos + 1 < json.size()) {
      ++pos;
    }
    result += json[pos++];
  }
  return result;
}

static float FindJsonFloat(const std::string &json, const std::string &key) {
  std::string needle = "\"" + key + "\"";
  size_t pos = json.find(needle);
  if (pos == std::string::npos)
    return 0.0f;
  pos += needle.size();
  while (pos < json.size() &&
         (json[pos] == ' ' || json[pos] == '\t' || json[pos] == ':'))
    ++pos;
  if (pos >= json.size())
    return 0.0f;
  if (json[pos] == '"') {
    ++pos;
    size_t e = json.find('"', pos);
    return e == std::string::npos
               ? 0.0f
               : (float)atof(json.substr(pos, e - pos).c_str());
  }
  return (float)atof(json.c_str() + pos);
}

static int FindJsonInt(const std::string &json, const std::string &key) {
  return (int)FindJsonFloat(json, key);
}

static std::string FindJsonValue(const std::string &json,
                                 const std::string &key) {
  std::string needle = "\"" + key + "\"";
  size_t pos = json.find(needle);
  if (pos == std::string::npos)
    return "";
  pos += needle.size();
  while (pos < json.size() &&
         (json[pos] == ' ' || json[pos] == '\t' || json[pos] == ':'))
    ++pos;
  if (pos >= json.size())
    return "";
  char open = json[pos];
  if (open == '{' || open == '[') {
    char close = (open == '{') ? '}' : ']';
    int depth = 0;
    size_t start = pos;
    bool in_str = false;
    while (pos < json.size()) {
      char c = json[pos];
      if (in_str) {
        if (c == '\\')
          ++pos;
        else if (c == '"')
          in_str = false;
      } else {
        if (c == '"')
          in_str = true;
        else if (c == open)
          ++depth;
        else if (c == close) {
          --depth;
          if (depth == 0) {
            ++pos;
            return json.substr(start, pos - start);
          }
        }
      }
      ++pos;
    }
    return "";
  }
  if (open == '"') {
    ++pos;
    std::string r;
    while (pos < json.size() && json[pos] != '"') {
      if (json[pos] == '\\' && pos + 1 < json.size())
        ++pos;
      r += json[pos++];
    }
    return r;
  }
  size_t start = pos;
  while (pos < json.size() && json[pos] != ',' && json[pos] != '}' &&
         json[pos] != ']' && json[pos] != '\n')
    ++pos;
  return json.substr(start, pos - start);
}

static void IterJsonArray(const std::string &arr,
                          const std::function<void(const std::string &)> &cb) {
  if (arr.empty() || arr[0] != '[')
    return;
  size_t pos = 1;
  while (pos < arr.size()) {
    while (pos < arr.size() &&
           (arr[pos] == ' ' || arr[pos] == '\t' || arr[pos] == '\r' ||
            arr[pos] == '\n' || arr[pos] == ','))
      ++pos;
    if (pos >= arr.size() || arr[pos] == ']')
      break;
    if (arr[pos] == '{') {
      int depth = 0;
      size_t start = pos;
      bool in_str = false;
      while (pos < arr.size()) {
        char c = arr[pos];
        if (in_str) {
          if (c == '\\')
            ++pos;
          else if (c == '"')
            in_str = false;
        } else {
          if (c == '"')
            in_str = true;
          else if (c == '{')
            ++depth;
          else if (c == '}') {
            --depth;
            if (depth == 0) {
              ++pos;
              break;
            }
          }
        }
        ++pos;
      }
      cb(arr.substr(start, pos - start));
    } else
      ++pos;
  }
}

static std::string UrlEncode(const std::string &s) {
  std::string out;
  out.reserve(s.size() * 3);
  for (unsigned char c : s) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
      out += (char)c;
    else {
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", c);
      out += buf;
    }
  }
  return out;
}

static std::vector<BeatmapSet> ParseSearchResponse(const std::string &json,
                                                   std::string &out_cursor) {
  std::vector<BeatmapSet> results;
  out_cursor = FindJsonString(json, "cursor_string");
  std::string sets_arr = FindJsonValue(json, "beatmapsets");
  if (sets_arr.empty())
    return results;

  IterJsonArray(sets_arr, [&](const std::string &obj) {
    BeatmapSet bs;
    bs.id = FindJsonInt(obj, "id");
    bs.title = FindJsonString(obj, "title");
    bs.title_unicode = FindJsonString(obj, "title_unicode");
    bs.artist = FindJsonString(obj, "artist");
    bs.artist_unicode = FindJsonString(obj, "artist_unicode");
    bs.creator = FindJsonString(obj, "creator");
    bs.bpm = FindJsonFloat(obj, "bpm");
    bs.status = FindJsonString(obj, "status");

    std::string covers = FindJsonValue(obj, "covers");
    if (!covers.empty()) {
      bs.cover_url = FindJsonString(covers, "list");
      if (bs.cover_url.empty())
        bs.cover_url = FindJsonString(covers, "card");
    }

    std::string bmaps_arr = FindJsonValue(obj, "beatmaps");
    if (!bmaps_arr.empty()) {
      IterJsonArray(bmaps_arr, [&](const std::string &bobj) {
        if (FindJsonString(bobj, "mode") != "mania" &&
            FindJsonInt(bobj, "mode_int") != 3)
          return;
        BeatmapDiff d;
        d.version = FindJsonString(bobj, "version");
        d.star_rating = FindJsonFloat(bobj, "difficulty_rating");
        d.cs = FindJsonFloat(bobj, "cs");
        d.total_length = FindJsonInt(bobj, "total_length");
        bs.diffs.push_back(d);
      });
    }
    std::sort(bs.diffs.begin(), bs.diffs.end(),
              [](const BeatmapDiff &a, const BeatmapDiff &b) {
                return a.star_rating < b.star_rating;
              });
    if (bs.id != 0)
      results.push_back(std::move(bs));
  });
  return results;
}

// ─── Color helpers
// ────────────────────────────────────────────────────────────
static Color StatusColor(const std::string &s) {
  if (s == "ranked")
    return UI_ACCENT;
  if (s == "loved")
    return Color{255, 102, 170, 255};
  if (s == "qualified")
    return Color{100, 180, 255, 255};
  if (s == "approved")
    return Color{100, 220, 130, 255};
  return UI_TEXT_MUTED;
}

static Color StarColor(float stars) {
  if (stars < 2.5f)
    return Color{100, 200, 255, 255};
  if (stars < 4.0f)
    return UI_ACCENT;
  if (stars < 5.5f)
    return Color{255, 200, 80, 255};
  if (stars < 7.0f)
    return Color{255, 130, 80, 255};
  return Color{255, 80, 80, 255};
}

static std::string TruncateText(Font font, const std::string &text, float max_w,
                                float fs, float sp) {
  if (MeasureTextEx(font, text.c_str(), fs, sp).x <= max_w)
    return text;
  std::string t = text;
  while (!t.empty()) {
    t.pop_back();
    if (MeasureTextEx(font, (t + "...").c_str(), fs, sp).x <= max_w)
      return t + "...";
  }
  return "...";
}

// ─── Screen state
// ─────────────────────────────────────────────────────────────
static SearchState search_state = SearchState::IDLE;
static BeatmapView view = BeatmapView::LIST;
static char search_buf[256] = {};
static bool search_editing = false;
static int status_filter = 0;
static int keys_filter = 0;
static FILE *search_pipe = nullptr;
static std::string search_accum;
static std::vector<BeatmapSet> results;
static std::string cursor_string;
static bool has_more = false;
static std::string error_msg;
static float scroll_y = 0.0f;
static float scroll_target = 0.0f;
static int selected_idx = -1;
static bool s_is_installed = false;
static Anim detail_slide;
static float spinner_angle = 0.0f;

// ─── Build search URL
// ─────────────────────────────────────────────────────────
static std::string BuildSearchUrl(const std::string &query, int sf, int /*kf*/,
                                  const std::string &cursor) {
  std::string url = "https://osu.ppy.sh/api/v2/beatmapsets/search?m=3";
  if (!query.empty())
    url += "&q=" + UrlEncode(query);
  if (sf == 1)
    url += "&s=ranked";
  else if (sf == 2)
    url += "&s=loved";
  else if (sf == 3)
    url += "&s=qualified";
  if (!cursor.empty())
    url += "&cursor_string=" + UrlEncode(cursor);
  return url;
}

static void StartSearch(bool append = false) {
  if (search_pipe) {
    pclose(search_pipe);
    search_pipe = nullptr;
  }
  search_accum.clear();
  if (!append) {
    results.clear();
    cursor_string.clear();
    scroll_y = scroll_target = 0.0f;
    selected_idx = -1;
    UnloadCover();
  }

  std::string url = BuildSearchUrl(search_buf, status_filter, keys_filter,
                                   append ? cursor_string : "");
  std::string cmd = "curl -s -S "
                    "-H \"Authorization: Bearer " +
                    osu_downloader.token +
                    "\" "
                    "-H \"Content-Type: application/json\" "
                    "-H \"Accept: application/json\" "
                    "-H \"x-api-version: 20220705\" "
                    "\"" +
                    url + "\" 2>&1";

  search_pipe = popen(cmd.c_str(), "r");
  if (!search_pipe) {
    search_state = SearchState::ERROR;
    error_msg = "Failed to launch curl.";
    return;
  }
  int fd = fileno(search_pipe);
  if (fd >= 0)
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
  search_state = SearchState::SEARCHING;
  spinner_angle = 0.0f;
}

static void DrainSearchPipe() {
  if (!search_pipe)
    return;
  char buf[4096];
  while (true) {
    errno = 0;
    if (fgets(buf, sizeof(buf), search_pipe) == nullptr) {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        return;
      break;
    }
    search_accum += buf;
  }
  if (!feof(search_pipe))
    return;

  pclose(search_pipe);
  search_pipe = nullptr;
  if (search_accum.empty()) {
    search_state = SearchState::ERROR;
    error_msg = "Empty response.";
    return;
  }

  size_t brace = search_accum.find('{');
  if (brace == std::string::npos) {
    search_state = SearchState::ERROR;
    error_msg = "Bad response: " + search_accum.substr(0, 120);
    return;
  }

  std::string json = search_accum.substr(brace);

  auto HasNonNullError = [&](const std::string &j) -> bool {
    size_t pos = j.find("\"error\"");
    if (pos == std::string::npos)
      return false;
    // Skip past "error" and any whitespace/colon
    pos += 7;
    while (pos < j.size() && (j[pos] == ' ' || j[pos] == '\t' || j[pos] == ':'))
      ++pos;
    // null = fine, anything else = error
    return j.substr(pos, 4) != "null";
  };

  if (json.find("\"authentication\"") != std::string::npos ||
      HasNonNullError(json)) {
    search_state = SearchState::ERROR;
    error_msg = "API error — check token in Settings.";
    TraceLog(LOG_ERROR, "BeatmapSearch API error: %.500s", json.c_str());
    return;
  }

  std::string new_cursor;
  std::vector<BeatmapSet> new_results = ParseSearchResponse(json, new_cursor);

  if (keys_filter != 0) {
    float target_cs = (keys_filter == 1) ? 4.0f : 7.0f;
    for (auto &bs : new_results)
      bs.diffs.erase(std::remove_if(bs.diffs.begin(), bs.diffs.end(),
                                    [target_cs](const BeatmapDiff &d) {
                                      return fabsf(d.cs - target_cs) > 0.5f;
                                    }),
                     bs.diffs.end());
    new_results.erase(
        std::remove_if(new_results.begin(), new_results.end(),
                       [](const BeatmapSet &bs) { return bs.diffs.empty(); }),
        new_results.end());
  }

  for (auto &bs : new_results)
    results.push_back(std::move(bs));
  cursor_string = new_cursor;
  has_more = !new_cursor.empty();
  search_state = SearchState::RESULTS;
}

// ─── Draw result card
// ─────────────────────────────────────────────────────────
static bool DrawResultCard(int id, Rectangle bounds, const BeatmapSet &bs,
                           bool selected) {
  auto &ws = ui.states[id % MAX_WIDGET_STATES];
  bool hov = CheckCollisionPointRec(GetMousePosition(), bounds);
  bool clk = hov && IsMouseButtonPressed(MOUSE_LEFT_BUTTON);

  ws.hover.target = (hov || selected) ? 1.0f : 0.0f;
  ws.hover.speed = 12.0f;
  if (clk)
    ws.scale.Set(0.97f);
  ws.scale.target = 1.0f;

  float sc = ws.scale.pos > 0.01f ? ws.scale.pos : 1.0f;
  float dw = bounds.width * (1 - sc) * 0.5f,
        dh = bounds.height * (1 - sc) * 0.5f;
  Rectangle r = {bounds.x + dw, bounds.y + dh, bounds.width - dw * 2,
                 bounds.height - dh * 2};

  DrawRectRounded(r, 10.0f,
                  selected
                      ? UI_SURFACE2
                      : ColorLerp(UI_SURFACE, UI_SURFACE2, ws.hover.value));
  DrawRectRoundedBorder(
      r, 10.0f, 1.5f,
      selected
          ? ColorAlpha(UI_ACCENT, 0.55f)
          : ColorLerp(UI_BORDER, ColorAlpha(UI_ACCENT, 0.35f), ws.hover.value));

  float bw = 3.0f + ws.hover.value * 1.5f;
  DrawRectRounded({r.x, r.y + 8.0f, bw, r.height - 16.0f}, bw,
                  selected ? UI_ACCENT : ColorAlpha(UI_ACCENT, ws.hover.value));

  // Cover: use loaded texture if this is the selected card
  float cover_x = r.x + 12.0f, cover_y = r.y + 10.0f;
  float cover_w = 60.0f, cover_h = r.height - 20.0f;

  if (selected && s_cover.id > 0 && s_cover_set_id == bs.id) {
    Rectangle src = {0, 0, (float)s_cover.width, (float)s_cover.height};
    float ta = cover_w / cover_h,
          xa = (float)s_cover.width / (float)s_cover.height;
    if (xa > ta) {
      float nw = s_cover.height * ta;
      src.x = (s_cover.width - nw) * 0.5f;
      src.width = nw;
    } else {
      float nh = s_cover.width / ta;
      src.y = (s_cover.height - nh) * 0.5f;
      src.height = nh;
    }
    DrawTexturePro(s_cover, src, {cover_x, cover_y, cover_w, cover_h}, {0, 0},
                   0.0f, WHITE);
    DrawRectangleRoundedLinesEx({cover_x, cover_y, cover_w, cover_h},
                                6.0f / fminf(cover_w, cover_h), 8, 1.0f,
                                ColorAlpha(UI_ACCENT, 0.3f));
  } else {
    Color col =
        ColorAlpha(StatusColor(bs.status), 0.25f + ws.hover.value * 0.1f);
    DrawRectRounded({cover_x, cover_y, cover_w, cover_h}, 6.0f, col);
    DrawRectRoundedBorder({cover_x, cover_y, cover_w, cover_h}, 6.0f, 1.0f,
                          ColorAlpha(StatusColor(bs.status), 0.4f));
    if (!bs.title.empty()) {
      char letter[2] = {(char)toupper((unsigned char)bs.title[0]), '\0'};
      DrawTextCentered(ui.font_heading, letter, cover_x + cover_w * 0.5f,
                       cover_y + cover_h * 0.5f - 14.0f, 22.0f, 0.5f,
                       ColorAlpha(StatusColor(bs.status), 0.9f));
    }
  }

  float tx = cover_x + cover_w + 12.0f;
  float avail = r.x + r.width - tx - 160.0f;

  DrawTextEx(
      ui.font_heading,
      TruncateText(ui.font_heading, bs.title, avail, 16.0f, 0.3f).c_str(),
      {tx, r.y + 10.0f}, 16.0f, 0.3f,
      selected ? UI_ACCENT
               : ColorLerp(UI_TEXT, UI_ACCENT, ws.hover.value * 0.3f));
  DrawTextEx(ui.font_body,
             TruncateText(ui.font_body, bs.artist, avail, 12.0f, 0.3f).c_str(),
             {tx, r.y + 30.0f}, 12.0f, 0.3f, UI_TEXT_MUTED);
  DrawTextEx(ui.font_body, ("mapped by " + bs.creator).c_str(),
             {tx, r.y + 48.0f}, 11.0f, 0.3f, ColorAlpha(UI_TEXT_MUTED, 0.7f));

  float rx2 = r.x + r.width - 12.0f;

  // Status badge
  std::string su = bs.status;
  for (auto &c : su)
    c = (char)toupper((unsigned char)c);
  Color sc_col = StatusColor(bs.status);
  float badge_w =
      MeasureTextEx(ui.font_body, su.c_str(), 10.0f, 1.0f).x + 12.0f;
  float badge_x = rx2 - badge_w;
  DrawRectRounded({badge_x, r.y + 10.0f, badge_w, 16.0f}, 4.0f,
                  ColorAlpha(sc_col, 0.18f));
  DrawRectRoundedBorder({badge_x, r.y + 10.0f, badge_w, 16.0f}, 4.0f, 1.0f,
                        ColorAlpha(sc_col, 0.5f));
  DrawTextCentered(ui.font_body, su.c_str(), badge_x + badge_w * 0.5f,
                   r.y + 11.0f, 10.0f, 1.0f, sc_col);

  char bpm_buf[32];
  snprintf(bpm_buf, sizeof(bpm_buf), "%.0f BPM", bs.bpm);
  DrawTextRight(ui.font_body, bpm_buf, rx2, r.y + 32.0f, 12.0f, 0.3f,
                UI_TEXT_MUTED);

  if (!bs.diffs.empty()) {
    char sb[32];
    snprintf(sb, sizeof(sb), "%.2f*", bs.diffs.back().star_rating);
    DrawTextRight(ui.font_body, sb, rx2, r.y + 48.0f, 12.0f, 0.3f,
                  StarColor(bs.diffs.back().star_rating));
  }

  bool has_4k = false, has_7k = false;
  for (const auto &d : bs.diffs) {
    if (fabsf(d.cs - 4.0f) < 0.5f)
      has_4k = true;
    if (fabsf(d.cs - 7.0f) < 0.5f)
      has_7k = true;
  }
  float kbx = rx2;
  auto DrawKeyBadge = [&](const char *label) {
    float kbw = 28.0f;
    kbx -= kbw + 2.0f;
    DrawRectRounded({kbx, r.y + 64.0f, kbw, 14.0f}, 3.0f,
                    ColorAlpha(UI_ACCENT, 0.15f));
    DrawRectRoundedBorder({kbx, r.y + 64.0f, kbw, 14.0f}, 3.0f, 1.0f,
                          ColorAlpha(UI_ACCENT, 0.4f));
    DrawTextCentered(ui.font_body, label, kbx + kbw * 0.5f, r.y + 65.0f, 9.0f,
                     0.5f, UI_ACCENT);
  };
  if (has_7k)
    DrawKeyBadge("7K");
  if (has_4k)
    DrawKeyBadge("4K");

  return clk;
}

// ─── Draw detail panel
// ────────────────────────────────────────────────────────
static void DrawDetailPanel(float x, float y, float w, float h,
                            const BeatmapSet &bs, bool is_installed) {
  DrawRectRounded({x, y, w, h}, 12.0f, UI_SURFACE);
  DrawRectRoundedBorder({x, y, w, h}, 12.0f, 1.5f, UI_BORDER);

  float px = x + 20.0f;
  float py = y + 20.0f;
  float cvw = w - 40.0f;
  float cvh = 120.0f;

  // Cover art
  if (s_cover.id > 0 && s_cover_set_id == bs.id) {
    Rectangle src = {0, 0, (float)s_cover.width, (float)s_cover.height};
    float ta = cvw / cvh, xa = (float)s_cover.width / (float)s_cover.height;
    if (xa > ta) {
      float nw = s_cover.height * ta;
      src.x = (s_cover.width - nw) * 0.5f;
      src.width = nw;
    } else {
      float nh = s_cover.width / ta;
      src.y = (s_cover.height - nh) * 0.5f;
      src.height = nh;
    }
    DrawTexturePro(s_cover, src, {px, py, cvw, cvh}, {0, 0}, 0.0f,
                   ColorAlpha(WHITE, 0.85f));
    DrawRectangleGradientV((int)px, (int)(py + cvh * 0.5f), (int)cvw,
                           (int)(cvh * 0.5f), ColorAlpha(BLACK, 0),
                           ColorAlpha(BLACK, 0.5f));
    DrawRectangleRoundedLinesEx({px, py, cvw, cvh}, 8.0f / fminf(cvw, cvh), 8,
                                1.5f, ColorAlpha(UI_ACCENT, 0.25f));
  } else if (s_loading_cover_id == bs.id) {
    // Loading spinner
    DrawRectRounded({px, py, cvw, cvh}, 8.0f, ColorAlpha(UI_SURFACE2, 0.8f));
    float sa = (float)GetTime() * 4.0f;
    for (int i = 0; i < 10; i++) {
      float a = sa + (float)i / 10 * 2 * PI;
      DrawCircleV({px + cvw * 0.5f + cosf(a) * 16.0f,
                   py + cvh * 0.5f + sinf(a) * 16.0f},
                  2.5f + ((float)i / 10) * 2.0f,
                  ColorAlpha(UI_ACCENT, (float)i / 10));
    }
  } else {
    DrawRectRounded({px, py, cvw, cvh}, 8.0f, ColorAlpha(UI_ACCENT, 0.12f));
    DrawRectRoundedBorder({px, py, cvw, cvh}, 8.0f, 1.5f,
                          ColorAlpha(UI_ACCENT, 0.25f));
    if (!bs.title.empty()) {
      char letter[2] = {(char)toupper((unsigned char)bs.title[0]), '\0'};
      DrawTextCentered(ui.font_heading, letter, px + cvw * 0.5f,
                       py + cvh * 0.5f - 24.0f, 48.0f, 0.5f,
                       ColorAlpha(StatusColor(bs.status), 0.7f));
    }
  }
  py += cvh + 16.0f;

  DrawTextEx(ui.font_heading,
             TruncateText(ui.font_heading, bs.title, cvw, 18.0f, 0.3f).c_str(),
             {px, py}, 18.0f, 0.3f, UI_TEXT);
  py += 24.0f;
  DrawTextEx(ui.font_body,
             TruncateText(ui.font_body, bs.artist, cvw, 13.0f, 0.3f).c_str(),
             {px, py}, 13.0f, 0.3f, UI_TEXT_MUTED);
  py += 18.0f;
  DrawTextEx(ui.font_body, ("by " + bs.creator).c_str(), {px, py}, 12.0f, 0.3f,
             ColorAlpha(UI_TEXT_MUTED, 0.7f));
  py += 20.0f;

  // BPM + status badge
  char bpm_buf[32];
  snprintf(bpm_buf, sizeof(bpm_buf), "%.0f BPM", bs.bpm);
  DrawTextEx(ui.font_body, bpm_buf, {px, py}, 12.0f, 0.3f, UI_TEXT_MUTED);
  std::string su = bs.status;
  for (auto &c : su)
    c = (char)toupper((unsigned char)c);
  Color sc_col = StatusColor(bs.status);
  float badge_w =
      MeasureTextEx(ui.font_body, su.c_str(), 10.0f, 1.0f).x + 12.0f;
  float badge_x = x + w - 20.0f - badge_w;
  DrawRectRounded({badge_x, py - 1.0f, badge_w, 16.0f}, 4.0f,
                  ColorAlpha(sc_col, 0.18f));
  DrawRectRoundedBorder({badge_x, py - 1.0f, badge_w, 16.0f}, 4.0f, 1.0f,
                        ColorAlpha(sc_col, 0.5f));
  DrawTextCentered(ui.font_body, su.c_str(), badge_x + badge_w * 0.5f, py,
                   10.0f, 1.0f, sc_col);
  py += 22.0f;

  DrawLineEx({px, py}, {x + w - 20.0f, py}, 1.0f, UI_BORDER);
  py += 10.0f;
  DrawTextEx(ui.font_body, "DIFFICULTIES", {px, py}, 11.0f, 2.0f,
             UI_TEXT_MUTED);
  py += 18.0f;

  float dh2 = 28.0f, diff_area_h = 160.0f;
  int max_diffs = (int)(diff_area_h / dh2);
  for (int i = 0; i < (int)bs.diffs.size() && i < max_diffs; i++) {
    float iy = py + i * dh2;
    bool row_hov =
        CheckCollisionPointRec(GetMousePosition(), {px, iy, cvw, dh2 - 2.0f});
    DrawRectRounded({px, iy, cvw, dh2 - 2.0f}, 4.0f,
                    row_hov ? ColorAlpha(UI_ACCENT, 0.07f)
                            : ColorAlpha(UI_SURFACE2, 0.5f));
    Color dc = StarColor(bs.diffs[i].star_rating);
    DrawCircleV({px + 10.0f, iy + dh2 * 0.5f - 1.0f}, 4.0f, dc);
    DrawTextEx(ui.font_body,
               TruncateText(ui.font_body, bs.diffs[i].version, cvw - 100.0f,
                            12.0f, 0.3f)
                   .c_str(),
               {px + 20.0f, iy + dh2 * 0.5f - 7.0f}, 12.0f, 0.3f, UI_TEXT);
    char sb[16];
    snprintf(sb, sizeof(sb), "%.2f*", bs.diffs[i].star_rating);
    DrawTextRight(ui.font_body, sb, x + w - 60.0f, iy + dh2 * 0.5f - 7.0f,
                  11.0f, 0.3f, dc);
    char kb[8];
    snprintf(kb, sizeof(kb), "%.0fK", bs.diffs[i].cs);
    DrawTextRight(ui.font_body, kb, x + w - 20.0f, iy + dh2 * 0.5f - 7.0f,
                  11.0f, 0.3f, UI_TEXT_MUTED);
  }
  if ((int)bs.diffs.size() > max_diffs) {
    char mb[32];
    snprintf(mb, sizeof(mb), "+%d more...", (int)bs.diffs.size() - max_diffs);
    DrawTextEx(ui.font_body, mb, {px, py + max_diffs * dh2}, 11.0f, 0.3f,
               ColorAlpha(UI_TEXT_MUTED, 0.6f));
  }
  py += diff_area_h + 8.0f;

  DrawLineEx({px, py}, {x + w - 20.0f, py}, 1.0f, UI_BORDER);
  py += 12.0f;

  // Download status message
  const auto &dl = osu_downloader.Status();
  bool is_this_set = (dl.beatmapset_id == bs.id);
  if (is_this_set && dl.state != DownloadStatus::State::IDLE) {
    Color mc = (dl.state == DownloadStatus::State::ERROR)  ? UI_DANGER
               : (dl.state == DownloadStatus::State::DONE) ? UI_ACCENT
                                                           : UI_TEXT_MUTED;
    DrawTextEx(ui.font_body,
               TruncateText(ui.font_body, dl.message, cvw, 12.0f, 0.3f).c_str(),
               {px, py}, 12.0f, 0.3f, mc);
    py += 18.0f;
    if (dl.state == DownloadStatus::State::DOWNLOADING ||
        dl.state == DownloadStatus::State::EXTRACTING) {
      float t = (float)GetTime();
      float seg = fmodf(t * 0.6f, 1.0f);
      DrawRectRounded({px, py, cvw, 4.0f}, 2.0f, UI_SURFACE2);
      DrawRectRounded({px + cvw * seg, py, cvw * 0.35f, 4.0f}, 2.0f, UI_ACCENT);
      py += 12.0f;
    }
  }

  // Buttons
  float btn_y = y + h - 60.0f;
  float btn_w = (cvw - 12.0f) * 0.5f;

  if (UIButtonGhost(BTN_DETAIL_BACK, {px, btn_y, btn_w, 36.0f}, "< BACK",
                    13.0f)) {
    view = BeatmapView::LIST;
    detail_slide.Set(0.0f);
    selected_idx = -1;
    UnloadCover();
  }

  bool busy = osu_downloader.Busy() && is_this_set;
  bool done =
      is_installed || (is_this_set && dl.state == DownloadStatus::State::DONE);

  if (busy) {
    if (UIButton(BTN_DETAIL_CANCEL, {px + btn_w + 12.0f, btn_y, btn_w, 36.0f},
                 "CANCEL", 13.0f))
      osu_downloader.Cancel();
  } else if (done) {
    DrawRectRounded({px + btn_w + 12.0f, btn_y, btn_w, 36.0f}, 10.0f,
                    ColorAlpha(UI_ACCENT, 0.12f));
    DrawRectRoundedBorder({px + btn_w + 12.0f, btn_y, btn_w, 36.0f}, 10.0f,
                          1.5f, ColorAlpha(UI_ACCENT, 0.4f));
    DrawTextCentered(ui.font_body, "INSTALLED",
                     px + btn_w + 12.0f + btn_w * 0.5f, btn_y + 11.0f, 13.0f,
                     0.5f, UI_ACCENT);
  } else {
    if (UIButtonAccent(BTN_DETAIL_DOWNLOAD,
                       {px + btn_w + 12.0f, btn_y, btn_w, 36.0f}, "DOWNLOAD",
                       13.0f))
      osu_downloader.Download(bs.id, "songs");
  }
}

static void DrawSpinner(float cx, float cy, float radius, float angle) {
  for (int i = 0; i < 12; i++) {
    float a = angle + (float)i / 12 * 2.0f * PI, alpha = (float)i / 12;
    DrawCircleV({cx + cosf(a) * radius, cy + sinf(a) * radius},
                3.0f + alpha * 2.0f, ColorAlpha(UI_ACCENT, alpha));
  }
}

// ─── Main update/draw
// ─────────────────────────────────────────────────────────
void UpdateDrawBeatmaps() {
  int sw = GetScreenWidth(), sh = GetScreenHeight();

  osu_downloader.Update();
  DrainSearchPipe();
  PollCoverLoad();

  detail_slide.speed = 14.0f;
  detail_slide.Update();
  spinner_angle += GetFrameTime() * 4.0f;

  float wheel = GetMouseWheelMove();
  if (view == BeatmapView::LIST && wheel != 0.0f)
    scroll_target -= wheel * 80.0f;
  scroll_y += (scroll_target - scroll_y) * GetFrameTime() * 14.0f;

  // Recheck install status every frame while panel is open
  if (selected_idx >= 0 && selected_idx < (int)results.size())
    s_is_installed = CheckInstalled(results[selected_idx].id);

  DrawRectangle(0, 0, sw, sh, UI_BG);
  DrawNavBar("BEATMAPS");

  float top = (float)NAVBAR_H;

  if (!osu_downloader.HasCredentials()) {
    DrawTextCentered(ui.font_heading, "BEATMAP SEARCH", (float)sw * 0.5f,
                     (float)sh * 0.5f - 20.0f, 28.0f, 1.0f,
                     ColorAlpha(UI_TEXT_MUTED, 0.2f));
    DrawTextCentered(ui.font_body,
                     "Connect your osu! account in Settings to search maps.",
                     (float)sw * 0.5f, (float)sh * 0.5f + 16.0f, 14.0f, 0.5f,
                     ColorAlpha(UI_TEXT_MUTED, 0.15f));
    ui.DrawParticles();
    screen_transition.Draw();
    return;
  }

  // ── Top bar
  // ───────────────────────────────────────────────────────────────
  float bar_h = 52.0f, bar_y = top + 8.0f, pad = 16.0f;
  DrawRectangle(0, (int)top, sw, (int)(bar_h + 16.0f), UI_SURFACE);
  DrawLineEx({0, top + bar_h + 16.0f}, {(float)sw, top + bar_h + 16.0f}, 1.0f,
             UI_BORDER);

  const char *status_labels[] = {"ALL", "RANKED", "LOVED", "QUALIFIED"};
  const char *key_labels[] = {"ANY", "4K", "7K"};
  float filter_btn_h = 28.0f,
        filter_btn_y = bar_y + (bar_h - filter_btn_h) * 0.5f;
  float key_btn_w = 44.0f, key_btn_y = filter_btn_y;
  float search_btn_w = 36.0f, search_btn_h = 36.0f;
  float search_btn_x = (float)sw - pad - search_btn_w;
  float search_btn_y = bar_y + (bar_h - search_btn_h) * 0.5f;

  float status_widths[4];
  float status_total = 0.0f;
  for (int i = 0; i < 4; i++) {
    status_widths[i] =
        MeasureTextEx(ui.font_body, status_labels[i], 12.0f, 0.5f).x + 20.0f;
    status_total += status_widths[i] + 4.0f;
  }
  float key_total = 3.0f * (key_btn_w + 4.0f);
  float search_w = fmaxf((float)sw - pad * 2.0f - status_total - 8.0f -
                             key_total - 8.0f - search_btn_w - 8.0f,
                         120.0f);

  Rectangle search_box = {pad, bar_y + (bar_h - 36.0f) * 0.5f, search_w, 36.0f};
  bool hov_search = CheckCollisionPointRec(GetMousePosition(), search_box);
  if (hov_search && IsMouseButtonPressed(MOUSE_LEFT_BUTTON))
    search_editing = true;
  if (search_editing && IsKeyPressed(KEY_ESCAPE))
    search_editing = false;

  DrawRectRounded(search_box, 8.0f,
                  search_editing ? ColorAlpha(UI_ACCENT, 0.1f) : UI_SURFACE2);
  DrawRectRoundedBorder(
      search_box, 8.0f, 1.5f,
      search_editing
          ? UI_ACCENT
          : (hov_search ? ColorLerp(UI_BORDER, UI_ACCENT, 0.4f) : UI_BORDER));

  if (search_editing) {
    int ch;
    while ((ch = GetCharPressed()) != 0) {
      int len = (int)strlen(search_buf);
      if (ch >= 32 && len < (int)sizeof(search_buf) - 1) {
        search_buf[len] = (char)ch;
        search_buf[len + 1] = '\0';
      }
    }
    if (IsKeyPressed(KEY_BACKSPACE)) {
      int len = (int)strlen(search_buf);
      if (len > 0)
        search_buf[len - 1] = '\0';
    }
    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
      search_editing = false;
      StartSearch();
    }
    if ((IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)) &&
        IsKeyPressed(KEY_V)) {
      const char *clip = GetClipboardText();
      if (clip) {
        int cur = (int)strlen(search_buf),
            space = (int)sizeof(search_buf) - 1 - cur;
        if (space > 0) {
          strncat(search_buf, clip, space);
          search_buf[sizeof(search_buf) - 1] = '\0';
        }
      }
    }
  }

  float bm_text_x = search_box.x + 10.0f,
        bm_text_avail = search_box.width - 18.0f;
  if (strlen(search_buf) == 0 && !search_editing) {
    DrawTextEx(ui.font_body, "Search beatmaps...",
               {bm_text_x, search_box.y + 10.0f}, 14.0f, 0.3f, UI_TEXT_MUTED);
  } else {
    const char *disp = search_buf;
    int slen = (int)strlen(search_buf);
    for (int i = 0; i < slen; i++)
      if (MeasureTextEx(ui.font_body, search_buf + i, 14.0f, 0.3f).x <=
          bm_text_avail) {
        disp = search_buf + i;
        break;
      }
    BeginScissorMode((int)bm_text_x, (int)search_box.y, (int)bm_text_avail,
                     (int)search_box.height);
    DrawTextEx(ui.font_body, disp, {bm_text_x, search_box.y + 10.0f}, 14.0f,
               0.3f, UI_TEXT);
    EndScissorMode();
  }
  if (search_editing && (int)(GetTime() * 2.0) % 2 == 0) {
    float tw = MeasureTextEx(ui.font_body, search_buf, 14.0f, 0.3f).x;
    float cur = bm_text_x + fminf(tw, bm_text_avail);
    DrawRectangle((int)cur, (int)(search_box.y + 8.0f), 2, 20, UI_ACCENT);
  }

  float fx = pad + search_w + 8.0f;
  for (int i = 0; i < 4; i++) {
    Rectangle fb = {fx, filter_btn_y, status_widths[i], filter_btn_h};
    if (status_filter == i)
      UIButtonAccent(BTN_STATUS_ALL + i, fb, status_labels[i], 12.0f);
    else if (UIButtonGhost(BTN_STATUS_ALL + i, fb, status_labels[i], 12.0f)) {
      status_filter = i;
      if (search_state == SearchState::RESULTS ||
          search_state == SearchState::ERROR)
        StartSearch();
    }
    fx += status_widths[i] + 4.0f;
  }
  fx += 4.0f;
  for (int i = 0; i < 3; i++) {
    Rectangle kb = {fx, key_btn_y, key_btn_w, filter_btn_h};
    if (keys_filter == i)
      UIButtonAccent(BTN_KEYS_ANY + i, kb, key_labels[i], 12.0f);
    else if (UIButtonGhost(BTN_KEYS_ANY + i, kb, key_labels[i], 12.0f)) {
      keys_filter = i;
      if (search_state == SearchState::RESULTS ||
          search_state == SearchState::ERROR)
        StartSearch();
    }
    fx += key_btn_w + 4.0f;
  }
  if (UIButtonAccent(BTN_SEARCH,
                     {search_btn_x, search_btn_y, search_btn_w, search_btn_h},
                     "GO", 12.0f)) {
    search_editing = false;
    StartSearch();
  }

  // ── Content
  // ───────────────────────────────────────────────────────────────
  float content_top = top + bar_h + 16.0f + 8.0f;
  float content_h = (float)sh - content_top;
  float detail_w = fminf(420.0f, (float)sw * 0.38f);
  float list_w = (float)sw - detail_w * detail_slide.value;

  float card_h = 84.0f, card_gap = 6.0f, list_pad = 16.0f;
  float total_list_h = (float)results.size() * (card_h + card_gap) +
                       (has_more ? 56.0f : 0.0f) + list_pad * 2.0f;
  float max_scroll = fmaxf(0.0f, total_list_h - content_h);
  scroll_target = fmaxf(0.0f, fminf(scroll_target, max_scroll));

  BeginScissorMode(0, (int)content_top, (int)list_w, (int)content_h);

  if (search_state == SearchState::IDLE) {
    DrawTextCentered(ui.font_heading, "SEARCH FOR MAPS", list_w * 0.5f,
                     (float)sh * 0.5f - 20.0f, 22.0f, 1.0f, UI_TEXT_MUTED);
    DrawTextCentered(ui.font_body, "Type in the search box and press GO",
                     list_w * 0.5f, (float)sh * 0.5f + 14.0f, 13.0f, 0.5f,
                     ColorAlpha(UI_TEXT_MUTED, 0.6f));
  } else if (search_state == SearchState::SEARCHING && results.empty()) {
    DrawSpinner(list_w * 0.5f, (float)sh * 0.5f, 20.0f, spinner_angle);
    DrawTextCentered(ui.font_body, "Searching...", list_w * 0.5f,
                     (float)sh * 0.5f + 36.0f, 13.0f, 0.5f, UI_TEXT_MUTED);
  } else if (search_state == SearchState::ERROR) {
    DrawTextCentered(ui.font_heading, "SEARCH ERROR", list_w * 0.5f,
                     (float)sh * 0.5f - 20.0f, 20.0f, 1.0f, UI_DANGER);
    DrawTextCentered(ui.font_body, error_msg.c_str(), list_w * 0.5f,
                     (float)sh * 0.5f + 14.0f, 12.0f, 0.3f, UI_TEXT_MUTED);
  } else {
    float cy = content_top + list_pad - scroll_y;
    float card_w = list_w - list_pad * 2.0f - 2.0f;
    for (int i = 0; i < (int)results.size(); i++) {
      Rectangle cb = {list_pad, cy, card_w, card_h};
      if (cy + card_h >= content_top && cy <= (float)sh) {
        bool sel = (selected_idx == i && view == BeatmapView::DETAIL);
        int wid = CARD_BASE +
                  (i % (MAX_WIDGET_STATES - CARD_BASE % MAX_WIDGET_STATES));
        if (DrawResultCard(wid, cb, results[i], sel)) {
          if (selected_idx == i && view == BeatmapView::DETAIL) {
            view = BeatmapView::LIST;
            detail_slide.Set(0.0f);
            selected_idx = -1;
            UnloadCover();
          } else {
            selected_idx = i;
            view = BeatmapView::DETAIL;
            detail_slide.Set(1.0f);
            s_is_installed = CheckInstalled(results[i].id);
            // Start async cover load
            StartLoadCover(results[i].id, results[i].cover_url);
          }
        }
      }
      cy += card_h + card_gap;
    }
    if (search_state == SearchState::SEARCHING && !results.empty())
      DrawSpinner(list_w * 0.5f,
                  content_top + list_pad +
                      (float)results.size() * (card_h + card_gap) + 20.0f,
                  12.0f, spinner_angle);
    if (has_more && search_state != SearchState::SEARCHING) {
      float lmy = content_top + list_pad +
                  (float)results.size() * (card_h + card_gap) + 8.0f;
      if (UIButtonGhost(BTN_LOAD_MORE,
                        {list_w * 0.5f - 80.0f, lmy, 160.0f, 36.0f},
                        "LOAD MORE", 13.0f))
        StartSearch(true);
    }
    if (results.empty() && search_state == SearchState::RESULTS) {
      DrawTextCentered(ui.font_heading, "NO RESULTS", list_w * 0.5f,
                       (float)sh * 0.5f - 20.0f, 22.0f, 1.0f, UI_TEXT_MUTED);
      DrawTextCentered(ui.font_body, "Try a different search or filter.",
                       list_w * 0.5f, (float)sh * 0.5f + 14.0f, 13.0f, 0.5f,
                       ColorAlpha(UI_TEXT_MUTED, 0.6f));
    }
  }
  EndScissorMode();

  if (detail_slide.value > 0.01f && selected_idx >= 0 &&
      selected_idx < (int)results.size()) {
    float slide = (1.0f - detail_slide.value) * (detail_w + 20.0f);
    DrawDetailPanel(list_w + 4.0f + slide, content_top + 4.0f, detail_w - 8.0f,
                    content_h - 8.0f, results[selected_idx], s_is_installed);
  }

  ui.DrawParticles();
  screen_transition.Draw();
}
