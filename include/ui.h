#pragma once
#include "raylib.h"
#include <functional>
#include <cmath>
#include <raymath.h>

// ─── Colors ───────────────────────────────────────────────────────────────────
#define UI_BG           Color{ 10,  12,  16,  255 }
#define UI_SURFACE      Color{ 18,  22,  28,  255 }
#define UI_SURFACE2     Color{ 26,  32,  40,  255 }
#define UI_BORDER       Color{ 40,  50,  62,  255 }
#define UI_TEXT         Color{ 220, 230, 240, 255 }
#define UI_TEXT_MUTED   Color{ 100, 120, 140, 255 }
#define UI_ACCENT       Color{ 98,  222, 178, 255 }
#define UI_ACCENT_DIM   Color{ 98,  222, 178, 60  }
#define UI_ACCENT_DARK  Color{ 60,  150, 118, 255 }
#define UI_DANGER       Color{ 220, 80,  80,  255 }
#define UI_WHITE        Color{ 255, 255, 255, 255 }

// ─── Easing ───────────────────────────────────────────────────────────────────
float EaseOutCubic(float t);
float EaseOutBack(float t);
float EaseInCubic(float t);
float EaseOutElastic(float t);

// ─── Anim ─────────────────────────────────────────────────────────────────────
struct Anim {
    float value  = 0.0f;
    float target = 0.0f;
    float speed  = 12.0f;

    void  Update();
    void  Set(float t)           { target = t; }
    void  Snap(float t)          { value = target = t; }
    bool  Done(float eps = 0.01f) const { return fabsf(value - target) < eps; }
};

struct SpringAnim {
    float pos       = 0.0f;
    float vel       = 0.0f;
    float target    = 0.0f;
    float stiffness = 280.0f;
    float damping   = 22.0f;

    void Update();
    void Set(float t)  { target = t; }
    void Snap(float t) { pos = vel = 0; pos = t; target = t; }
};

// ─── Particles ────────────────────────────────────────────────────────────────
struct Particle {
    Vector2 pos, vel;
    float   life, max_life;
    float   size;
    Color   color;
    bool    active = false;
};

#define MAX_PARTICLES 256

struct ParticleSystem {
    Particle particles[MAX_PARTICLES];
    void Emit(Vector2 pos, Color color, int count = 8);
    void Update();
    void Draw();
};

// ─── Widget state ─────────────────────────────────────────────────────────────
#define MAX_WIDGET_STATES 64

struct WidgetState {
    Anim       hover;
    Anim       press;
    SpringAnim scale;
    bool       was_hovered = false;
};

struct UIContext {
    WidgetState    states[MAX_WIDGET_STATES];
    Font           font_body;
    Font           font_heading;
    Font           font_mono;
    bool           fonts_loaded = false;
    ParticleSystem particles;

    void LoadFonts(const char* body, const char* heading, const char* mono);
    void UnloadFonts();
    void Update();
    void DrawParticles();
};

extern UIContext ui;

// ─── Draw helpers ─────────────────────────────────────────────────────────────
void  DrawRectRounded(Rectangle r, float radius, Color color);
void  DrawRectRoundedBorder(Rectangle r, float radius, float thick, Color color);
void  DrawTextCentered(Font font, const char* text, float cx, float y, float size, float spacing, Color color);
void  DrawTextRight(Font font, const char* text, float right, float y, float size, float spacing, Color color);
Color ColorLerp(Color a, Color b, float t);
Color ColorAlpha(Color c, float alpha);

// ─── Widgets ──────────────────────────────────────────────────────────────────
bool UIButton(int id, Rectangle bounds, const char* label, float font_size = 18.0f);
bool UIButtonAccent(int id, Rectangle bounds, const char* label, float font_size = 18.0f);
bool UIButtonGhost(int id, Rectangle bounds, const char* label, float font_size = 18.0f);
bool UIButtonIcon(int id, Rectangle bounds, const char* icon, float font_size = 22.0f);
bool UISlider(int id, Rectangle bounds, float min, float max, float* value, const char* label = nullptr);
bool UIToggle(int id, Rectangle bounds, bool* value, const char* label = nullptr);
bool UISongCard(int id, Rectangle bounds, const char* title, const char* artist, const char* diff, float stars, bool selected);
void UIBar(int id, Rectangle bounds, float value, float max, Color fill_color = UI_ACCENT, bool animated = true);
void UIComboPopup(int id, Vector2 anchor, int combo, bool just_increased);
void UIPanel(Rectangle bounds, float radius = 10.0f);
void UISectionLabel(float x, float y, const char* text);

// ─── Screen transition ────────────────────────────────────────────────────────
struct ScreenTransition {
    Anim  alpha;
    bool  fading_out = false;
    bool  fading_in  = false;
    std::function<void()> on_switch;

    void FadeTo(std::function<void()> switch_fn);
    void Update();
    void Draw();
    bool Busy() const;
};

extern ScreenTransition screen_transition;
