#include "screen_settings.h"
#include "ui.h"
#include "ui/navbar.h"
#include "screens.h"
#include "beatmap/downloader.h"
#include <cstdio>
#include <cstring>
#include <raymath.h>
extern Screen            current_screen;
extern ScreenTransition  screen_transition;

// ─── Settings state ───────────────────────────────────────────────────────────
struct Settings {
    float master_volume  = 1.0f;
    float music_volume   = 1.0f;
    float sfx_volume     = 0.8f;
    float scroll_speed   = 20.0f;
    float offset_ms      = 0.0f;
    bool  fullscreen     = false;
    bool  show_fps       = false;
    bool  hit_lighting   = true;
    bool  lane_cover     = false;
    int   key_count      = 4;      // 4k or 7k
};

static Settings cfg;

// Widget IDs — keep unique per screen
enum SettingsIDs {
    SLD_MASTER = 0,
    SLD_MUSIC,
    SLD_SFX,
    SLD_SCROLL,
    SLD_OFFSET,
    TGL_FULLSCREEN,
    TGL_FPS,
    TGL_LIGHTING,
    TGL_COVER,
    BTN_4K,
    BTN_7K,
    BTN_APPLY,
    BTN_RESET,
    BTN_TOKEN_SAVE,
    BTN_TOKEN_CLEAR,
};

static void DrawSection(const char* label, float x, float y) {
    DrawTextEx(ui.font_body, label, {x, y}, 11.0f, 2.5f, UI_TEXT_MUTED);
    DrawLineEx({x, y+15.0f}, {x+340.0f, y+15.0f}, 1.0f, UI_BORDER);
}

// ─── Token input state ────────────────────────────────────────────────────────
static char  token_buf[512]  = {};
static bool  token_editing   = false;
static bool  token_loaded    = false;

static void EnsureTokenBufLoaded() {
    if (!token_loaded) {
        strncpy(token_buf, osu_downloader.token.c_str(), sizeof(token_buf) - 1);
        token_buf[sizeof(token_buf) - 1] = '\0';
        token_loaded = true;
    }
}

// Draw a masked token string: show last 6 chars, mask the rest with *
static std::string MaskToken(const std::string& tok) {
    if (tok.empty()) return "(none)";
    if ((int)tok.size() <= 6) return std::string(tok.size(), '*');
    return std::string(tok.size() - 6, '*') + tok.substr(tok.size() - 6);
}

void UpdateDrawSettings() {
    int sw = GetScreenWidth();
    int sh = GetScreenHeight();

    DrawRectangle(0, 0, sw, sh, UI_BG);
    DrawNavBar("SETTINGS");

    float top  = NAVBAR_H + 32.0f;
    float col1 = sw * 0.5f - 380.0f;
    float col2 = sw * 0.5f + 40.0f;
    float col_w = 340.0f;

    // ── LEFT COLUMN: Audio + Gameplay ────────────────────────────────────────
    float y = top;

    // Audio
    DrawSection("AUDIO", col1, y); y += 28.0f;

    UISlider(SLD_MASTER, {col1, y + 18.0f, col_w, 32.0f}, 0.0f, 1.0f, &cfg.master_volume, "Master volume");
    y += 56.0f;
    UISlider(SLD_MUSIC,  {col1, y + 18.0f, col_w, 32.0f}, 0.0f, 1.0f, &cfg.music_volume,  "Music volume");
    y += 56.0f;
    UISlider(SLD_SFX,    {col1, y + 18.0f, col_w, 32.0f}, 0.0f, 1.0f, &cfg.sfx_volume,    "SFX volume");
    y += 72.0f;

    // Gameplay
    DrawSection("GAMEPLAY", col1, y); y += 28.0f;

    UISlider(SLD_SCROLL, {col1, y + 18.0f, col_w, 32.0f}, 5.0f, 40.0f, &cfg.scroll_speed, "Scroll speed");
    y += 56.0f;
    UISlider(SLD_OFFSET, {col1, y + 18.0f, col_w, 32.0f}, -200.0f, 200.0f, &cfg.offset_ms, "Audio offset (ms)");
    y += 72.0f;

    // Key count
    DrawTextEx(ui.font_body, "Key count", {col1, y}, 13.0f, 0.5f, UI_TEXT_MUTED);
    y += 20.0f;
    if (cfg.key_count == 4)
        UIButtonAccent(BTN_4K, {col1, y, 80.0f, 36.0f}, "4K", 14.0f);
    else
        if (UIButtonGhost(BTN_4K, {col1, y, 80.0f, 36.0f}, "4K", 14.0f)) cfg.key_count = 4;

    if (cfg.key_count == 7)
        UIButtonAccent(BTN_7K, {col1 + 92.0f, y, 80.0f, 36.0f}, "7K", 14.0f);
    else
        if (UIButtonGhost(BTN_7K, {col1 + 92.0f, y, 80.0f, 36.0f}, "7K", 14.0f)) cfg.key_count = 7;
    y += 56.0f;

    // ── osu! Token ────────────────────────────────────────────────────────────
    EnsureTokenBufLoaded();
    DrawSection("OSU! ACCOUNT", col1, y); y += 28.0f;

    DrawTextEx(ui.font_body, "Bearer token", {col1, y}, 13.0f, 0.5f, UI_TEXT_MUTED);
    y += 18.0f;

    // Text input box
    Rectangle token_box = {col1, y, col_w, 34.0f};
    bool hov_box = CheckCollisionPointRec(GetMousePosition(), token_box);
    if (hov_box && IsMouseButtonPressed(MOUSE_LEFT_BUTTON))
        token_editing = true;
    if (token_editing && IsKeyPressed(KEY_ESCAPE))
        token_editing = false;

    // Draw box
    DrawRectRounded(token_box, 8.0f,
        token_editing ? ColorAlpha(UI_ACCENT, 0.12f) : UI_SURFACE2);
    DrawRectRoundedBorder(token_box, 8.0f, 1.5f,
        token_editing ? UI_ACCENT : (hov_box ? ColorLerp(UI_BORDER, UI_ACCENT, 0.4f) : UI_BORDER));

    if (token_editing) {
        // Accept keyboard input
        int ch;
        while ((ch = GetCharPressed()) != 0) {
            int len = (int)strlen(token_buf);
            if (ch >= 32 && len < (int)sizeof(token_buf) - 1) {
                token_buf[len]     = (char)ch;
                token_buf[len + 1] = '\0';
            }
        }
        if (IsKeyPressed(KEY_BACKSPACE)) {
            int len = (int)strlen(token_buf);
            if (len > 0) token_buf[len - 1] = '\0';
        }

        // Show actual text while editing (so user can see what they paste)
        // Truncate display to fit box
        const char* display = token_buf;
        DrawTextEx(ui.font_mono, display,
                   {token_box.x + 8.0f, token_box.y + 9.0f},
                   12.0f, 0.3f, UI_TEXT);

        // Blinking cursor
        if ((int)(GetTime() * 2.0) % 2 == 0) {
            float tw = MeasureTextEx(ui.font_mono, token_buf, 12.0f, 0.3f).x;
            DrawRectangle((int)(token_box.x + 8.0f + tw), (int)(token_box.y + 8.0f),
                          2, 18, UI_ACCENT);
        }
    } else {
        // Show masked token
        std::string masked = MaskToken(osu_downloader.token);
        DrawTextEx(ui.font_mono, masked.c_str(),
                   {token_box.x + 8.0f, token_box.y + 9.0f},
                   12.0f, 0.3f,
                   osu_downloader.token.empty() ? UI_TEXT_MUTED : UI_TEXT);
    }
    y += 42.0f;

    // Save / Clear buttons
    if (UIButtonAccent(BTN_TOKEN_SAVE, {col1, y, 100.0f, 32.0f}, "SAVE", 13.0f)) {
        osu_downloader.token = token_buf;
        osu_downloader.SaveToken("config.ini");
        token_editing = false;
    }
    if (UIButtonGhost(BTN_TOKEN_CLEAR, {col1 + 112.0f, y, 100.0f, 32.0f}, "CLEAR", 13.0f)) {
        osu_downloader.token = "";
        memset(token_buf, 0, sizeof(token_buf));
        osu_downloader.SaveToken("config.ini");
        token_editing = false;
    }

    // Status line
    y += 40.0f;
    const auto& dl = osu_downloader.Status();
    if (dl.state != DownloadStatus::State::IDLE) {
        Color sc = (dl.state == DownloadStatus::State::ERROR)   ? UI_DANGER
                 : (dl.state == DownloadStatus::State::DONE)    ? UI_ACCENT
                 : UI_TEXT_MUTED;
        DrawTextEx(ui.font_body, dl.message.c_str(), {col1, y}, 12.0f, 0.3f, sc);
    } else if (!osu_downloader.token.empty()) {
        DrawTextEx(ui.font_body, "Token saved. Downloads enabled.",
                   {col1, y}, 12.0f, 0.3f, UI_ACCENT);
    } else {
        DrawTextEx(ui.font_body, "No token — paste from osu! OAuth settings.",
                   {col1, y}, 12.0f, 0.3f, UI_TEXT_MUTED);
    }

    // ── RIGHT COLUMN: Display + Toggles ──────────────────────────────────────
    y = top;
    DrawSection("DISPLAY", col2, y); y += 28.0f;

    float ty = y + 6.0f;
    UIToggle(TGL_FULLSCREEN, {col2, ty, 48.0f, 26.0f}, &cfg.fullscreen, "Fullscreen");
    ty += 48.0f;
    UIToggle(TGL_FPS,        {col2, ty, 48.0f, 26.0f}, &cfg.show_fps,   "Show FPS");
    ty += 72.0f;

    DrawSection("VISUALS", col2, ty); ty += 28.0f + 6.0f;
    UIToggle(TGL_LIGHTING, {col2, ty, 48.0f, 26.0f}, &cfg.hit_lighting, "Hit lighting");
    ty += 48.0f;
    UIToggle(TGL_COVER,    {col2, ty, 48.0f, 26.0f}, &cfg.lane_cover,   "Lane cover");
    ty += 72.0f;

    // Current values summary panel
    DrawSection("SUMMARY", col2, ty); ty += 28.0f;
    UIPanel({col2, ty, col_w, 110.0f}, 10.0f);

    char buf[64];
    snprintf(buf, sizeof(buf), "Scroll speed:  %.0f", cfg.scroll_speed);
    DrawTextEx(ui.font_body, buf, {col2+16.0f, ty+14.0f}, 13.0f, 0.3f, UI_TEXT_MUTED);
    snprintf(buf, sizeof(buf), "Offset:        %.0f ms", cfg.offset_ms);
    DrawTextEx(ui.font_body, buf, {col2+16.0f, ty+34.0f}, 13.0f, 0.3f, UI_TEXT_MUTED);
    snprintf(buf, sizeof(buf), "Keys:          %dK", cfg.key_count);
    DrawTextEx(ui.font_body, buf, {col2+16.0f, ty+54.0f}, 13.0f, 0.3f, UI_TEXT_MUTED);
    snprintf(buf, sizeof(buf), "Fullscreen:    %s", cfg.fullscreen ? "on" : "off");
    DrawTextEx(ui.font_body, buf, {col2+16.0f, ty+74.0f}, 13.0f, 0.3f, UI_TEXT_MUTED);

    // Apply / Reset
    float bot = (float)sh - 72.0f;
    if (UIButtonAccent(BTN_APPLY, {col2, bot, 160.0f, 44.0f}, "APPLY")) {
        SetMasterVolume(cfg.master_volume);
        if (cfg.fullscreen) ToggleFullscreen();
    }
    if (UIButtonGhost(BTN_RESET, {col2 + 172.0f, bot, 120.0f, 44.0f}, "RESET")) {
        cfg = Settings{};
    }

    // FPS overlay
    if (cfg.show_fps) {
        char fps[16];
        snprintf(fps, sizeof(fps), "%d fps", GetFPS());
        DrawTextEx(ui.font_mono, fps, {(float)sw - 80.0f, (float)sh - 28.0f}, 13.0f, 0.5f, UI_TEXT_MUTED);
    }

    ui.DrawParticles();
    screen_transition.Draw();
}
