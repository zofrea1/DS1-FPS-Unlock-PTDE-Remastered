#pragma once

// The FPS Unlock's version, for both DLLs and their version resources (src\ptde\version.rc, src\dsr\version.rc).
// src\ptde\dllmain.cpp keeps the same string literally in ptde_fps_unlock_version(), because Prepare to Co-op's
// tools\make_ini.ps1 reads it from there; a static_assert there keeps the two equal.
#define FPSU_VERSION_MAJOR 2
#define FPSU_VERSION_MINOR 2
#define FPSU_VERSION_PATCH 0
#define FPSU_VERSION_STR "2.2.0"
