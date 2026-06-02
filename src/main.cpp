#include "raylib.h"
#include "screens.h"
#include "ui.h"
#include "screen_mainmenu.h"
#include "screen_settings.h"
#include "screen_beatmaps.h"
#include "beatmap/library.h"
#include "beatmap/downloader.h"
#include <raymath.h>
Screen current_screen = MAIN_MENU;

int main() {
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT);
    InitWindow(1280, 720, "Ensou");
    SetTargetFPS(240);
    InitAudioDevice();

    // Load fonts — put your ttf files in skin/
    ui.LoadFonts("skin/body.ttf", "skin/heading.ttf", "skin/mono.ttf");

    // Scan songs directory for beatmaps
    int found = beatmap_library.Scan("songs");
    TraceLog(LOG_INFO, "BeatmapLibrary: found %d song(s)", found);

    // Load osu! token from config if present
    if (osu_downloader.LoadToken("config.ini"))
        TraceLog(LOG_INFO, "osu! token loaded from config.ini");
    else
        TraceLog(LOG_INFO, "No osu! token found — paste one in Settings to enable downloads");

    while (!WindowShouldClose()) {
        if (IsKeyPressed(KEY_ESCAPE))
            break;

        ui.Update();
        screen_transition.Update();
        osu_downloader.Update();

        BeginDrawing();
        ClearBackground(UI_BG);

        switch (current_screen) {
            case MAIN_MENU: UpdateDrawMainMenu(); break;
            case BEATMAPS:   UpdateDrawBeatmaps();  break;
            case SETTINGS:  UpdateDrawSettings(); break;
            default: {
                TraceLog(LOG_INFO, "Not Supported Yet!");
                screen_transition.FadeTo([]{ current_screen = MAIN_MENU; });


            };
        }

        EndDrawing();
    }

    ui.UnloadFonts();
    CloseAudioDevice();
    CloseWindow();
    return 0;
}
