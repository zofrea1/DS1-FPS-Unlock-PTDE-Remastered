#pragma once

// LargeAddressAware (default true, 2.7.1). DARKSOULS.exe (the Steam build, no DRM wrapper) is not large address aware:
// the game has 2 GB of address space, and at 4K with DXVK about 1.5 GB of it is in use on the Undead Burg's bridge
// alone. With more players in a co-op session (up to 8), the Undead Burg's enemies near a teammate and other heavy
// settings it ran out: a host crashed in NVIDIA's Vulkan driver at 1702 MB in use (2026-11-21), another at 1660 MB
// (2026-11-19). With the flag (IMAGE_FILE_LARGE_ADDRESS_AWARE in the COFF header) 64-bit Windows gives the game 4 GB.
//
// The address space of a running process is fixed when it starts, so the flag is set on the file for the next start:
// a copy with the flag (one byte, and the header checksum when it has one) is written next to the exe, the running exe
// is renamed DARKSOULS.exe.before-laa (Windows lets a running exe be renamed, not replaced) and the copy takes its
// name. Nothing is done when the game already has more than 2 GB (the flag set before, or Proton's
// WINE_LARGE_ADDRESS_AWARE), or for a file that is not the expected plain 32-bit executable. Steam's "verify integrity
// of game files" puts the original back, and the next start sets the flag again. false: the exe is left as it is.
void laa_ensure(bool enabled);
