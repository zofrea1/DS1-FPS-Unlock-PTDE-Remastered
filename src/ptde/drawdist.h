#pragma once

// DrawMapPieces (0 game, 1 adjacent, 2 all): the game draws the map pieces the floor underfoot lists (its room's
// display groups), so grass, rocks and ruins showed only from certain places (Firelink's ledge grass from inside the
// shrine). adjacent adds the pieces seen from the rooms next to the player's (the floor's draw groups); all draws
// every piece of the loaded maps (costly: Demon Ruins draws Lost Izalith). Only the map pieces: characters, objects,
// collisions, effects, sounds and lights keep the game's rooms (2.7.0 widened them all, which loaded and woke the
// characters of the whole map: events that wait for one ran early, Solaire left the Undead Burg). See drawdist.cpp.
// LodDistance: the distances at which models switch to their lower levels of detail, in percent (100 = the
// game's; Remastered raised the default row's from 15/30 m to 25/50 m, about 167).
// EffectLodDistance: the distances at which fires (torches, bonfires) drop their embers and other detail, in percent.
bool drawdist_install(int mode, int lod_percent, int effect_percent);
// Once a frame (game thread): with adjacent or all, a map piece that would be drawn over a twin of the same surfaces
// (a detailed piece and its stand-in) is hidden while the twin is the player's room's own; put back otherwise.
void drawdist_tick();
// RemoteControl: `drawmode 0|1|2` for this session (takes effect within a few seconds: the floor is applied again);
// `lodpct N`.
void drawdist_set_mode(int mode);
void drawdist_set_lod(int percent);
void drawdist_set_effect_lod(int percent);  // RemoteControl `fxlodpct N`
