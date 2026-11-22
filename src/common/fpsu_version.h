#pragma once

// The FPS Unlock's versions: Prepare to Die Edition's (src\ptde\version.rc and its log) and Remastered's
// (src\dsr\version.rc and its log), each that game's last release, or its next one once the tree no longer behaves as
// the last (the texts and the log then name the same behaviour). A release for one game moves only that game's, and
// package_release.bat names each zip after its own. src\ptde\dllmain.cpp keeps the PTDE string literally in
// ptde_fps_unlock_version(), because Prepare to Co-op's tools\make_ini.ps1 reads it from there; a static_assert there
// keeps the two equal.
#define FPSU_VERSION_MAJOR 2
#define FPSU_VERSION_MINOR 9
#define FPSU_VERSION_PATCH 0
#define FPSU_VERSION_STR "2.9.0"

// Remastered: 2.6.0 changed only Prepare to Die Edition (Remastered stayed 2.5.0); 2.7.0 gave both games the same
// number; 2.8.0 and 2.9.0 again change only Prepare to Die Edition (Remastered stays 2.7.0).
#define FPSU_DSR_VERSION_MAJOR 2
#define FPSU_DSR_VERSION_MINOR 7
#define FPSU_DSR_VERSION_PATCH 0
#define FPSU_DSR_VERSION_STR "2.7.0"
