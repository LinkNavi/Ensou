#include "raylib.h"
#include "screens.h"
#include "ui.h"
#include "screen_mainmenu.h"
#include "screen_settings.h"
#include "screen_beatmaps.h"
#include "screen_song_select.h"
#include "beatmap/library.h"
#include "beatmap/downloader.h"
#include <raymath.h>

Screen current_screen = MAIN_MENU;

int main() {
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT);
    InitWindow(1280, 720, "Ensou");
    SetTargetFPS(240);
    InitAudioDevice();

    ui.LoadFonts("skin/body.ttf", "skin/heading.ttf", "skin/mono.ttf");

    int found = beatmap_library.Scan("songs");
    TraceLog(LOG_INFO, "BeatmapLibrary: found %d song(s)", found);

    if (osu_downloader.LoadToken("config.ini"))
        TraceLog(LOG_INFO, "osu! credentials loaded from config.ini");
    else
        TraceLog(LOG_INFO, "No osu! credentials found");

    while (!WindowShouldClose()) {
        if (IsKeyPressed(KEY_ESCAPE))
            break;

        ui.Update();
        screen_transition.Update();
        osu_downloader.Update();

        BeginDrawing();
        ClearBackground(UI_BG);

        switch (current_screen) {
            case MAIN_MENU:   UpdateDrawMainMenu();    break;
            case BEATMAPS:    UpdateDrawBeatmaps();    break;
            case SETTINGS:    UpdateDrawSettings();    break;
            case SONG_SELECT: UpdateDrawSongSelect();  break;
            default:
                screen_transition.FadeTo([]{ current_screen = MAIN_MENU; });
                break;
        }

        EndDrawing();
    }

    ui.UnloadFonts();
    CloseAudioDevice();
    CloseWindow();
    return 0;
}
