project(
    'Ensou',
    'cpp',
    meson_version: '>= 1.3.0',
    version: '0.1',
    default_options: ['warning_level=3', 'cpp_std=c++17'],
)

raylib = dependency('raylib')

include = include_directories('include')

sources = [
    'src/main.cpp',
    'src/ui.cpp',
    'src/ui/navbar.cpp',
    'src/ui/play_button.cpp',
    'src/screen_mainmenu.cpp',
    'src/screen_settings.cpp',
    'src/screen_beatmaps.cpp',
    'src/ui/oauth_popup.cpp',
    'src/screen_song_select.cpp',
    'src/beatmap/parser.cpp',
    'src/beatmap/library.cpp',
    'src/beatmap/downloader.cpp',
]

executable(
    'ensou',
    sources,
    include_directories: include,
    dependencies: raylib,
    install: true,
)
