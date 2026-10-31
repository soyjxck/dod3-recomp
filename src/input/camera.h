/* The camera's stick on its way from the script to the pawn: src/input/camera.cpp. */
#pragma once

/* The script's calls (tools/menu_patch.py; the bridge in src/ui/settings_menu.cpp). */
void dod3_camera_frame(float delta, int dragon); /* 11: SendOrder's fDelta, before the rest; is the pawn the dragon */
int dod3_camera_active(); /* 12: is the mouse turning the camera? */
float dod3_camera_axis(int axis, float stick); /* 13: the camera stick's X (0) or Y (1) -> the value to use */
void dod3_camera_view(int pitch, int yaw, int gameplay); /* 14: the view's rotation, Unreal units */

/* When the mouse last turned the camera (dod3_now_us), for whoever asks
 * whether the player is on the keyboard and mouse. */
unsigned long long dod3_camera_mouse_time();
/* DOD3_MOUSE_SENS may have changed (the settings page). */
void dod3_camera_reload();

/* The dragon's shooting cursor (src/patches/dragon_cursor.cpp), once a frame
 * while it is on screen: the mouse's motion since the last frame as a move of
 * the cursor, in fractions of the screen's width and height (false: none, or
 * no mouse look). The view gets nothing of it meanwhile, except what the
 * cursor hands back with dod3_camera_cursor_spill: the part of a move that
 * went past the screen's edge. */
bool dod3_camera_cursor_motion(float* dx, float* dy);
void dod3_camera_cursor_spill(float dx, float dy);
