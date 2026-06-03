#include "screen_settings.h"
#include "ui.h"
#include "ui/navbar.h"
#include "ui/oauth_popup.h"
#include "screens.h"
#include "beatmap/downloader.h"
#include <cstdio>
#include <cstring>
#include <string>
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
    int   key_count      = 4;
};

static Settings cfg;

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
    BTN_TOKEN_GUIDE,
};

static void DrawSection(const char* label, float x, float y) {
    DrawTextEx(ui.font_body, label, {x, y}, 11.0f, 2.5f, UI_TEXT_MUTED);
    DrawLineEx({x, y + 15.0f}, {x + 340.0f, y + 15.0f}, 1.0f, UI_BORDER);
}

void UpdateDrawSettings() {
    int sw = GetScreenWidth();
    int sh = GetScreenHeight();

    DrawRectangle(0, 0, sw, sh, UI_BG);
    DrawNavBar("SETTINGS");

    float top   = NAVBAR_H + 32.0f;
    float col1  = sw * 0.5f - 380.0f;
    float col2  = sw * 0.5f + 40.0f;
    float col_w = 340.0f;

    // ── LEFT COLUMN ───────────────────────────────────────────────────────────
    float y = top;

    DrawSection("AUDIO", col1, y); y += 28.0f;
    UISlider(SLD_MASTER, {col1, y + 18.0f, col_w, 32.0f}, 0.0f, 1.0f,    &cfg.master_volume, "Master volume");  y += 56.0f;
    UISlider(SLD_MUSIC,  {col1, y + 18.0f, col_w, 32.0f}, 0.0f, 1.0f,    &cfg.music_volume,  "Music volume");   y += 56.0f;
    UISlider(SLD_SFX,    {col1, y + 18.0f, col_w, 32.0f}, 0.0f, 1.0f,    &cfg.sfx_volume,    "SFX volume");     y += 72.0f;

    DrawSection("GAMEPLAY", col1, y); y += 28.0f;
    UISlider(SLD_SCROLL, {col1, y + 18.0f, col_w, 32.0f}, 5.0f,   40.0f,  &cfg.scroll_speed, "Scroll speed");     y += 56.0f;
    UISlider(SLD_OFFSET, {col1, y + 18.0f, col_w, 32.0f}, -200.0f, 200.0f, &cfg.offset_ms,   "Audio offset (ms)"); y += 72.0f;

    DrawTextEx(ui.font_body, "Key count", {col1, y}, 13.0f, 0.5f, UI_TEXT_MUTED);
    y += 20.0f;
    if (cfg.key_count == 4) UIButtonAccent(BTN_4K, {col1,        y, 80.0f, 36.0f}, "4K", 14.0f);
    else if (UIButtonGhost(BTN_4K,                 {col1,        y, 80.0f, 36.0f}, "4K", 14.0f)) cfg.key_count = 4;
    if (cfg.key_count == 7) UIButtonAccent(BTN_7K, {col1+92.0f,  y, 80.0f, 36.0f}, "7K", 14.0f);
    else if (UIButtonGhost(BTN_7K,                 {col1+92.0f,  y, 80.0f, 36.0f}, "7K", 14.0f)) cfg.key_count = 7;
    y += 56.0f;

    // ── osu! Account ──────────────────────────────────────────────────────────
    DrawSection("OSU! ACCOUNT", col1, y); y += 28.0f;

    static char s_id_buf[64]   = {};
    static char s_sec_buf[256] = {};
    static bool s_id_init      = false;
    static bool s_id_editing   = false;
    static bool s_sec_editing  = false;

    if (!s_id_init) {
        strncpy(s_id_buf,  osu_downloader.client_id.c_str(),     sizeof(s_id_buf)-1);
        strncpy(s_sec_buf, osu_downloader.client_secret.c_str(), sizeof(s_sec_buf)-1);
        s_id_init = true;
    }

    // Client ID
    DrawTextEx(ui.font_body, "Client ID", {col1, y}, 13.0f, 0.5f, UI_TEXT_MUTED);
    y += 18.0f;

    Rectangle id_box = {col1, y, col_w, 34.0f};
    bool hov_id = CheckCollisionPointRec(GetMousePosition(), id_box);
    if (hov_id && IsMouseButtonPressed(MOUSE_LEFT_BUTTON))  { s_id_editing = true;  s_sec_editing = false; }
    if (s_id_editing && IsKeyPressed(KEY_ESCAPE))             s_id_editing = false;
    if (s_id_editing && IsKeyPressed(KEY_TAB))              { s_id_editing = false;  s_sec_editing = true; }

    DrawRectRounded(id_box, 8.0f, s_id_editing ? ColorAlpha(UI_ACCENT, 0.12f) : UI_SURFACE2);
    DrawRectRoundedBorder(id_box, 8.0f, 1.5f,
        s_id_editing ? UI_ACCENT : (hov_id ? ColorLerp(UI_BORDER, UI_ACCENT, 0.4f) : UI_BORDER));

    if (s_id_editing) {
        int ch;
        while ((ch = GetCharPressed()) != 0) {
            int len = (int)strlen(s_id_buf);
            if (ch >= 32 && len < (int)sizeof(s_id_buf)-1)
                { s_id_buf[len] = (char)ch; s_id_buf[len+1] = '\0'; }
        }
        if (IsKeyPressed(KEY_BACKSPACE)) {
            int len = (int)strlen(s_id_buf);
            if (len > 0) s_id_buf[len-1] = '\0';
        }
        if ((IsKeyDown(KEY_LEFT_CONTROL)||IsKeyDown(KEY_RIGHT_CONTROL)) && IsKeyPressed(KEY_V)) {
            const char* clip = GetClipboardText();
            if (clip) {
                int cur = (int)strlen(s_id_buf), space = (int)sizeof(s_id_buf)-1-cur;
                if (space > 0) { strncat(s_id_buf, clip, space); s_id_buf[sizeof(s_id_buf)-1] = '\0'; }
            }
        }
    }

    DrawTextEx(ui.font_mono,
               strlen(s_id_buf)==0 && !s_id_editing ? "e.g. 12345" : s_id_buf,
               {id_box.x+10.0f, id_box.y+9.0f}, 12.0f, 0.3f,
               strlen(s_id_buf)==0 && !s_id_editing ? UI_TEXT_MUTED : UI_TEXT);
    if (s_id_editing && (int)(GetTime()*2.0)%2==0) {
        float tw = MeasureTextEx(ui.font_mono, s_id_buf, 12.0f, 0.3f).x;
        DrawRectangle((int)(id_box.x+10.0f+tw), (int)(id_box.y+8.0f), 2, 18, UI_ACCENT);
    }
    y += 42.0f;

    // Client Secret
    DrawTextEx(ui.font_body, "Client Secret", {col1, y}, 13.0f, 0.5f, UI_TEXT_MUTED);
    y += 18.0f;

    Rectangle sec_box = {col1, y, col_w, 34.0f};
    bool hov_sec = CheckCollisionPointRec(GetMousePosition(), sec_box);
    if (hov_sec && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) { s_sec_editing = true;  s_id_editing = false; }
    if (s_sec_editing && IsKeyPressed(KEY_ESCAPE))            s_sec_editing = false;
    if (s_sec_editing && IsKeyPressed(KEY_TAB))             { s_sec_editing = false;  s_id_editing = true; }

    DrawRectRounded(sec_box, 8.0f, s_sec_editing ? ColorAlpha(UI_ACCENT, 0.12f) : UI_SURFACE2);
    DrawRectRoundedBorder(sec_box, 8.0f, 1.5f,
        s_sec_editing ? UI_ACCENT : (hov_sec ? ColorLerp(UI_BORDER, UI_ACCENT, 0.4f) : UI_BORDER));

    if (s_sec_editing) {
        int ch;
        while ((ch = GetCharPressed()) != 0) {
            int len = (int)strlen(s_sec_buf);
            if (ch >= 32 && len < (int)sizeof(s_sec_buf)-1)
                { s_sec_buf[len] = (char)ch; s_sec_buf[len+1] = '\0'; }
        }
        if (IsKeyPressed(KEY_BACKSPACE)) {
            int len = (int)strlen(s_sec_buf);
            if (len > 0) s_sec_buf[len-1] = '\0';
        }
        if ((IsKeyDown(KEY_LEFT_CONTROL)||IsKeyDown(KEY_RIGHT_CONTROL)) && IsKeyPressed(KEY_V)) {
            const char* clip = GetClipboardText();
            if (clip) {
                int cur = (int)strlen(s_sec_buf), space = (int)sizeof(s_sec_buf)-1-cur;
                if (space > 0) { strncat(s_sec_buf, clip, space); s_sec_buf[sizeof(s_sec_buf)-1] = '\0'; }
            }
        }
    }

    {
        int sec_len = (int)strlen(s_sec_buf);
        std::string sec_display;
        if (sec_len == 0 && !s_sec_editing)
            sec_display = "Paste client secret here...";
        else if (!s_sec_editing)
            sec_display = std::string(sec_len > 8 ? sec_len-8 : sec_len, '*')
                        + (sec_len > 8 ? std::string(s_sec_buf).substr(sec_len-8) : "");
        else
            sec_display = s_sec_buf;

        DrawTextEx(ui.font_mono, sec_display.c_str(),
                   {sec_box.x+10.0f, sec_box.y+9.0f}, 12.0f, 0.3f,
                   sec_len==0 && !s_sec_editing ? UI_TEXT_MUTED : UI_TEXT);
        if (s_sec_editing && (int)(GetTime()*2.0)%2==0) {
            float tw = MeasureTextEx(ui.font_mono, s_sec_buf, 12.0f, 0.3f).x;
            DrawRectangle((int)(sec_box.x+10.0f+tw), (int)(sec_box.y+8.0f), 2, 18, UI_ACCENT);
        }
    }
    y += 42.0f;

    // Buttons
    if (UIButtonAccent(BTN_TOKEN_SAVE, {col1, y, 100.0f, 32.0f}, "SAVE", 13.0f)) {
        osu_downloader.client_id     = s_id_buf;
        osu_downloader.client_secret = s_sec_buf;
        osu_downloader.token.clear();
        osu_downloader.token_expiry  = 0.0;
        osu_downloader.SaveToken("config.ini");
        s_id_editing = s_sec_editing = false;
    }
    if (UIButtonGhost(BTN_TOKEN_CLEAR, {col1+112.0f, y, 100.0f, 32.0f}, "CLEAR", 13.0f)) {
        osu_downloader.client_id = osu_downloader.client_secret = "";
        osu_downloader.token.clear();
        osu_downloader.token_expiry = 0.0;
        memset(s_id_buf,  0, sizeof(s_id_buf));
        memset(s_sec_buf, 0, sizeof(s_sec_buf));
        s_id_init = false;
        osu_downloader.SaveToken("config.ini");
    }
    if (UIButtonGhost(BTN_TOKEN_GUIDE, {col1+224.0f, y, 116.0f, 32.0f}, "SETUP GUIDE", 12.0f))
        OAuthPopup::Show();
    y += 44.0f;

    // Status
    if (osu_downloader.HasCredentials()) {
        DrawTextEx(ui.font_body,
                   osu_downloader.TokenValid() ? "Token active. Downloads enabled." : "Refreshing token...",
                   {col1, y}, 12.0f, 0.3f,
                   osu_downloader.TokenValid() ? UI_ACCENT : UI_TEXT_MUTED);
    } else {
        DrawTextEx(ui.font_body, "No credentials — click Setup Guide to get started.",
                   {col1, y}, 12.0f, 0.3f, UI_TEXT_MUTED);
    }

    // ── RIGHT COLUMN ──────────────────────────────────────────────────────────
    y = top;
    DrawSection("DISPLAY", col2, y); y += 28.0f;

    float ty = y + 6.0f;
    UIToggle(TGL_FULLSCREEN, {col2, ty, 48.0f, 26.0f}, &cfg.fullscreen,   "Fullscreen"); ty += 48.0f;
    UIToggle(TGL_FPS,        {col2, ty, 48.0f, 26.0f}, &cfg.show_fps,     "Show FPS");   ty += 72.0f;

    DrawSection("VISUALS", col2, ty); ty += 34.0f;
    UIToggle(TGL_LIGHTING, {col2, ty, 48.0f, 26.0f}, &cfg.hit_lighting, "Hit lighting"); ty += 48.0f;
    UIToggle(TGL_COVER,    {col2, ty, 48.0f, 26.0f}, &cfg.lane_cover,   "Lane cover");   ty += 72.0f;

    DrawSection("SUMMARY", col2, ty); ty += 28.0f;
    UIPanel({col2, ty, col_w, 110.0f}, 10.0f);

    char buf[64];
    snprintf(buf, sizeof(buf), "Scroll speed:  %.0f",    cfg.scroll_speed);  DrawTextEx(ui.font_body, buf, {col2+16.0f, ty+14.0f}, 13.0f, 0.3f, UI_TEXT_MUTED);
    snprintf(buf, sizeof(buf), "Offset:        %.0f ms", cfg.offset_ms);     DrawTextEx(ui.font_body, buf, {col2+16.0f, ty+34.0f}, 13.0f, 0.3f, UI_TEXT_MUTED);
    snprintf(buf, sizeof(buf), "Keys:          %dK",     cfg.key_count);     DrawTextEx(ui.font_body, buf, {col2+16.0f, ty+54.0f}, 13.0f, 0.3f, UI_TEXT_MUTED);
    snprintf(buf, sizeof(buf), "Fullscreen:    %s",      cfg.fullscreen ? "on" : "off"); DrawTextEx(ui.font_body, buf, {col2+16.0f, ty+74.0f}, 13.0f, 0.3f, UI_TEXT_MUTED);

    float bot = (float)sh - 72.0f;
    if (UIButtonAccent(BTN_APPLY, {col2, bot, 160.0f, 44.0f}, "APPLY")) {
        SetMasterVolume(cfg.master_volume);
        if (cfg.fullscreen) ToggleFullscreen();
    }
    if (UIButtonGhost(BTN_RESET, {col2+172.0f, bot, 120.0f, 44.0f}, "RESET"))
        cfg = Settings{};

    if (cfg.show_fps) {
        char fps[16];
        snprintf(fps, sizeof(fps), "%d fps", GetFPS());
        DrawTextEx(ui.font_mono, fps, {(float)sw-80.0f, (float)sh-28.0f}, 13.0f, 0.5f, UI_TEXT_MUTED);
    }

    OAuthPopup::UpdateDraw();
    ui.DrawParticles();
    screen_transition.Draw();
}
