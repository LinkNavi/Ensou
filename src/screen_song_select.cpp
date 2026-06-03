#include "screen_song_select.h"
#include "beatmap/library.h"
#include "screens.h"
#include "ui.h"
#include "ui/navbar.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern Screen current_screen;
extern ScreenTransition screen_transition;

enum SongSelectIDs {
  SS_CARD_BASE = 300,
  SS_BTN_PLAY = 364,
  SS_BTN_DIFF_BASE = 365,
};

// ─── State
// ────────────────────────────────────────────────────────────────────
static char s_search[128] = {};
static bool s_search_editing = false;
static float s_scroll_y = 0.0f;
static float s_scroll_target = 0.0f;
static int s_selected_group = -1;
static int s_selected_diff = 0;
static Anim s_detail_anim;

static std::vector<BeatmapGroup *> s_filtered;
static char s_last_search[128] = {'\1'};
static int s_last_group_count = -1;

// ─── Album art
// ────────────────────────────────────────────────────────────────
static Texture2D s_cover = {};
static std::string s_cover_folder; // which group the texture belongs to
static void UnloadCover() {
  if (s_cover.id > 0) {
    UnloadTexture(s_cover);
    s_cover = {};
  }
  s_cover_folder.clear();
}
static void LoadCover(int group_idx) {
  if (group_idx < 0 || group_idx >= (int)s_filtered.size()) {
    UnloadCover();
    return;
  }
  const std::string &folder = s_filtered[group_idx]->folder;
  if (folder == s_cover_folder) return;

  if (s_cover.id > 0) { UnloadTexture(s_cover); s_cover = {}; }
  s_cover_folder = folder;

  const std::string &path = s_filtered[group_idx]->background_path;
  TraceLog(LOG_INFO, "LoadCover: path='%s'", path.c_str());
  if (path.empty()) { TraceLog(LOG_WARNING, "LoadCover: empty path"); return; }

  Image img = LoadImage(path.c_str());
  TraceLog(LOG_INFO, "LoadCover: after LoadImage data=%p format=%d w=%d h=%d",
           img.data, img.format, img.width, img.height);

  if (img.data == nullptr || img.format == 0) {
    TraceLog(LOG_WARNING, "LoadCover: direct load failed, trying convert...");
    if (img.data) UnloadImage(img);
    img = {};
std::string tmp = "/tmp/ensou_bg_conv.bmp";
std::string cmd = "magick \"" + path + "\" -colorspace sRGB \"" + tmp + "\" 2>/dev/null";
    int ret = system(cmd.c_str());
    TraceLog(LOG_INFO, "LoadCover: convert exit=%d", ret);
    // Log convert stderr
    FILE* log = fopen("/tmp/ensou_convert.log", "r");
    if (log) {
      char buf[256]; std::string err;
      while (fgets(buf, sizeof(buf), log)) err += buf;
      fclose(log);
      if (!err.empty())
        TraceLog(LOG_WARNING, "LoadCover: convert stderr: %s", err.c_str());
    }
    img = LoadImage(tmp.c_str());
    TraceLog(LOG_INFO, "LoadCover: after convert LoadImage data=%p format=%d w=%d h=%d",
             img.data, img.format, img.width, img.height);
    if (img.data == nullptr || img.format == 0) {
      TraceLog(LOG_ERROR, "LoadCover: convert fallback also failed");
      if (img.data) UnloadImage(img);
      return;
    }
  }

  ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
  TraceLog(LOG_INFO, "LoadCover: after ImageFormat format=%d", img.format);
  s_cover = LoadTextureFromImage(img);
  UnloadImage(img);
  TraceLog(LOG_INFO, "LoadCover: texture id=%d", s_cover.id);
  if (s_cover.id > 0)
    SetTextureFilter(s_cover, TEXTURE_FILTER_BILINEAR);
}

// ─── Helpers
// ──────────────────────────────────────────────────────────────────
static void RebuildFilter() {
  s_filtered = beatmap_library.Search(s_search);
  if (s_selected_group >= (int)s_filtered.size()) {
    s_selected_group = -1;
    UnloadCover();
  }
}

static Color DiffColor(float stars) {
  if (stars < 2.0f)
    return Color{100, 200, 255, 255};
  if (stars < 3.5f)
    return UI_ACCENT;
  if (stars < 5.0f)
    return Color{255, 200, 80, 255};
  if (stars < 6.5f)
    return Color{255, 130, 80, 255};
  return Color{255, 80, 80, 255};
}

static std::string FmtTime(int ms) {
  int s = ms / 1000;
  char buf[16];
  snprintf(buf, sizeof(buf), "%d:%02d", s / 60, s % 60);
  return buf;
}

static std::string TruncStr(Font font, const std::string &text, float max_w,
                            float size, float spacing) {
  if (MeasureTextEx(font, text.c_str(), size, spacing).x <= max_w)
    return text;
  std::string t = text;
  while (!t.empty()) {
    t.pop_back();
    if (MeasureTextEx(font, (t + "...").c_str(), size, spacing).x <= max_w)
      return t + "...";
  }
  return "...";
}

// ─── Draw song card
// ───────────────────────────────────────────────────────────
static bool DrawSongCard(int id, Rectangle r, const BeatmapGroup &g,
                         bool selected) {
  auto &ws = ui.states[id % MAX_WIDGET_STATES];
  bool hov = CheckCollisionPointRec(GetMousePosition(), r);
  bool clk = hov && IsMouseButtonPressed(MOUSE_LEFT_BUTTON);

  ws.hover.target = (hov || selected) ? 1.0f : 0.0f;
  ws.hover.speed = 12.0f;
  if (clk)
    ws.scale.Set(0.97f);
  ws.scale.target = 1.0f;

  float sc = ws.scale.pos > 0.01f ? ws.scale.pos : 1.0f;
  float dw = r.width * (1 - sc) * 0.5f, dh = r.height * (1 - sc) * 0.5f;
  Rectangle rr = {r.x + dw, r.y + dh, r.width - dw * 2, r.height - dh * 2};

  DrawRectRounded(rr, 10.0f,
                  selected
                      ? UI_SURFACE2
                      : ColorLerp(UI_SURFACE, UI_SURFACE2, ws.hover.value));
  DrawRectRoundedBorder(
      rr, 10.0f, 1.5f,
      selected
          ? ColorAlpha(UI_ACCENT, 0.6f)
          : ColorLerp(UI_BORDER, ColorAlpha(UI_ACCENT, 0.35f), ws.hover.value));

  float bar_w = 3.0f + ws.hover.value * 2.0f;
  DrawRectRounded({rr.x, rr.y + 8.0f, bar_w, rr.height - 16.0f}, bar_w,
                  selected ? UI_ACCENT : ColorAlpha(UI_ACCENT, ws.hover.value));

  // Cover thumbnail
  float cv = 52.0f;
  float cvx = rr.x + 12.0f, cvy = rr.y + (rr.height - cv) * 0.5f;

  // Check if this group has its texture loaded (only selected group has it)
  bool has_tex = (selected && s_cover.id > 0);
  if (has_tex) {
    // Draw texture scaled to fit the square
    Rectangle src = {0, 0, (float)s_cover.width, (float)s_cover.height};
    // Crop to square from center
    if (s_cover.width > s_cover.height) {
      float diff = (float)(s_cover.width - s_cover.height);
      src.x = diff * 0.5f;
      src.width = (float)s_cover.height;
    } else {
      float diff = (float)(s_cover.height - s_cover.width);
      src.y = diff * 0.5f;
      src.height = (float)s_cover.width;
    }
    DrawTexturePro(s_cover, src, {cvx, cvy, cv, cv}, {0, 0}, 0.0f, WHITE);
    DrawRectangleRoundedLinesEx({cvx, cvy, cv, cv}, 8.0f / cv, 8, 1.0f,
                                ColorAlpha(UI_ACCENT, 0.3f));
  } else {
    Color cvCol = ColorAlpha(UI_ACCENT, 0.2f + ws.hover.value * 0.1f);
    DrawRectRounded({cvx, cvy, cv, cv}, 8.0f, cvCol);
    if (!g.title.empty()) {
      char letter[2] = {(char)toupper((unsigned char)g.title[0]), '\0'};
      DrawTextCentered(ui.font_heading, letter, cvx + cv * 0.5f,
                       cvy + cv * 0.5f - 13.0f, 22.0f, 0.5f,
                       ColorAlpha(UI_ACCENT, 0.85f));
    }
  }

  float tx = cvx + cv + 10.0f;
  float avail = rr.x + rr.width - tx - 140.0f;

  std::string title = TruncStr(ui.font_heading, g.title, avail, 16.0f, 0.3f);
  std::string artist = TruncStr(ui.font_body, g.artist, avail, 12.0f, 0.3f);

  DrawTextEx(ui.font_heading, title.c_str(), {tx, rr.y + 10.0f}, 16.0f, 0.3f,
             selected ? UI_ACCENT
                      : ColorLerp(UI_TEXT, UI_ACCENT, ws.hover.value * 0.3f));
  DrawTextEx(ui.font_body, artist.c_str(), {tx, rr.y + 30.0f}, 12.0f, 0.3f,
             UI_TEXT_MUTED);

  float rx2 = rr.x + rr.width - 12.0f;
  if (g.top_stars > 0.0f) {
    char star_buf[16];
    snprintf(star_buf, sizeof(star_buf), "%.2f*", g.top_stars);
    DrawTextRight(ui.font_body, star_buf, rx2, rr.y + 12.0f, 12.0f, 0.3f,
                  DiffColor(g.top_stars));
  }

  char diff_buf[24];
  snprintf(diff_buf, sizeof(diff_buf), "%d diff%s", (int)g.diffs.size(),
           g.diffs.size() == 1 ? "" : "s");
  DrawTextRight(ui.font_body, diff_buf, rx2, rr.y + 28.0f, 11.0f, 0.3f,
                UI_TEXT_MUTED);

  if (!g.diffs.empty()) {
    char bpm_buf[16];
    snprintf(bpm_buf, sizeof(bpm_buf), "%.0f BPM", g.diffs[0].DominantBpm());
    DrawTextRight(ui.font_body, bpm_buf, rx2, rr.y + 44.0f, 11.0f, 0.3f,
                  UI_TEXT_MUTED);
  }

  return clk;
}

// ─── Draw detail panel
// ────────────────────────────────────────────────────────
static void DrawDetailPanel(float x, float y, float w, float h, BeatmapGroup &g,
                            int &sel_diff) {
  DrawRectRounded({x, y, w, h}, 12.0f, UI_SURFACE);
  DrawRectRoundedBorder({x, y, w, h}, 12.0f, 1.5f, UI_BORDER);

  float px = x + 18.0f;
  float py = y + 18.0f;
  float cvw = w - 36.0f;
  float cvh = 120.0f;

  // Cover art
  if (s_cover.id > 0) {
    // Draw full-width cover with slight dimming
    Rectangle src = {0, 0, (float)s_cover.width, (float)s_cover.height};
    // Letterbox: crop width to match aspect
    float target_aspect = cvw / cvh;
    float tex_aspect = (float)s_cover.width / (float)s_cover.height;
    if (tex_aspect > target_aspect) {
      float new_w = s_cover.height * target_aspect;
      src.x = (s_cover.width - new_w) * 0.5f;
      src.width = new_w;
    } else {
      float new_h = s_cover.width / target_aspect;
      src.y = (s_cover.height - new_h) * 0.5f;
      src.height = new_h;
    }
    DrawTexturePro(s_cover, src, {px, py, cvw, cvh}, {0, 0}, 0.0f,
                   ColorAlpha(WHITE, 0.85f));
    // Gradient overlay so text reads over it
    DrawRectangleGradientV((int)px, (int)(py + cvh * 0.5f), (int)cvw,
                           (int)(cvh * 0.5f), ColorAlpha(BLACK, 0),
                           ColorAlpha(BLACK, 0.55f));
    DrawRectangleRoundedLinesEx({px, py, cvw, cvh}, 8.0f / fminf(cvw, cvh), 8,
                                1.5f, ColorAlpha(UI_ACCENT, 0.25f));
  } else {
    DrawRectRounded({px, py, cvw, cvh}, 8.0f, ColorAlpha(UI_ACCENT, 0.12f));
    DrawRectRoundedBorder({px, py, cvw, cvh}, 8.0f, 1.5f,
                          ColorAlpha(UI_ACCENT, 0.25f));
    if (!g.title.empty()) {
      char letter[2] = {(char)toupper((unsigned char)g.title[0]), '\0'};
      DrawTextCentered(ui.font_heading, letter, px + cvw * 0.5f,
                       py + cvh * 0.5f - 22.0f, 44.0f, 0.5f,
                       ColorAlpha(UI_ACCENT, 0.4f));
    }
  }
  py += cvh + 14.0f;

  std::string title = TruncStr(ui.font_heading, g.title, cvw, 17.0f, 0.3f);
  DrawTextEx(ui.font_heading, title.c_str(), {px, py}, 17.0f, 0.3f, UI_TEXT);
  py += 22.0f;
  DrawTextEx(ui.font_body, g.artist.c_str(), {px, py}, 12.0f, 0.3f,
             UI_TEXT_MUTED);
  py += 16.0f;
  DrawTextEx(ui.font_body, ("by " + g.creator).c_str(), {px, py}, 11.0f, 0.3f,
             ColorAlpha(UI_TEXT_MUTED, 0.6f));
  py += 22.0f;

  DrawLineEx({px, py}, {x + w - 18.0f, py}, 1.0f, UI_BORDER);
  py += 10.0f;

  DrawTextEx(ui.font_body, "DIFFICULTY", {px, py}, 10.0f, 2.0f, UI_TEXT_MUTED);
  py += 16.0f;

  float dh2 = 26.0f, dgap = 4.0f;
  int max_show = (int)((h - (py - y) - 80.0f) / (dh2 + dgap));
  int shown = 0;
  for (int i = 0; i < (int)g.diffs.size() && shown < max_show; i++, shown++) {
    bool sel = (i == sel_diff);
    Rectangle dr = {px, py, w - 36.0f, dh2};
    bool hov = CheckCollisionPointRec(GetMousePosition(), dr);
    if (hov && IsMouseButtonPressed(MOUSE_LEFT_BUTTON))
      sel_diff = i;

    DrawRectRounded(dr, 5.0f,
                    sel   ? ColorAlpha(UI_ACCENT, 0.15f)
                    : hov ? ColorAlpha(UI_SURFACE2, 0.8f)
                          : Color{});
    if (sel)
      DrawRectRoundedBorder(dr, 5.0f, 1.0f, ColorAlpha(UI_ACCENT, 0.4f));

    Color dc = DiffColor(g.diffs[i].difficulty.star_rating);
    DrawCircleV({px + 10.0f, py + dh2 * 0.5f}, 4.0f, dc);

    std::string vname = TruncStr(ui.font_body, g.diffs[i].difficulty.name,
                                 w - 36.0f - 90.0f, 12.0f, 0.3f);
    DrawTextEx(ui.font_body, vname.c_str(),
               {px + 20.0f, py + dh2 * 0.5f - 7.0f}, 12.0f, 0.3f,
               sel ? UI_ACCENT : UI_TEXT);

    if (g.diffs[i].difficulty.star_rating > 0.0f) {
      char sb[16];
      snprintf(sb, sizeof(sb), "%.2f*", g.diffs[i].difficulty.star_rating);
      DrawTextRight(ui.font_body, sb, x + w - 18.0f, py + dh2 * 0.5f - 7.0f,
                    11.0f, 0.3f, dc);
    }
    py += dh2 + dgap;
  }

  if ((int)g.diffs.size() > max_show) {
    char mb[16];
    snprintf(mb, sizeof(mb), "+%d more", (int)g.diffs.size() - max_show);
    DrawTextEx(ui.font_body, mb, {px, py}, 11.0f, 0.3f,
               ColorAlpha(UI_TEXT_MUTED, 0.5f));
  }

  // Info row
  if (sel_diff < (int)g.diffs.size()) {
    float iy = y + h - 80.0f;
    DrawLineEx({px, iy}, {x + w - 18.0f, iy}, 1.0f, UI_BORDER);
    iy += 8.0f;
    std::string len_str = FmtTime(g.diffs[sel_diff].LengthMs());
    char bpm_buf[24];
    snprintf(bpm_buf, sizeof(bpm_buf), "%.0f BPM",
             g.diffs[sel_diff].DominantBpm());
    DrawTextEx(ui.font_body, len_str.c_str(), {px, iy}, 12.0f, 0.3f,
               UI_TEXT_MUTED);
    DrawTextRight(ui.font_body, bpm_buf, x + w - 18.0f, iy, 12.0f, 0.3f,
                  UI_TEXT_MUTED);
  }

  float btn_y = y + h - 48.0f;
  if (UIButtonAccent(SS_BTN_PLAY, {px, btn_y, w - 36.0f, 36.0f}, "PLAY",
                     14.0f)) {
    TraceLog(LOG_INFO, "Play: %s [%s]", g.title.c_str(),
             sel_diff < (int)g.diffs.size()
                 ? g.diffs[sel_diff].difficulty.name.c_str()
                 : "?");
  }
}

// ─── Main update/draw
// ─────────────────────────────────────────────────────────
void UpdateDrawSongSelect() {
  int sw = GetScreenWidth();
  int sh = GetScreenHeight();

  DrawRectangle(0, 0, sw, sh, UI_BG);
  DrawNavBar("SONG SELECT", true);

  float top = (float)NAVBAR_H;

  // Rebuild filter if search or library changed
  int cur_group_count = beatmap_library.TotalGroups();
  if (memcmp(s_search, s_last_search, sizeof(s_search)) != 0 ||
      cur_group_count != s_last_group_count) {
    RebuildFilter();
    memcpy(s_last_search, s_search, sizeof(s_search));
    s_last_group_count = cur_group_count;
    s_scroll_target = 0.0f;
    if (s_selected_group >= (int)s_filtered.size()) {
      s_selected_group = -1;
      UnloadCover();
    }
  }

  // Load cover for selected group
  LoadCover(s_selected_group);

  // ── Search bar ────────────────────────────────────────────────────────────
  float bar_h = 52.0f;
  float bar_y = top + 8.0f;
  float pad = 16.0f;

  DrawRectangle(0, (int)top, sw, (int)(bar_h + 16.0f), UI_SURFACE);
  DrawLineEx({0, top + bar_h + 16.0f}, {(float)sw, top + bar_h + 16.0f}, 1.0f,
             UI_BORDER);

  char count_buf[32];
  snprintf(count_buf, sizeof(count_buf), "%d songs", (int)s_filtered.size());
  float count_w = MeasureTextEx(ui.font_body, count_buf, 13.0f, 0.3f).x + 16.0f;
  DrawTextEx(ui.font_body, count_buf,
             {(float)sw - pad - count_w + 8.0f, bar_y + (bar_h - 13.0f) * 0.5f},
             13.0f, 0.3f, UI_TEXT_MUTED);

  float search_w = (float)sw - pad * 2.0f - count_w - 8.0f;
  Rectangle search_box = {pad, bar_y + (bar_h - 36.0f) * 0.5f, search_w, 36.0f};

  bool hov_s = CheckCollisionPointRec(GetMousePosition(), search_box);
  if (hov_s && IsMouseButtonPressed(MOUSE_LEFT_BUTTON))
    s_search_editing = true;
  if (s_search_editing && IsKeyPressed(KEY_ESCAPE))
    s_search_editing = false;

  DrawRectRounded(search_box, 8.0f,
                  s_search_editing ? ColorAlpha(UI_ACCENT, 0.1f) : UI_SURFACE2);
  DrawRectRoundedBorder(
      search_box, 8.0f, 1.5f,
      s_search_editing
          ? UI_ACCENT
          : (hov_s ? ColorLerp(UI_BORDER, UI_ACCENT, 0.4f) : UI_BORDER));

  if (s_search_editing) {
    int ch;
    while ((ch = GetCharPressed()) != 0) {
      int len = (int)strlen(s_search);
      if (ch >= 32 && len < (int)sizeof(s_search) - 1) {
        s_search[len] = (char)ch;
        s_search[len + 1] = '\0';
      }
    }
    if (IsKeyPressed(KEY_BACKSPACE)) {
      int len = (int)strlen(s_search);
      if (len > 0)
        s_search[len - 1] = '\0';
    }
    if ((IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)) &&
        IsKeyPressed(KEY_V)) {
      const char *clip = GetClipboardText();
      if (clip) {
        int cur = (int)strlen(s_search),
            space = (int)sizeof(s_search) - 1 - cur;
        if (space > 0) {
          strncat(s_search, clip, space);
          s_search[sizeof(s_search) - 1] = '\0';
        }
      }
    }
  }

  bool empty_search = strlen(s_search) == 0;
  float text_x = search_box.x + 14.0f;
  float text_avail = search_box.width - 22.0f;

  if (empty_search && !s_search_editing) {
    DrawTextEx(ui.font_body, "Search songs...", {text_x, search_box.y + 10.0f},
               14.0f, 0.3f, UI_TEXT_MUTED);
  } else {
    const char *disp = s_search;
    int slen = (int)strlen(s_search);
    for (int i = 0; i < slen; i++) {
      if (MeasureTextEx(ui.font_body, s_search + i, 14.0f, 0.3f).x <=
          text_avail) {
        disp = s_search + i;
        break;
      }
    }
    BeginScissorMode((int)text_x, (int)search_box.y, (int)text_avail,
                     (int)search_box.height);
    DrawTextEx(ui.font_body, disp, {text_x, search_box.y + 10.0f}, 14.0f, 0.3f,
               UI_TEXT);
    EndScissorMode();
  }
  if (s_search_editing && (int)(GetTime() * 2.0) % 2 == 0) {
    float tw = MeasureTextEx(ui.font_body, s_search, 14.0f, 0.3f).x;
    float cur = text_x + fminf(tw, text_avail);
    DrawRectangle((int)cur, (int)(search_box.y + 8.0f), 2, 20, UI_ACCENT);
  }

  // ── Layout ────────────────────────────────────────────────────────────────
  float content_top = top + bar_h + 16.0f + 8.0f;
  float content_h = (float)sh - content_top;
  float detail_w = fminf(380.0f, (float)sw * 0.32f);
  float list_w = (float)sw - detail_w * s_detail_anim.value;

  s_detail_anim.speed = 14.0f;
  s_detail_anim.target = s_selected_group >= 0 ? 1.0f : 0.0f;
  s_detail_anim.Update();

  float card_h = 80.0f;
  float card_gap = 6.0f;
  float list_pad = 16.0f;
  float total_h =
      (float)s_filtered.size() * (card_h + card_gap) + list_pad * 2.0f;
  float max_scroll = fmaxf(0.0f, total_h - content_h);

  float wheel = GetMouseWheelMove();
  if (wheel != 0.0f)
    s_scroll_target -= wheel * 80.0f;
  s_scroll_target = fmaxf(0.0f, fminf(s_scroll_target, max_scroll));
  s_scroll_y += (s_scroll_target - s_scroll_y) * GetFrameTime() * 14.0f;

  // ── Card list ─────────────────────────────────────────────────────────────
  BeginScissorMode(0, (int)content_top, (int)list_w, (int)content_h);

  if (s_filtered.empty()) {
    const char *msg = beatmap_library.TotalGroups() == 0
                          ? "No songs found in songs/ folder"
                          : "No results for your search";
    DrawTextCentered(ui.font_heading, msg, list_w * 0.5f,
                     content_top + content_h * 0.5f - 16.0f, 20.0f, 1.0f,
                     UI_TEXT_MUTED);
    if (beatmap_library.TotalGroups() == 0)
      DrawTextCentered(ui.font_body, "Download maps from the Beatmaps screen",
                       list_w * 0.5f, content_top + content_h * 0.5f + 12.0f,
                       13.0f, 0.5f, ColorAlpha(UI_TEXT_MUTED, 0.6f));
  }

  float cy = content_top + list_pad - s_scroll_y;
  float card_w = list_w - list_pad * 2.0f - 2.0f;

  for (int i = 0; i < (int)s_filtered.size(); i++) {
    Rectangle cb = {list_pad, cy, card_w, card_h};
    if (cy + card_h >= content_top && cy <= (float)sh) {
      bool sel = (s_selected_group == i);
      int wid = SS_CARD_BASE +
                (i % (MAX_WIDGET_STATES - SS_CARD_BASE % MAX_WIDGET_STATES));
      if (DrawSongCard(wid, cb, *s_filtered[i], sel)) {
        if (s_selected_group == i) {
          s_selected_group = -1;
          UnloadCover();
        } else {
          s_selected_group = i;
          s_selected_diff = 0;
          s_cover_folder.clear(); // force reload
        }
      }
    }
    cy += card_h + card_gap;
  }

  EndScissorMode();

  // ── Detail panel ──────────────────────────────────────────────────────────
  if (s_detail_anim.value > 0.01f && s_selected_group >= 0 &&
      s_selected_group < (int)s_filtered.size()) {
    float slide = (1.0f - s_detail_anim.value) * (detail_w + 20.0f);
    float px = list_w + 4.0f + slide;
    float py = content_top + 4.0f;
    DrawDetailPanel(px, py, detail_w - 8.0f, content_h - 8.0f,
                    *s_filtered[s_selected_group], s_selected_diff);
  }

  ui.DrawParticles();
  screen_transition.Draw();
}
