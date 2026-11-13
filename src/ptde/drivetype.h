#pragma once

// The game's resource loader asks Windows what kind of drive each file it opens is on (0x6B6150 calls
// GetDriveTypeW for the file's root; hard disk, CD-ROM and network drives are handled differently) and does it while
// it holds the loader's locks (the worker loop 0x66C4F0 and the file load event queue 0x60B470), which the game thread
// also takes to ask for files. Under Wine the answer comes from the mount manager in another process (a device
// request through wineserver); on a phone under GameNative that took 50 to 600 ms a time while a level loaded, the
// game thread waiting behind it: the hitches after a loading screen and while new areas stream in.
// With CacheDriveType the answer is asked once per drive root and kept (a drive does not change its kind while the
// game runs). With `measure` (Profile) every call is timed and a [drive] line every 10 s says how many there were,
// how many Windows answered and how long they took.
void drivetype_install(bool cache, bool measure);
