#include "screen_mainmenu.h"
#include "screens.h"
#include "ui.h"
#include "ui/navbar.h"
#include "ui/play_button.h"
#include <raymath.h>

extern Screen current_screen;
extern ScreenTransition screen_transition;

enum MainMenuIDs { BTN_SETTINGS = 0, BTN_QUIT };

void UpdateDrawMainMenu() {
    int sw = GetScreenWidth();
    int sh = GetScreenHeight();

    DrawRectangle(0, 0, sw, sh, UI_BG);
    DrawNavBar("MAIN MENU");

    float mid_y = NAVBAR_H + (sh - NAVBAR_H) * 0.5f;
    float left_x = sw * 0.12f;

    DrawTextEx(ui.font_heading, "ENSOU", {left_x, mid_y - 90.0f}, 72.0f, 2.0f, UI_ACCENT);
    DrawTextEx(ui.font_body, "rhythm game", {left_x, mid_y - 14.0f}, 15.0f, 3.0f, UI_TEXT_MUTED);
    DrawLineEx({left_x, mid_y + 12.0f}, {left_x + 120.0f, mid_y + 12.0f}, 1.0f, UI_BORDER);

    float bw = 220, bh = 48;
    float by = mid_y + 36.0f;

    if (UIButton(BTN_SETTINGS, {left_x, by, bw, bh}, "SETTINGS"))
        screen_transition.FadeTo([]{ current_screen = SETTINGS; });

    if (UIButtonGhost(BTN_QUIT, {left_x, by + 60.0f, bw, bh}, "QUIT"))
        CloseWindow();

    float right_cx = sw * 0.72f;
    float right_cy = mid_y;

    UpdatePlayButton([](){
        screen_transition.FadeTo([]{ current_screen = SONG_SELECT; });
    }, right_cx, right_cy);

    ui.DrawParticles();
    screen_transition.Draw();
}
