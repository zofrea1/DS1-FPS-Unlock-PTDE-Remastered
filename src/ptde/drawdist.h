#pragma once

// DrawAllMapParts: every map piece of the loaded maps is drawn from wherever the player stands, not only the ones
// the floor underfoot lists (its display groups): grass, rocks and ruins that the game showed only from certain
// places (Firelink's ledge grass from inside the shrine) are there from farther away too. See drawdist.cpp.
// LodDistance: the distances at which models switch to their lower levels of detail, in percent (100 = the
// game's; Remastered raised the default row's from 15/30 m to 25/50 m, about 167).
bool drawdist_install(bool all_parts, int lod_percent);
// RemoteControl: `drawall 0|1` for this session (takes effect at once when the map's draw state is known, else at
// the next area change or load); `lodpct N`.
void drawdist_set_all(bool on);
void drawdist_set_lod(int percent);
