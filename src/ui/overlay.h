/* The in-game layer: Dear ImGui drawn over the game's own frames, through the
 * renderer's present-time overlay (ps3recomp rsx_overlay_set: Direct3D 12 for
 * now), and what it shows -- a trophy's pop-up when the game unlocks one
 * (ps3recomp's sceNpTrophy reads the disc's trophy pack, keeps the unlocks
 * and tells us, trophy_host.h).
 *
 * The pop-up is the launcher's look (ui/look.h): Inter, the panel colour, the
 * trophy's own icon from the pack, the grade's colour. One at a time, from
 * the top right of the window: slid in, held, slid out; a platinum the
 * unlock completes follows its trophy.
 *
 * DOD3_TROPHY_POPUPS=0 (dod3.ini) leaves the pop-ups out; the trophies are
 * still kept. Tests: PS3_TROPHY_PREVIEW=<id>[,<id>] shows those pop-ups
 * at boot without unlocking anything; RSX_OVERLAY=0 turns the layer off. */
#pragma once

/* main.cpp, before the game starts: the layer, the trophy listener, and the
 * sync's client name. */
void dod3_overlay_install();
