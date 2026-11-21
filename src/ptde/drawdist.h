#pragma once

#include <cstdint>

// DrawMapPieces (0 game, 1 adjacent, 2 all, 3 adjacent+): the game draws the map pieces the floor underfoot lists (its
// room's display groups), so grass, rocks and ruins showed only from certain places (Firelink's ledge grass from
// inside the shrine). adjacent adds the pieces seen from the rooms next to the player's (the floor's draw groups);
// adjacent+ those of the rooms up to three steps away (the neighbours' neighbours and theirs); all draws
// every piece of the loaded maps (costly: Demon Ruins draws Lost Izalith). The map pieces, their effects, their
// lights and (DrawMapObjects) their objects (since 2.7.1): characters, collisions and sounds keep the game's rooms (up
// to 2.7.0 all of them were widened, which loaded and woke the characters of the whole map: events that wait for one
// ran early, Solaire left the Undead Burg). See drawdist.cpp.
// LodDistance: the distances at which models switch to their lower levels of detail, in percent (100 = the
// game's; Remastered raised the default row's from 15/30 m to 25/50 m, about 167).
// EffectLodDistance: the distances at which fires (torches, bonfires) drop their embers and other detail, in percent.
bool drawdist_install(int mode, int lod_percent, int effect_percent);
// Once a frame (game thread): with adjacent or all, the detail rules (drawdist.cpp's top): a stand-in hidden while
// the finer pieces of its surfaces are drawn in its place, coarse far backdrops only where the game draws them, and the
// depth levels that let the finer of two overlapping pieces win.
void drawdist_tick();
// The detail rules' answer for a map piece (its block, its index in it): 0 = not theirs (the generic rule); else bit 0
// = drawn from any room in place of a stand-in (counts as a piece DrawMapPieces adds), bit 1 = drawn in a stand-in's
// place at all (the player was checked against its surfaces, not its box), bits 4-7 = its depth level + 1 (each level
// drawn a little farther in depth). Game thread; drawdist_role_gen() changes whenever any answer does.
uint8_t drawdist_piece_role(int block, int index);
uint32_t drawdist_role_gen();
// RemoteControl `detailrules 0|1` (diagnostics A/B): the detail rules off (every piece as the masks say, the generic
// depth bias) or on.
void drawdist_set_detail(int on);
// RemoteControl: `drawmode 0|1|2|3` for this session (takes effect within a few seconds: the floor is applied again);
// `lodpct N`.
void drawdist_set_mode(int mode);
void drawdist_set_lod(int percent);
void drawdist_set_effect_lod(int percent);  // RemoteControl `fxlodpct N`
// Which managers adjacent/all widen: 1 the map pieces, 2 the map effects, 4 the point lights, 8 the objects (their draw
// and streaming masks: loaded and drawn; DrawMapObjects, default true = 15, false = 7). Also RemoteControl `drawwiden
// MASK` (a manager left out keeps the game's masks from the next rebuild on).
void drawdist_set_widen(int mask);
// A few scenery characters (Firelink's crow, with adjacent and all) are kept active wherever their map is loaded, so
// they are seen from farther (drawdist.cpp's kScenery). RemoteControl `scenery 0|1`.
void drawdist_set_scenery(int on);  // 2: kept active at the game's rate (diagnostics)
// The game's own draw mask of a map block (what it draws of that map from where the player is, as 0xDE2C60 wrote it
// before DrawMapPieces widened it): false when not known (not loaded, or not noted yet: the live mask is the game's
// then). Game thread. The generation changes each time the masks are written.
bool drawdist_game_mask(int block, uint32_t out[4]);
uint32_t drawdist_game_mask_gen();
// The same for the map effects' blocks (WorldSfxManImp's draw mask, +0x1C of its 0x3C-byte blocks).
bool drawdist_game_sfx_mask(int block, uint32_t out[4]);
