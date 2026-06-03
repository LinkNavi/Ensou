#include "ui/navbar.h"
#include "screens.h"
#include "ui.h"

extern Screen current_screen;
extern ScreenTransition screen_transition;

static NavBtn btns[] = {
    {"MAIN_MENU", MAIN_MENU},
    {"SONGS", SONG_SELECT},
    {"BEATMAPS", BEATMAPS},
    {"SETTINGS", SETTINGS},
};
static int btn_count = 4;

void DrawNavBar(const char *title, bool show_back) {
  int sw = GetScreenWidth();

  DrawRectangle(0, 0, sw, NAVBAR_H, UI_SURFACE);
  DrawLineEx({0, (float)NAVBAR_H}, {(float)sw, (float)NAVBAR_H}, 1.0f,
             UI_BORDER);

  // Logo — vertically centered
  float logo_size = 22.0f;
  float logo_y = (NAVBAR_H - logo_size) * 0.5f;
  DrawTextEx(ui.font_heading, "ENSOU", {16, logo_y}, logo_size, 1.0f,
             UI_ACCENT);

  // Nav buttons — wider padding, vertically centered
  float btn_h = 32.0f;
  float btn_y = (NAVBAR_H - btn_h) * 0.5f;
  float bx = 110.0f;

  for (int i = 0; i < btn_count; i++) {
    float bw =
        MeasureTextEx(ui.font_body, btns[i].label, 14.0f, 0.5f).x + 48.0f;
    Rectangle r = {bx, btn_y, bw, btn_h};

    if (current_screen == (Screen)btns[i].screen) {
      UIButtonAccent(80 + i, r, btns[i].label, 13.0f);
    } else {
      int target = btns[i].screen;
      if (UIButtonGhost(80 + i, r, btns[i].label, 13.0f))
        screen_transition.FadeTo([target] { current_screen = (Screen)target; });
    }
    bx += bw + 8.0f;
  }

  // Centered page title — vertically centered
  float title_size = 14.0f;
  float title_y = (NAVBAR_H - title_size) * 0.5f;
  DrawTextCentered(ui.font_body, title, sw * 0.5f, title_y, title_size, 2.0f,
                   UI_TEXT_MUTED);

  // Back button (right side) — vertically centered
  if (show_back) {
    float back_w = 96.0f;
    if (UIButtonGhost(99, {(float)sw - back_w - 14.0f, btn_y, back_w, btn_h},
                      "< BACK", 13.0f))
      screen_transition.FadeTo([] { current_screen = MAIN_MENU; });
  }
}
