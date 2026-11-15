// Stands in for DSfix 3.0's occlusion query wrapper (built as sunquery_dll\dinput8.dll for test_sunquery.exe): it
// calls the real GetData and then divides the count, so the call to GetData comes from a module named dinput8.dll.
// Built with PLAIN (as sunquery_game\game.dll) it stands for the game's own call: no wrapper in between.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

extern "C" __declspec(dllexport) HRESULT __cdecl call_get_data(IDirect3DQuery9* q, void* data, DWORD size,
                                                               DWORD flags) {
    const HRESULT hr = q->GetData(data, size, flags);
#ifndef PLAIN
    if (hr == S_OK && data && size >= sizeof(DWORD) && q->GetType() == D3DQUERYTYPE_OCCLUSION) {
        DWORD* pixels = static_cast<DWORD*>(data);
        pixels[0] = static_cast<DWORD>(pixels[0] / 11.25f + 0.5f);
    }
#endif
    return hr;
}
