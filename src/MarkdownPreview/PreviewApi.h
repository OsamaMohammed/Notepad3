#pragma once
#include <windows.h>
#include <stdint.h>

#define NP3_PREVIEW_ABI 1
typedef void* NP3PreviewHandle;
enum NP3PreviewEventKind {
    NP3PreviewReady = 1, NP3PreviewError, NP3PreviewNavigateSource,
    NP3PreviewToggleSource, NP3PreviewHostCommand
};
typedef struct NP3PreviewEvent {
    uint32_t cbSize;
    uint64_t generation;
    uint32_t kind;
    HRESULT result;
    uint64_t sourceOffset;
    uint32_t command;
} NP3PreviewEvent;
typedef void (__cdecl* NP3PreviewCallback)(void* context, const NP3PreviewEvent* event);
typedef struct NP3PreviewRequest {
    uint32_t cbSize;
    HWND parent;
    uint64_t generation;
    const char* utf8;
    uint64_t byteCount;
    const wchar_t* sourcePath;
    RECT bounds;
    BOOL dark;
    NP3PreviewCallback callback;
    void* context;
    const wchar_t* copyLabel;
    const wchar_t* openLinkLabel;
} NP3PreviewRequest;
typedef struct NP3PreviewApi {
    uint32_t cbSize;
    HRESULT (__cdecl* Begin)(const NP3PreviewRequest*, NP3PreviewHandle*);
    HRESULT (__cdecl* Resize)(NP3PreviewHandle, const RECT*);
    HRESULT (__cdecl* SetDark)(NP3PreviewHandle, BOOL);
    HRESULT (__cdecl* Copy)(NP3PreviewHandle);
    HRESULT (__cdecl* Focus)(NP3PreviewHandle);
    void (__cdecl* Close)(NP3PreviewHandle);
} NP3PreviewApi;
typedef HRESULT (__cdecl* NP3PreviewGetApi)(uint32_t, uint32_t, NP3PreviewApi*);

#ifdef __cplusplus
extern "C" {
#endif
HRESULT __cdecl NP3Preview_GetApi(uint32_t version, uint32_t apiSize, NP3PreviewApi* api);
#ifdef __cplusplus
}
#endif
