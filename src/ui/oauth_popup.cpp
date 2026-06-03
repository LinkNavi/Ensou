#include "ui/oauth_popup.h"
#include "ui.h"
#include "beatmap/downloader.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <rlgl.h>

// ─── Steps ────────────────────────────────────────────────────────────────────
// 0  Intro
// 1  Go to osu! account settings
// 2  Create OAuth application
// 3  Enter Client ID
// 4  Enter Client Secret
// 5  Fetching token (auto-advances)
// 6  Done / Error

static constexpr int TOTAL_STEPS = 7;

// ─── State ────────────────────────────────────────────────────────────────────
static bool  s_open         = false;
static int   s_step         = 0;
static Anim  s_backdrop;        // 0→1 fade
static Anim  s_card;            // 0→1 slide+fade for card
static Anim  s_step_anim;       // 0→1 for current step content
static float s_slide_dir    = 1.0f; // +1 forward, -1 back

// Input buffers
static char  s_client_id[64]     = {};
static char  s_client_secret[256] = {};
static bool  s_editing_id        = false;
static bool  s_editing_secret    = false;

// Token fetch state
static FILE*       s_pipe         = nullptr;
static std::string s_pipe_accum;
static bool        s_fetching     = false;
static std::string s_error_msg;
static float       s_spinner      = 0.0f;

// Step dot bounce anims
static Anim s_dots[TOTAL_STEPS];

// ─── Helpers ──────────────────────────────────────────────────────────────────
static void GoTo(int next) {
    s_slide_dir = (next > s_step) ? 1.0f : -1.0f;
    s_step = next;
    s_step_anim.Snap(0.0f);
    s_step_anim.target = 1.0f;
}

static void StartFetch() {
    if (s_pipe) { pclose(s_pipe); s_pipe = nullptr; }
    s_pipe_accum.clear();
    s_error_msg.clear();
    s_fetching = true;

    // client_credentials grant — no browser needed
    std::string cmd =
        "curl -s -X POST \"https://osu.ppy.sh/oauth/token\" "
        "-H \"Content-Type: application/x-www-form-urlencoded\" "
        "--data-urlencode \"client_id=" + std::string(s_client_id) + "\" "
        "--data-urlencode \"client_secret=" + std::string(s_client_secret) + "\" "
        "--data-urlencode \"grant_type=client_credentials\" "
        "--data-urlencode \"scope=public\" "
        "2>&1";

    s_pipe = popen(cmd.c_str(), "r");
}

static void PollFetch() {
    if (!s_pipe) return;
    char buf[1024];
    while (fgets(buf, sizeof(buf), s_pipe))
        s_pipe_accum += buf;

    if (feof(s_pipe)) {
        pclose(s_pipe);
        s_pipe = nullptr;
        s_fetching = false;

        // Parse access_token from JSON
        const std::string& json = s_pipe_accum;
        size_t pos = json.find("\"access_token\"");
        if (pos == std::string::npos) {
            // Try to get error description
            size_t ep = json.find("\"message\"");
            if (ep == std::string::npos) ep = json.find("\"error_description\"");
            s_error_msg = "No token in response.";
            if (ep != std::string::npos) {
                ep = json.find('"', ep + 9);
                if (ep != std::string::npos) {
                    ++ep;
                    size_t ee = json.find('"', ep);
                    if (ee != std::string::npos)
                        s_error_msg = json.substr(ep, ee - ep);
                }
            }
            GoTo(6); // done step shows error
            return;
        }

        // Extract the token string
        pos = json.find('"', pos + 14); // skip key
        if (pos == std::string::npos) { s_error_msg = "Parse error."; GoTo(6); return; }
        size_t colon = json.find(':', pos);
        if (colon == std::string::npos) { s_error_msg = "Parse error."; GoTo(6); return; }
        size_t q1 = json.find('"', colon);
        if (q1 == std::string::npos) { s_error_msg = "Parse error."; GoTo(6); return; }
        ++q1;
        size_t q2 = json.find('"', q1);
        if (q2 == std::string::npos) { s_error_msg = "Parse error."; GoTo(6); return; }

        std::string token = json.substr(q1, q2 - q1);

        // Save to downloader and config
        osu_downloader.token         = token;
        osu_downloader.client_id     = std::string(s_client_id);
        osu_downloader.client_secret = std::string(s_client_secret);
        osu_downloader.SaveToken("config.ini");

        GoTo(6);
    }
}

// Draw a text input box, returns true if value changed
static bool DrawInput(Rectangle box, char* buf, int buf_size,
                      bool& editing, const char* placeholder,
                      bool masked = false) {
    bool hov = CheckCollisionPointRec(GetMousePosition(), box);
    if (hov && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) editing = true;
    if (editing && IsKeyPressed(KEY_ESCAPE))            editing = false;
    if (editing && IsKeyPressed(KEY_TAB))               editing = false;

    DrawRectRounded(box, 8.0f, editing ? ColorAlpha(UI_ACCENT, 0.12f) : UI_SURFACE2);
    DrawRectRoundedBorder(box, 8.0f, 1.5f,
        editing ? UI_ACCENT : (hov ? ColorLerp(UI_BORDER, UI_ACCENT, 0.4f) : UI_BORDER));

    bool changed = false;
    if (editing) {
        int ch;
        while ((ch = GetCharPressed()) != 0) {
            int len = (int)strlen(buf);
            if (ch >= 32 && len < buf_size - 1) {
                buf[len] = (char)ch; buf[len+1] = '\0';
                changed = true;
            }
        }
        if (IsKeyPressed(KEY_BACKSPACE)) {
            int len = (int)strlen(buf);
            if (len > 0) { buf[len-1] = '\0'; changed = true; }
        }
        // Paste
        if ((IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)) && IsKeyPressed(KEY_V)) {
            const char* clip = GetClipboardText();
            if (clip) {
                int cur = (int)strlen(buf);
                int space = buf_size - 1 - cur;
                if (space > 0) { strncat(buf, clip, space); buf[buf_size-1] = '\0'; changed = true; }
            }
        }
    }

    // Display
    bool empty = (strlen(buf) == 0);
    std::string display;
    if (empty && !editing) {
        display = placeholder;
    } else if (masked && !editing) {
        int len = (int)strlen(buf);
        display = std::string(len > 6 ? len - 6 : len, '*');
        if (len > 6) display += std::string(buf).substr(len - 6);
    } else {
        display = buf;
    }

    DrawTextEx(ui.font_mono, display.c_str(),
               {box.x + 10.0f, box.y + box.height * 0.5f - 7.0f},
               13.0f, 0.3f,
               empty && !editing ? UI_TEXT_MUTED : UI_TEXT);

    if (editing && (int)(GetTime() * 2.0) % 2 == 0) {
        float tw = MeasureTextEx(ui.font_mono, buf, 13.0f, 0.3f).x;
        DrawRectangle((int)(box.x + 10.0f + tw), (int)(box.y + 8.0f), 2, (int)(box.height - 16.0f), UI_ACCENT);
    }

    return changed;
}

// Draw step progress dots at the top of the card
static void DrawStepDots(float cx, float y, int current, int total) {
    float dot_r   = 5.0f;
    float dot_gap = 18.0f;
    float total_w = (total - 1) * dot_gap;
    float sx = cx - total_w * 0.5f;

    for (int i = 0; i < total; i++) {
        s_dots[i].target = (i == current) ? 1.0f : 0.0f;
        s_dots[i].speed  = 10.0f;
        s_dots[i].Update();

        float r  = dot_r + s_dots[i].value * 3.0f;
        float fx = sx + i * dot_gap;
        Color col = i < current  ? ColorAlpha(UI_ACCENT, 0.5f)
                  : i == current ? UI_ACCENT
                                 : ColorAlpha(UI_BORDER, 0.8f);

        // Connector line
        if (i > 0) {
            float lx = sx + (i-1) * dot_gap + dot_r;
            float t  = (i < current) ? 1.0f : (i == current ? s_dots[i].value : 0.0f);
            if (t > 0.001f)
                DrawLineEx({lx, y}, {lx + (dot_gap - dot_r*2) * t, y}, 1.5f, ColorAlpha(UI_ACCENT, 0.4f));
            else
                DrawLineEx({lx, y}, {fx - dot_r, y}, 1.0f, ColorAlpha(UI_BORDER, 0.4f));
        }
        DrawCircleV({fx, y}, r, col);
        if (i == current)
            DrawCircleV({fx, y}, r - 3.0f, UI_BG);
    }
}

// ─── Step content renderers ───────────────────────────────────────────────────

static void DrawStepIntro(float cx, float cy, float alpha) {
    Color ta = ColorAlpha(UI_TEXT, alpha);
    Color ma = ColorAlpha(UI_TEXT_MUTED, alpha);
    Color aa = ColorAlpha(UI_ACCENT, alpha);

    DrawTextCentered(ui.font_heading, "Set up osu! downloads", cx, cy - 60.0f, 22.0f, 0.5f, ta);
    DrawTextCentered(ui.font_body,
        "Ensou needs an osu! OAuth app to search", cx, cy - 24.0f, 13.0f, 0.3f, ma);
    DrawTextCentered(ui.font_body,
        "and download beatmaps. It only takes a minute.", cx, cy - 6.0f, 13.0f, 0.3f, ma);

    // Feature list
    const char* feats[] = { "Search ranked & loved maps", "One-click download", "Auto token refresh" };
    for (int i = 0; i < 3; i++) {
        float fy = cy + 28.0f + i * 22.0f;
        DrawTextCentered(ui.font_body, "✓", cx - 90.0f, fy, 13.0f, 0.3f, aa);
        DrawTextEx(ui.font_body, feats[i], {cx - 74.0f, fy}, 13.0f, 0.3f, ta);
    }
}

static void DrawStepGoToSettings(float cx, float cy, float alpha) {
    Color ta = ColorAlpha(UI_TEXT, alpha);
    Color ma = ColorAlpha(UI_TEXT_MUTED, alpha);
    Color aa = ColorAlpha(UI_ACCENT, alpha);

    DrawTextCentered(ui.font_heading, "Open osu! account settings", cx, cy - 70.0f, 20.0f, 0.5f, ta);

    // URL box
    float bw = 380.0f;
    Rectangle url_box = {cx - bw*0.5f, cy - 36.0f, bw, 34.0f};
    DrawRectRounded(url_box, 8.0f, ColorAlpha(UI_SURFACE2, alpha));
    DrawRectRoundedBorder(url_box, 8.0f, 1.5f, ColorAlpha(UI_ACCENT, alpha * 0.5f));
    DrawTextCentered(ui.font_mono, "osu.ppy.sh/home/account/edit",
                     cx, cy - 36.0f + 9.0f, 12.0f, 0.3f, aa);

    DrawTextCentered(ui.font_body, "Scroll down to the  OAuth  section.", cx, cy + 14.0f, 13.0f, 0.3f, ma);
    DrawTextCentered(ui.font_body, "You'll see  \"own clients\"  with a New button.", cx, cy + 34.0f, 13.0f, 0.3f, ma);

    // Open in browser hint
    DrawTextCentered(ui.font_body, "Press  Ctrl+C  to copy the URL",
                     cx, cy + 62.0f, 12.0f, 0.3f, ColorAlpha(UI_TEXT_MUTED, alpha * 0.6f));
}

static void DrawStepCreateApp(float cx, float cy, float alpha) {
    Color ta = ColorAlpha(UI_TEXT, alpha);
    Color ma = ColorAlpha(UI_TEXT_MUTED, alpha);
    Color aa = ColorAlpha(UI_ACCENT, alpha);

    DrawTextCentered(ui.font_heading, "Create a new OAuth app", cx, cy - 80.0f, 20.0f, 0.5f, ta);

    struct Row { const char* field; const char* value; };
    Row rows[] = {
        { "Application Name:", "Ensou (or anything)" },
        { "Callback URL:",     "http://localhost"     },
    };

    float ry = cy - 44.0f;
    float lw = 160.0f;
    for (auto& row : rows) {
        DrawTextEx(ui.font_body, row.field, {cx - 190.0f, ry}, 12.0f, 0.3f, ma);

        float vw = 210.0f;
        Rectangle vbox = {cx - 190.0f + lw, ry - 4.0f, vw, 24.0f};
        DrawRectRounded(vbox, 6.0f, ColorAlpha(UI_SURFACE2, alpha));
        DrawRectRoundedBorder(vbox, 6.0f, 1.0f, ColorAlpha(UI_ACCENT, alpha * 0.3f));
        DrawTextEx(ui.font_mono, row.value, {vbox.x + 8.0f, vbox.y + 5.0f}, 12.0f, 0.3f, aa);
        ry += 36.0f;
    }

    DrawTextCentered(ui.font_body, "Click  Register application.", cx, cy + 10.0f, 13.0f, 0.3f, ma);
    DrawTextCentered(ui.font_body, "You'll see your  Client ID  and  Client Secret.", cx, cy + 30.0f, 13.0f, 0.3f, ma);
    DrawTextCentered(ui.font_body, "Keep that page open for the next steps.",
                     cx, cy + 52.0f, 12.0f, 0.3f, ColorAlpha(UI_TEXT_MUTED, alpha * 0.6f));
}

static void DrawStepClientID(float cx, float cy, float alpha, Rectangle card) {
    Color ta = ColorAlpha(UI_TEXT, alpha);
    Color ma = ColorAlpha(UI_TEXT_MUTED, alpha);

    DrawTextCentered(ui.font_heading, "Enter your Client ID", cx, cy - 60.0f, 20.0f, 0.5f, ta);
    DrawTextCentered(ui.font_body, "Copy the Client ID from the osu! OAuth page.", cx, cy - 28.0f, 13.0f, 0.3f, ma);
    DrawTextCentered(ui.font_body, "It's a short number, e.g.  12345", cx, cy - 10.0f, 13.0f, 0.3f, ma);

    float bw = 320.0f;
    Rectangle box = {cx - bw*0.5f, cy + 16.0f, bw, 38.0f};
    // Only draw input when alpha is high enough to interact
    if (alpha > 0.8f)
        DrawInput(box, s_client_id, sizeof(s_client_id), s_editing_id, "Paste Client ID here...");
    else {
        DrawRectRounded(box, 8.0f, ColorAlpha(UI_SURFACE2, alpha));
        DrawRectRoundedBorder(box, 8.0f, 1.5f, ColorAlpha(UI_BORDER, alpha));
    }

    DrawTextCentered(ui.font_body, "Ctrl+V to paste", cx, cy + 62.0f, 12.0f, 0.3f,
                     ColorAlpha(UI_TEXT_MUTED, alpha * 0.5f));
}

static void DrawStepClientSecret(float cx, float cy, float alpha) {
    Color ta = ColorAlpha(UI_TEXT, alpha);
    Color ma = ColorAlpha(UI_TEXT_MUTED, alpha);

    DrawTextCentered(ui.font_heading, "Enter your Client Secret", cx, cy - 60.0f, 20.0f, 0.5f, ta);
    DrawTextCentered(ui.font_body, "Copy the Client Secret from the osu! OAuth page.", cx, cy - 28.0f, 13.0f, 0.3f, ma);
    DrawTextCentered(ui.font_body, "It's the long string below your Client ID.", cx, cy - 10.0f, 13.0f, 0.3f, ma);

    float bw = 320.0f;
    Rectangle box = {cx - bw*0.5f, cy + 16.0f, bw, 38.0f};
    if (alpha > 0.8f)
        DrawInput(box, s_client_secret, sizeof(s_client_secret), s_editing_secret,
                  "Paste Client Secret here...", true);
    else {
        DrawRectRounded(box, 8.0f, ColorAlpha(UI_SURFACE2, alpha));
        DrawRectRoundedBorder(box, 8.0f, 1.5f, ColorAlpha(UI_BORDER, alpha));
    }

    DrawTextCentered(ui.font_body, "Ctrl+V to paste", cx, cy + 62.0f, 12.0f, 0.3f,
                     ColorAlpha(UI_TEXT_MUTED, alpha * 0.5f));
}

static void DrawStepFetching(float cx, float cy, float alpha) {
    Color ma = ColorAlpha(UI_TEXT_MUTED, alpha);

    // Spinner
    int   segs = 12;
    for (int i = 0; i < segs; i++) {
        float a   = s_spinner + (float)i / segs * 2.0f * PI;
        float seg_alpha = (float)i / segs * alpha;
        float x   = cx + cosf(a) * 24.0f;
        float y   = cy - 30.0f + sinf(a) * 24.0f;
        DrawCircleV({x, y}, 3.5f + seg_alpha * 2.0f, ColorAlpha(UI_ACCENT, seg_alpha));
    }

    DrawTextCentered(ui.font_heading, "Connecting to osu!...", cx, cy + 12.0f, 18.0f, 0.5f,
                     ColorAlpha(UI_TEXT, alpha));
    DrawTextCentered(ui.font_body, "Exchanging credentials for an access token.",
                     cx, cy + 38.0f, 13.0f, 0.3f, ma);
}

static void DrawStepDone(float cx, float cy, float alpha) {
    bool ok = s_error_msg.empty();

    if (ok) {
        // Checkmark circle
        DrawCircleV({cx, cy - 38.0f}, 26.0f, ColorAlpha(UI_ACCENT, alpha * 0.2f));
        DrawCircleV({cx, cy - 38.0f}, 20.0f, ColorAlpha(UI_ACCENT, alpha));
        DrawTextCentered(ui.font_heading, "✓", cx, cy - 52.0f, 22.0f, 0.5f,
                         ColorAlpha(UI_BG, alpha));

        DrawTextCentered(ui.font_heading, "All set!", cx, cy + 4.0f, 22.0f, 0.5f,
                         ColorAlpha(UI_ACCENT, alpha));
        DrawTextCentered(ui.font_body, "Your token has been saved. Downloads are enabled.",
                         cx, cy + 32.0f, 13.0f, 0.3f, ColorAlpha(UI_TEXT_MUTED, alpha));
    } else {
        DrawCircleV({cx, cy - 38.0f}, 26.0f, ColorAlpha(UI_DANGER, alpha * 0.2f));
        DrawCircleV({cx, cy - 38.0f}, 20.0f, ColorAlpha(UI_DANGER, alpha));
        DrawTextCentered(ui.font_heading, "✗", cx, cy - 52.0f, 22.0f, 0.5f,
                         ColorAlpha(UI_BG, alpha));

        DrawTextCentered(ui.font_heading, "Something went wrong", cx, cy + 4.0f, 20.0f, 0.5f,
                         ColorAlpha(UI_DANGER, alpha));

        // Error message — truncate to fit
        std::string err = s_error_msg.size() > 60
            ? s_error_msg.substr(0, 57) + "..."
            : s_error_msg;
        DrawTextCentered(ui.font_body, err.c_str(), cx, cy + 32.0f, 12.0f, 0.3f,
                         ColorAlpha(UI_TEXT_MUTED, alpha));
        DrawTextCentered(ui.font_body, "Check your Client ID and Secret and try again.",
                         cx, cy + 50.0f, 12.0f, 0.3f, ColorAlpha(UI_TEXT_MUTED, alpha * 0.7f));
    }
}

// ─── Public API ───────────────────────────────────────────────────────────────

void OAuthPopup::Show() {
    if (s_open) return;
    s_open = true;
    s_step = 0;
    memset(s_client_id,     0, sizeof(s_client_id));
    memset(s_client_secret, 0, sizeof(s_client_secret));
    s_editing_id     = false;
    s_editing_secret = false;
    s_error_msg.clear();
    s_fetching = false;
    if (s_pipe) { pclose(s_pipe); s_pipe = nullptr; }

    s_backdrop.Snap(0.0f); s_backdrop.target = 1.0f; s_backdrop.speed = 7.0f;
    s_card.Snap(0.0f);     s_card.target     = 1.0f; s_card.speed     = 9.0f;
    s_step_anim.Snap(1.0f);
    for (auto& d : s_dots) d.Snap(0.0f);
}

void OAuthPopup::Hide() {
    s_open = false;
    s_backdrop.target = 0.0f;
    s_card.target     = 0.0f;
    s_editing_id = s_editing_secret = false;
    if (s_pipe) { pclose(s_pipe); s_pipe = nullptr; }
}

bool OAuthPopup::IsOpen() { return s_open; }

void OAuthPopup::UpdateDraw() {
    // Animate even while closing
    s_backdrop.Update();
    s_card.Update();
    s_step_anim.Update();
    s_spinner += GetFrameTime() * 4.0f;

    if (s_fetching) PollFetch();

    float sw = (float)GetScreenWidth();
    float sh = (float)GetScreenHeight();

    if (s_backdrop.value < 0.005f && !s_open) return;

    // ── Backdrop ──────────────────────────────────────────────────────────────
    DrawRectangle(0, 0, (int)sw, (int)sh, ColorAlpha(BLACK, s_backdrop.value * 0.6f));

    // ── Card ──────────────────────────────────────────────────────────────────
    float cw = fminf(500.0f, sw - 48.0f);
    float ch = 420.0f;
    float cx = sw * 0.5f;
    float cy = sh * 0.5f;

    float slide = (1.0f - EaseOutCubic(s_card.value)) * 40.0f;
    Rectangle card = {cx - cw*0.5f, cy - ch*0.5f + slide, cw, ch};

    // Shadow
    DrawRectRounded({card.x + 4, card.y + 8, card.width, card.height},
                    14.0f, ColorAlpha(BLACK, s_card.value * 0.4f));
    // Card bg
    DrawRectRounded(card, 14.0f, UI_SURFACE);
    DrawRectRoundedBorder(card, 14.0f, 1.5f, ColorAlpha(UI_ACCENT, s_card.value * 0.3f));

    // Top accent bar
    DrawRectRounded({card.x, card.y, card.width, 4.0f}, 14.0f,
                    ColorAlpha(UI_ACCENT, s_card.value));

    // Close button
    float close_x = card.x + card.width - 36.0f;
    float close_y = card.y + 12.0f;
    bool close_hov = CheckCollisionPointRec(GetMousePosition(), {close_x, close_y, 24.0f, 24.0f});
    DrawTextEx(ui.font_body, "✕", {close_x, close_y},
               16.0f, 0.3f, close_hov ? UI_ACCENT : ColorAlpha(UI_TEXT_MUTED, s_card.value));
    if (close_hov && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
        OAuthPopup::Hide();
        return;
    }

    // Step dots
    float dots_y = card.y + 22.0f;
    DrawStepDots(cx, dots_y, s_step, TOTAL_STEPS);

    // ── Step content (slide transition) ───────────────────────────────────────
    float t       = EaseOutCubic(s_step_anim.value);
    float x_off   = (1.0f - t) * 60.0f * s_slide_dir;
    float alpha   = t * s_card.value;
    float content_cy = card.y + ch * 0.5f - 10.0f;

    // Clip to card, translate the whole draw by x_off so every step function
    // can use cx as the true screen center without knowing about the slide.
    BeginScissorMode((int)card.x, (int)card.y, (int)card.width, (int)card.height);
    rlPushMatrix();
    rlTranslatef(x_off, 0.0f, 0.0f);

    switch (s_step) {
        case 0: DrawStepIntro(cx, content_cy, alpha); break;
        case 1: DrawStepGoToSettings(cx, content_cy, alpha); break;
        case 2: DrawStepCreateApp(cx, content_cy, alpha); break;
        case 3: DrawStepClientID(cx, content_cy, alpha, card); break;
        case 4: DrawStepClientSecret(cx, content_cy, alpha); break;
        case 5: DrawStepFetching(cx, content_cy, alpha); break;
        case 6: DrawStepDone(cx, content_cy, alpha); break;
    }

    rlPopMatrix();
    EndScissorMode();

    // ── Bottom buttons ────────────────────────────────────────────────────────
    float btn_y   = card.y + ch - 56.0f;
    float btn_h   = 38.0f;
    float btn_pad = 20.0f;

    bool show_back = (s_step > 0 && s_step < 5 && !s_fetching);
    bool show_next = (s_step < 5 && !s_fetching);
    bool show_close_btn = (s_step == 6);
    bool show_retry = (s_step == 6 && !s_error_msg.empty());

    // Validate: can we advance?
    bool can_next = true;
    if (s_step == 3 && strlen(s_client_id) == 0)     can_next = false;
    if (s_step == 4 && strlen(s_client_secret) == 0) can_next = false;

    const char* next_label = (s_step == 4) ? "CONNECT" : (s_step == 0 ? "GET STARTED" : "NEXT  >");

    if (show_back && show_next) {
        float bw = (cw - btn_pad * 3.0f) * 0.5f;
        if (UIButtonGhost(190, {card.x + btn_pad, btn_y, bw, btn_h}, "< BACK", 13.0f))
            GoTo(s_step - 1);
        if (can_next) {
            if (UIButtonAccent(191, {card.x + btn_pad * 2.0f + bw, btn_y, bw, btn_h}, next_label, 13.0f)) {
                if (s_step == 4) { GoTo(5); StartFetch(); }
                else              GoTo(s_step + 1);
            }
        } else {
            // Greyed out next
            DrawRectRounded({card.x + btn_pad * 2.0f + bw, btn_y, bw, btn_h}, 10.0f,
                            ColorAlpha(UI_SURFACE2, s_card.value));
            DrawRectRoundedBorder({card.x + btn_pad * 2.0f + bw, btn_y, bw, btn_h}, 10.0f, 1.5f,
                                  ColorAlpha(UI_BORDER, s_card.value));
            DrawTextCentered(ui.font_body, next_label, card.x + btn_pad * 2.0f + bw + bw*0.5f,
                             btn_y + btn_h*0.5f - 7.0f, 13.0f, 0.5f,
                             ColorAlpha(UI_TEXT_MUTED, s_card.value * 0.4f));
        }
    } else if (show_next) {
        float bw = cw - btn_pad * 2.0f;
        if (UIButtonAccent(191, {card.x + btn_pad, btn_y, bw, btn_h}, next_label, 13.0f))
            GoTo(s_step + 1);
    }

    if (show_retry) {
        float bw = (cw - btn_pad * 3.0f) * 0.5f;
        if (UIButtonGhost(192, {card.x + btn_pad, btn_y, bw, btn_h}, "TRY AGAIN", 13.0f))
            GoTo(3); // go back to client ID entry
        if (UIButtonAccent(193, {card.x + btn_pad*2.0f + bw, btn_y, bw, btn_h}, "CLOSE", 13.0f))
            OAuthPopup::Hide();
    } else if (show_close_btn) {
        float bw = cw - btn_pad * 2.0f;
        if (UIButtonAccent(193, {card.x + btn_pad, btn_y, bw, btn_h}, "START SEARCHING", 13.0f))
            OAuthPopup::Hide();
    }

    // Click outside to close (only on non-input steps)
    if (s_step != 3 && s_step != 4 && s_step != 5) {
        if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON) &&
            !CheckCollisionPointRec(GetMousePosition(), card))
            OAuthPopup::Hide();
    }
}
