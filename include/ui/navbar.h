#pragma once

#define NAVBAR_H 52

struct NavBtn {
    const char* label;
    int         screen;
};

void DrawNavBar(const char* title, bool show_back = false);
