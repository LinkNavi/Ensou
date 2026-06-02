#include "ui.h"
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <raymath.h>
UIContext        ui;
ScreenTransition screen_transition;

// ─── Easing ───────────────────────────────────────────────────────────────────
float EaseOutCubic(float t)   { float x = 1.0f - t; return 1.0f - x*x*x; }
float EaseInCubic(float t)    { return t*t*t; }
float EaseOutBack(float t) {
    const float c1 = 1.70158f, c3 = c1 + 1.0f;
    return 1.0f + c3 * powf(t - 1.0f, 3.0f) + c1 * powf(t - 1.0f, 2.0f);
}
float EaseOutElastic(float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    const float c4 = (2.0f * PI) / 3.0f;
    return powf(2.0f, -10.0f * t) * sinf((t * 10.0f - 0.75f) * c4) + 1.0f;
}

// ─── Anim ─────────────────────────────────────────────────────────────────────
void Anim::Update() {
    float dt = GetFrameTime();
    value += (target - value) * fminf(speed * dt, 1.0f);
}

void SpringAnim::Update() {
    float dt    = GetFrameTime();
    float force = stiffness * (target - pos) - damping * vel;
    vel += force * dt;
    pos += vel * dt;
}

// ─── Color helpers ────────────────────────────────────────────────────────────
Color ColorLerp(Color a, Color b, float t) {
    t = fmaxf(0.0f, fminf(1.0f, t));
    return {
        (unsigned char)(a.r + (b.r - a.r) * t),
        (unsigned char)(a.g + (b.g - a.g) * t),
        (unsigned char)(a.b + (b.b - a.b) * t),
        (unsigned char)(a.a + (b.a - a.a) * t),
    };
}

Color ColorAlpha(Color c, float alpha) {
    c.a = (unsigned char)(fmaxf(0.0f, fminf(1.0f, alpha)) * 255.0f);
    return c;
}

// ─── Particles ────────────────────────────────────────────────────────────────
void ParticleSystem::Emit(Vector2 pos, Color color, int count) {
    for (int i = 0; i < count; i++) {
        for (int j = 0; j < MAX_PARTICLES; j++) {
            if (!particles[j].active) {
                float angle = ((float)rand() / RAND_MAX) * 2.0f * PI;
                float speed = 60.0f + ((float)rand() / RAND_MAX) * 140.0f;
                particles[j] = {
                    pos,
                    { cosf(angle) * speed, sinf(angle) * speed - 40.0f },
                    1.0f, 1.0f,
                    3.0f + ((float)rand() / RAND_MAX) * 4.0f,
                    color, true
                };
                break;
            }
        }
    }
}

void ParticleSystem::Update() {
    float dt = GetFrameTime();
    for (auto& p : particles) {
        if (!p.active) continue;
        p.vel.y += 200.0f * dt;
        p.pos.x += p.vel.x * dt;
        p.pos.y += p.vel.y * dt;
        p.life  -= dt * 1.8f;
        if (p.life <= 0.0f) p.active = false;
    }
}

void ParticleSystem::Draw() {
    for (auto& p : particles) {
        if (!p.active) continue;
        float t = p.life / p.max_life;
        DrawCircleV(p.pos, p.size * t, ColorAlpha(p.color, t * t));
    }
}

// ─── UIContext ────────────────────────────────────────────────────────────────
void UIContext::LoadFonts(const char* body, const char* heading, const char* mono) {
    font_body    = LoadFontEx(body,    32, nullptr, 512);
    font_heading = LoadFontEx(heading, 64, nullptr, 512);
    font_mono    = LoadFontEx(mono,    32, nullptr, 256);
    SetTextureFilter(font_body.texture,    TEXTURE_FILTER_BILINEAR);
    SetTextureFilter(font_heading.texture, TEXTURE_FILTER_BILINEAR);
    SetTextureFilter(font_mono.texture,    TEXTURE_FILTER_BILINEAR);
    fonts_loaded = true;
}

void UIContext::UnloadFonts() {
    if (!fonts_loaded) return;
    UnloadFont(font_body);
    UnloadFont(font_heading);
    UnloadFont(font_mono);
}

void UIContext::Update() {
    for (auto& s : states) {
        s.hover.Update();
        s.press.Update();
        s.scale.Update();
    }
    particles.Update();
}

void UIContext::DrawParticles() { particles.Draw(); }

// ─── Draw helpers ─────────────────────────────────────────────────────────────
void DrawRectRounded(Rectangle r, float radius, Color color) {
    DrawRectangleRounded(r, radius / fminf(r.width, r.height), 12, color);
}

void DrawRectRoundedBorder(Rectangle r, float radius, float thick, Color color) {
    DrawRectangleRoundedLinesEx(r, radius / fminf(r.width, r.height), 12, thick, color);
}

void DrawTextCentered(Font font, const char* text, float cx, float y, float size, float spacing, Color color) {
    Vector2 m = MeasureTextEx(font, text, size, spacing);
    DrawTextEx(font, text, { cx - m.x * 0.5f, y }, size, spacing, color);
}

void DrawTextRight(Font font, const char* text, float right, float y, float size, float spacing, Color color) {
    Vector2 m = MeasureTextEx(font, text, size, spacing);
    DrawTextEx(font, text, { right - m.x, y }, size, spacing, color);
}

// ─── Internal ─────────────────────────────────────────────────────────────────
static bool IsHovered(Rectangle b) {
    return CheckCollisionPointRec(GetMousePosition(), b);
}

static Rectangle ScaleRect(Rectangle r, float s) {
    float dw = r.width  * (s - 1.0f) * 0.5f;
    float dh = r.height * (s - 1.0f) * 0.5f;
    return { r.x - dw, r.y - dh, r.width + dw*2, r.height + dh*2 };
}

static WidgetState& WS(int id) { return ui.states[id % MAX_WIDGET_STATES]; }
static Font FB() { return ui.fonts_loaded ? ui.font_body    : GetFontDefault(); }
static Font FH() { return ui.fonts_loaded ? ui.font_heading : GetFontDefault(); }

// ─── UIPanel / label ──────────────────────────────────────────────────────────
void UIPanel(Rectangle bounds, float radius) {
    DrawRectRounded(bounds, radius, UI_SURFACE);
    DrawRectRoundedBorder(bounds, radius, 1.0f, UI_BORDER);
}

void UISectionLabel(float x, float y, const char* text) {
    DrawTextEx(FB(), text, {x, y}, 11.0f, 2.0f, UI_TEXT_MUTED);
}

// ─── UIButton ────────────────────────────────────────────────────────────────
bool UIButton(int id, Rectangle bounds, const char* label, float font_size) {
    auto& ws  = WS(id);
    bool  hov = IsHovered(bounds);
    bool  clk = hov && IsMouseButtonPressed(MOUSE_LEFT_BUTTON);

    ws.hover.target = hov ? 1.0f : 0.0f; ws.hover.speed = 14.0f;
    ws.press.target = (hov && IsMouseButtonDown(MOUSE_LEFT_BUTTON)) ? 1.0f : 0.0f;
    ws.press.speed  = 20.0f;
    if (clk) { ws.scale.Set(0.93f); ui.particles.Emit({bounds.x+bounds.width*.5f,bounds.y+bounds.height*.5f}, UI_ACCENT, 6); }
    ws.scale.target = 1.0f;

    Rectangle r = ScaleRect(bounds, ws.scale.pos > 0.01f ? ws.scale.pos : 1.0f);
    DrawRectRounded(r, 10.0f, ColorLerp(UI_SURFACE2, UI_SURFACE, ws.hover.value));
    DrawRectRoundedBorder(r, 10.0f, 1.5f, ColorLerp(UI_BORDER, UI_ACCENT, ws.hover.value * 0.7f));
    if (ws.hover.value > 0.01f)
        DrawRectRounded(r, 10.0f, ColorAlpha(UI_ACCENT, ws.hover.value * 0.08f));
    DrawTextCentered(FB(), label, r.x+r.width*.5f, r.y+r.height*.5f-font_size*.5f, font_size, 0.5f,
                     ColorLerp(UI_TEXT_MUTED, UI_TEXT, ws.hover.value));
    return clk;
}

// ─── UIButtonAccent ───────────────────────────────────────────────────────────
bool UIButtonAccent(int id, Rectangle bounds, const char* label, float font_size) {
    auto& ws  = WS(id);
    bool  hov = IsHovered(bounds);
    bool  clk = hov && IsMouseButtonPressed(MOUSE_LEFT_BUTTON);

    ws.hover.target = hov ? 1.0f : 0.0f; ws.hover.speed = 14.0f;
    if (clk) { ws.scale.Set(0.93f); ui.particles.Emit({bounds.x+bounds.width*.5f,bounds.y+bounds.height*.5f}, UI_ACCENT, 12); }
    ws.scale.target = 1.0f;

    Rectangle r = ScaleRect(bounds, ws.scale.pos > 0.01f ? ws.scale.pos : 1.0f);
    DrawRectRounded(r, 10.0f, ColorLerp(UI_ACCENT_DARK, UI_ACCENT, ws.hover.value));
    DrawRectRounded({r.x, r.y, r.width, r.height*.5f}, 10.0f, ColorAlpha(UI_WHITE, 0.08f + ws.hover.value*0.04f));
    DrawTextCentered(FB(), label, r.x+r.width*.5f, r.y+r.height*.5f-font_size*.5f, font_size, 0.5f, UI_BG);
    return clk;
}

// ─── UIButtonGhost ────────────────────────────────────────────────────────────
bool UIButtonGhost(int id, Rectangle bounds, const char* label, float font_size) {
    auto& ws  = WS(id);
    bool  hov = IsHovered(bounds);
    bool  clk = hov && IsMouseButtonPressed(MOUSE_LEFT_BUTTON);

    ws.hover.target = hov ? 1.0f : 0.0f; ws.hover.speed = 14.0f;
    if (clk) ws.scale.Set(0.95f);
    ws.scale.target = 1.0f;

    Rectangle r = ScaleRect(bounds, ws.scale.pos > 0.01f ? ws.scale.pos : 1.0f);
    DrawRectRounded(r, 10.0f, ColorAlpha(UI_ACCENT, ws.hover.value * 0.07f));
    DrawRectRoundedBorder(r, 10.0f, 1.5f, ColorLerp(UI_BORDER, UI_ACCENT, ws.hover.value));
    DrawTextCentered(FB(), label, r.x+r.width*.5f, r.y+r.height*.5f-font_size*.5f, font_size, 0.5f,
                     ColorLerp(UI_TEXT_MUTED, UI_ACCENT, ws.hover.value));
    return clk;
}

// ─── UIButtonIcon ─────────────────────────────────────────────────────────────
bool UIButtonIcon(int id, Rectangle bounds, const char* icon, float font_size) {
    auto& ws  = WS(id);
    bool  hov = IsHovered(bounds);
    bool  clk = hov && IsMouseButtonPressed(MOUSE_LEFT_BUTTON);

    ws.hover.target = hov ? 1.0f : 0.0f; ws.hover.speed = 16.0f;
    if (clk) ws.scale.Set(0.88f);
    ws.scale.target = 1.0f;

    Rectangle r = ScaleRect(bounds, ws.scale.pos > 0.01f ? ws.scale.pos : 1.0f);
    DrawRectRounded(r, r.width*.5f, ColorAlpha(UI_ACCENT, ws.hover.value * 0.15f));
    if (ws.hover.value > 0.01f)
        DrawRectRoundedBorder(r, r.width*.5f, 1.5f, ColorAlpha(UI_ACCENT, ws.hover.value * 0.6f));
    DrawTextCentered(FB(), icon, r.x+r.width*.5f, r.y+r.height*.5f-font_size*.5f, font_size, 0.0f,
                     ColorLerp(UI_TEXT_MUTED, UI_ACCENT, ws.hover.value));
    return clk;
}

// ─── UISlider ────────────────────────────────────────────────────────────────
bool UISlider(int id, Rectangle bounds, float min, float max, float* value, const char* label) {
    auto& ws  = WS(id);
    bool  hov = IsHovered(bounds);
    bool  drag = false;
    float t   = (*value - min) / (max - min);

    if (hov && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) ws.press.Snap(1.0f);
    if (ws.press.value > 0.5f && IsMouseButtonDown(MOUSE_LEFT_BUTTON)) {
        t = (GetMousePosition().x - bounds.x) / bounds.width;
        t = fmaxf(0.0f, fminf(1.0f, t));
        *value = min + t * (max - min);
        drag = true;
    }
    if (IsMouseButtonReleased(MOUSE_LEFT_BUTTON)) ws.press.Snap(0.0f);
    ws.hover.target = hov ? 1.0f : 0.0f; ws.hover.speed = 14.0f;

    float th = 4.0f;
    Rectangle track = { bounds.x, bounds.y + bounds.height*.5f - th*.5f, bounds.width, th };
    DrawRectRounded(track, th, UI_SURFACE2);
    if (t > 0.0f) DrawRectRounded({track.x, track.y, track.width*t, th}, th, UI_ACCENT);

    float tr = 7.0f + ws.hover.value * 3.0f;
    Vector2 tp = { bounds.x + bounds.width*t, bounds.y + bounds.height*.5f };
    DrawCircleV(tp, tr + 2.0f, ColorAlpha(UI_ACCENT, 0.2f));
    DrawCircleV(tp, tr, UI_ACCENT);

    if (label) {
        char val_str[32];
        snprintf(val_str, sizeof(val_str), "%.1f", *value);
        DrawTextEx(FB(), label, {bounds.x, bounds.y - 20.0f}, 13.0f, 0.5f, UI_TEXT_MUTED);
        DrawTextRight(FB(), val_str, bounds.x + bounds.width, bounds.y - 20.0f, 13.0f, 0.5f, UI_ACCENT);
    }
    return drag;
}

// ─── UIToggle ────────────────────────────────────────────────────────────────
bool UIToggle(int id, Rectangle bounds, bool* value, const char* label) {
    auto& ws  = WS(id);
    bool  hov = IsHovered(bounds);
    bool  clk = hov && IsMouseButtonPressed(MOUSE_LEFT_BUTTON);

    if (clk) *value = !(*value);
    ws.hover.target = hov ? 1.0f : 0.0f; ws.hover.speed = 14.0f;
    ws.press.target = *value ? 1.0f : 0.0f; ws.press.speed = 12.0f;

    float radius = bounds.height * 0.5f;
    DrawRectRounded(bounds, radius, ColorLerp(UI_SURFACE2, UI_ACCENT, ws.press.value));
    DrawRectRoundedBorder(bounds, radius, 1.5f, ColorLerp(UI_BORDER, UI_ACCENT_DARK, ws.press.value));

    float kr  = bounds.height * 0.5f - 3.0f;
    float kx  = bounds.x + kr + 3.0f + (bounds.width - (kr+3.0f)*2.0f) * ws.press.value;
    DrawCircleV({kx, bounds.y + bounds.height*.5f}, kr, UI_WHITE);

    if (label)
        DrawTextEx(FB(), label, {bounds.x + bounds.width + 10.0f, bounds.y + bounds.height*.5f - 8.0f},
                   15.0f, 0.5f, UI_TEXT);
    return clk;
}

// ─── UISongCard ───────────────────────────────────────────────────────────────
bool UISongCard(int id, Rectangle bounds, const char* title, const char* artist,
                const char* diff, float stars, bool selected) {
    auto& ws  = WS(id);
    bool  hov = IsHovered(bounds);
    bool  clk = hov && IsMouseButtonPressed(MOUSE_LEFT_BUTTON);

    ws.hover.target = (hov || selected) ? 1.0f : 0.0f; ws.hover.speed = 12.0f;
    if (clk) ws.scale.Set(0.97f);
    ws.scale.target = 1.0f;

    Rectangle r = ScaleRect(bounds, ws.scale.pos > 0.01f ? ws.scale.pos : 1.0f);
    DrawRectRounded(r, 12.0f, selected ? UI_SURFACE2 : ColorLerp(UI_SURFACE, UI_SURFACE2, ws.hover.value));

    float bw = 3.0f + ws.hover.value;
    DrawRectRounded({r.x, r.y+8.0f, bw, r.height-16.0f}, bw,
                    selected ? UI_ACCENT : ColorAlpha(UI_ACCENT, ws.hover.value));
    DrawRectRoundedBorder(r, 12.0f, 1.5f,
                          selected ? ColorAlpha(UI_ACCENT, 0.5f)
                                   : ColorLerp(UI_BORDER, ColorAlpha(UI_ACCENT, 0.3f), ws.hover.value));

    float tx = r.x + 16.0f;
    DrawTextEx(FH(), title,  {tx, r.y+12.0f}, 18.0f, 0.3f, selected ? UI_ACCENT : UI_TEXT);
    DrawTextEx(FB(), artist, {tx, r.y+34.0f}, 13.0f, 0.3f, UI_TEXT_MUTED);

    float sy = r.y + r.height - 22.0f;
    DrawTextEx(FB(), diff, {tx, sy}, 12.0f, 0.5f, UI_TEXT_MUTED);

    char sbuf[32]; snprintf(sbuf, sizeof(sbuf), "%.1f*", stars);
    Color sc = stars < 4.0f ? UI_ACCENT : stars < 6.0f ? (Color){255,200,80,255} : (Color){255,100,100,255};
    DrawTextRight(FB(), sbuf, r.x+r.width-12.0f, sy, 12.0f, 0.5f, sc);
    return clk;
}

// ─── UIBar ────────────────────────────────────────────────────────────────────
void UIBar(int id, Rectangle bounds, float value, float max, Color fill_color, bool animated) {
    auto& ws = WS(id);
    ws.hover.speed  = 5.0f;
    ws.hover.target = value / max;
    if (!animated) ws.hover.Snap(value / max);

    DrawRectRounded(bounds, bounds.height, UI_SURFACE2);
    if (ws.hover.value > 0.001f) {
        DrawRectRounded({bounds.x, bounds.y, bounds.width*ws.hover.value, bounds.height}, bounds.height, fill_color);
        float hx = bounds.x + bounds.width * ws.hover.value;
        DrawRectRounded({hx-6.0f, bounds.y, 6.0f, bounds.height}, bounds.height, ColorAlpha(UI_WHITE, 0.2f));
    }
}

// ─── UIComboPopup ─────────────────────────────────────────────────────────────
void UIComboPopup(int id, Vector2 anchor, int combo, bool just_increased) {
    auto& ay = ui.states[(id*2)   % MAX_WIDGET_STATES].hover;
    auto& aa = ui.states[(id*2+1) % MAX_WIDGET_STATES].hover;

    if (just_increased) { ay.Snap(0.0f); aa.Snap(1.0f); ay.target = 1.0f; aa.target = 0.0f; }
    ay.speed = 2.5f; aa.speed = 0.8f;
    if (aa.value < 0.01f) return;

    char buf[32]; snprintf(buf, sizeof(buf), "%d", combo);
    float size = 28.0f + (1.0f - ay.value) * 8.0f;
    DrawTextCentered(FH(), buf, anchor.x, anchor.y - 40.0f * EaseOutCubic(ay.value),
                     size, 0.5f, ColorAlpha(UI_ACCENT, aa.value));
}

// ─── ScreenTransition ────────────────────────────────────────────────────────
void ScreenTransition::FadeTo(std::function<void()> switch_fn) {
    if (fading_out || fading_in) return;
    on_switch  = switch_fn;
    fading_out = true;
    alpha.speed  = 8.0f;
    alpha.target = 1.0f;
}

void ScreenTransition::Update() {
    alpha.Update();
    if (fading_out && alpha.Done()) {
        if (on_switch) { on_switch(); on_switch = nullptr; }
        fading_out = false; fading_in = true;
        alpha.speed = 5.0f; alpha.target = 0.0f;
    }
    if (fading_in && alpha.Done()) fading_in = false;
}

void ScreenTransition::Draw() {
    if (alpha.value < 0.005f) return;
    DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), ColorAlpha(UI_BG, alpha.value));
}

bool ScreenTransition::Busy() const { return fading_out || fading_in; }
