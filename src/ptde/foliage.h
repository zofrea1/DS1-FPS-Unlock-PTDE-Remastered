#pragma once

struct IDirect3DDevice9;

// FoliageSway: grass, flowers, bushes, ivy and leaves sway in the wind as in Remastered (see foliage.cpp): the sway of
// every mesh Remastered made sway is worked out as its model loads, and the static models' vertex shaders get a copy
// that moves those vertices. Strength and speed in percent of Remastered's.
// FoliageTreeSway: the leaves of trees too (Firelink Shrine, the Undead Burg and Parish, the way to Sen's Fortress,
// Anor Londo; see foliage.cpp). With FoliageSway on they are always marked and the shaders always have the trees' copy,
// so foliage_set_trees switches them at once.
void foliage_configure(bool on, int strength_percent, int speed_percent, bool trees_on);
void foliage_set_trees(bool on);
bool foliage_trees();
// The model loader's hook (before any map loads).
bool foliage_install();
// The device's CreateVertexShader and SetVertexShader (at the device's creation, before the game makes its shaders).
void foliage_hook_device(IDirect3DDevice9* dev);
// RemoteControl: `sway N`, `swayspeed N`, `swaystats`.
void foliage_set_strength(int percent);
void foliage_set_speed(int percent);
void foliage_log_stats();
