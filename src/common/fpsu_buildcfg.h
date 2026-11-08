#pragma once

// FPSU_DIAGNOSTICS: 1 (the default) compiles the development diagnostics in: traces, hardware watchpoints, the
// render thread sampler, the input and stamina probes and the test hotkeys ([Diagnostics] in the INI). Release
// builds (package_release.bat, which runs build_dsr.bat release and build_ptde.bat release) set it to 0: those
// modules become empty stubs, so the shipped DLL carries no thread suspension, thread context or debug register
// code and does not poll the keyboard. A build that bundles this mod (Prepare to Co-op) keeps the default.
#ifndef FPSU_DIAGNOSTICS
#define FPSU_DIAGNOSTICS 1
#endif
