#pragma once

#include <windows.h>

#include "app_gui.h"

#include <functional>

// The borderless main window: hit testing, resizing, fullscreen and keys.
namespace app_window {

// Centered on the monitor under the cursor. A size of 0 means the default.
HWND create(HINSTANCE instance, app_gui::Frame& gui, int width, int height);

// Size of the window when neither maximized nor minimized. False in fullscreen,
// where the window's size says nothing about the normal one.
bool normal_size(HWND hwnd, int& width, int& height);

// Repaints while Windows runs its modal size/move loop. Null detaches it.
void set_paint_callback(std::function<void()> paint);

// Called when Windows is about to sleep or end the session. Null detaches it.
void set_power_down_callback(std::function<void()> callback);

// ImGui is initialised and may receive window messages.
void set_imgui_ready(bool ready);

// True during a modal size or move: the main loop must not block.
bool sizing();

} // namespace app_window
