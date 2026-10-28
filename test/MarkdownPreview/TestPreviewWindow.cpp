#include "PreviewApi.h"
#include <cstdio>
#include <stdexcept>
#include <string>

struct Results { unsigned ready = 0, errors = 0, stale = 0; bool closed = false, navigated = false; };
static void __cdecl Event(void* context, const NP3PreviewEvent* event)
{
    auto& result = *static_cast<Results*>(context);
    if (result.closed) { ++result.stale; }
    if (event->kind == NP3PreviewReady) { ++result.ready; }
    if (event->kind == NP3PreviewError) { ++result.errors; std::printf("Preview error: %08lx\n", static_cast<unsigned long>(event->result)); }
    if (event->kind == NP3PreviewNavigateSource) { result.navigated = true; std::printf("SOURCE %llu\n", static_cast<unsigned long long>(event->sourceOffset)); std::fflush(stdout); }
}
static void Pump(DWORD milliseconds, const Results* result = nullptr, bool navigation = false)
{
    const ULONGLONG end = GetTickCount64() + milliseconds;
    while (GetTickCount64() < end && (!result || (navigation ? !result->navigated : (!result->ready && !result->errors)))) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
    }
}
int wmain(int argc, wchar_t** argv)
{
    unsigned checks = 0;
    auto check = [&](bool okay, const char* name) { ++checks; if (!okay) { throw std::runtime_error(name); } };
    try {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (argc < 2) { throw std::runtime_error("DLL path required"); }
        HMODULE module = LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        check(module != nullptr, "load preview DLL");
        auto getApi = reinterpret_cast<NP3PreviewGetApi>(GetProcAddress(module, "NP3Preview_GetApi"));
        check(getApi != nullptr, "versioned C export");
        NP3PreviewApi api{};
        check(FAILED(getApi(999, sizeof(api), &api)), "reject incompatible ABI");
        check(SUCCEEDED(getApi(1, sizeof(api), &api)) && api.Begin && api.Close, "load ABI version 1");
        NP3PreviewHandle handle = nullptr;
        check(FAILED(api.Begin(nullptr, &handle)), "reject null request");
        WNDCLASSW cls{}; cls.lpfnWndProc = DefWindowProcW; cls.lpszClassName = L"NP3PreviewTestParent"; cls.hInstance = GetModuleHandleW(nullptr);
        RegisterClassW(&cls);
        HWND parent = CreateWindowW(cls.lpszClassName, L"Markdown preview test", WS_OVERLAPPEDWINDOW, 0, 0, 900, 700, nullptr, nullptr, cls.hInstance, nullptr);
        check(parent != nullptr, "create test parent");
        const std::string source = "# Preview\n\n**same** **same**\n\nA &amp; B\n\n| A | B |\n|---|---|\n| one | two |\n\n- [x] done\n\n```mermaid\ngraph TD; A-->B\n```\n\n```mermaid\nnot valid mermaid\n```\n\n```mermaid\n%%{init: {'securityLevel':'loose'}}%%\ngraph TD; A-->B\n```\n\n![missing](missing.png)\n\n<script>window.np3SourceExecutedSentinel=true</script>\n";
        Results results;
        NP3PreviewRequest request{sizeof(request), parent, 42, source.data(), source.size(), L"D:\\preview-test.md", {0,0,860,640}, FALSE, Event, &results};
        check(SUCCEEDED(api.Begin(&request, &handle)) && handle, "begin preview");
        Pump(35000, &results);
        check(results.ready == 1 && results.errors == 0, "render Markdown and Mermaid in WebView2");
        if (argc > 2 && std::wstring(argv[2]) == L"--serve") {
            std::puts("READY"); std::fflush(stdout);
            Pump(120000, &results, true);
            check(results.navigated, "browser test sends source navigation");
        }
        check(results.errors == 0, "malformed messages do not change preview lifecycle");
        results.closed = true;
        api.Close(handle);
        Pump(100);
        check(results.stale == 0, "no callbacks after close");
        for (unsigned i = 0; i < 25; ++i) {
            results = {};
            handle = nullptr;
            check(SUCCEEDED(api.Begin(&request, &handle)), "begin cancellation cycle");
            results.closed = true;
            api.Close(handle);
            Pump(10);
            check(results.stale == 0, "cancel pending creation without callback");
        }
        DestroyWindow(parent);
        std::printf("PASS: %u WebView DLL assertions\n", checks);
        // The host intentionally keeps the DLL loaded until process exit.
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL after %u WebView checks: %s\n", checks, error.what());
        return 1;
    }
}
