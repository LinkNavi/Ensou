// play_button.h
#pragma once
#include <functional>
void UpdatePlayButton(std::function<void()> on_click = nullptr, float cx = -1.0f, float cy = -1.0f);