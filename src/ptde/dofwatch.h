#pragma once

struct IDirect3DDevice9;

// Diagnostics builds: logs changes of the depth of field's pixel constants c8..c11 (see dofwatch.cpp).
void dofwatch_hook_device(IDirect3DDevice9* dev);
