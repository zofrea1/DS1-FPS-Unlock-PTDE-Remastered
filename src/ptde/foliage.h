#pragma once

struct IDirect3DDevice9;

// FoliageSway: grass, flowers, bushes, ivy and leaves sway in the wind as in Remastered (see foliage.cpp): the sway of
// every mesh Remastered made sway is worked out as its model loads, and the static models' vertex shaders get a copy
// that moves those vertices. Strength and speed in percent of Remastered's.
// FoliageTreeSway: trees too (the Undead Burg and Parish, the way to Sen's Fortress, Anor Londo, Firelink Shrine's
// great tree's crown; see foliage.cpp). Their marks go into the models as they load and their block into the shaders
// as the device makes them, so it is decided at the start.
void foliage_configure(bool on, int strength_percent, int speed_percent, bool trees_on);
// The model loader's hook (before any map loads).
bool foliage_install();
// The device's CreateVertexShader and SetVertexShader (at the device's creation, before the game makes its shaders).
void foliage_hook_device(IDirect3DDevice9* dev);
// RemoteControl: `sway N`, `swayspeed N`, `swaystats`.
void foliage_set_strength(int percent);
void foliage_set_speed(int percent);
void foliage_log_stats();
