#include "ui.h"
#include "screens.h"
#include <raylib.h>
#include "ui/play_button.h"

static SpringAnim scale;
static Anim       pulse;

void UpdatePlayButton(std::function<void()> on_click, float cx, float cy) {
    int sw = GetScreenWidth();
    int sh = GetScreenHeight();
    float base = fminf((float)sw, (float)sh);

    if (cx < 0.0f) cx = sw / 2.0f;
    if (cy < 0.0f) cy = sh / 2.0f;

    // Pulse — slow sine wave
    pulse.target = sinf((float)GetTime() * 2.0f) * 0.01f;
    pulse.speed  = 4.0f;
    pulse.Update();

    // Hover scale
    float dist    = Vector2Distance(GetMousePosition(), {cx, cy});
    float outer_r = base * 0.15f;
    bool  hovered = dist < outer_r;

    scale.target = hovered ? 1.08f : 1.0f + pulse.value;
    scale.Update();

    // Click bounce
    if (hovered && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
        scale.Set(0.92f);
        if (on_click) on_click();
    }

    float r_outer = outer_r * scale.pos;
    float r_inner = base * 0.12f * scale.pos;

    // Glow ring on hover
    if (hovered)
        DrawCircle(cx, cy, r_outer + 6.0f, ColorAlpha(UI_ACCENT, 0.15f));

    DrawCircle(cx, cy, r_outer, UI_ACCENT_DARK);
    DrawCircle(cx, cy, r_inner, UI_ACCENT);

    // Label
    DrawTextCentered(ui.font_heading, "PLAY", cx, cy - 12.0f, 22.0f, 1.0f, UI_BG);
}