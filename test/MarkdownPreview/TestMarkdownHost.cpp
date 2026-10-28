// Native integration test: drives a real Notepad3.exe through a preview toggle.
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <stdexcept>
#include <string>

#define IDM_VIEW_MARKDOWN_PREVIEW 41066
#define IDC_EDITFRAME 0xFB04
#define IDC_EDIT 0xFB03

#define TB_BUTTONCOUNT 0x0418

#define SCI_GETLENGTH 2006
#define SCI_GETCURRENTPOS 2008
#define SCI_GOTOPOS 2024

static const wchar_t PreviewWindowClass[] = L"NP3MarkdownPreviewChild";
static const wchar_t PreviewDllName[] = L"MarkdownPreview.dll";

struct MainSearch { DWORD process; HWND window; };

static BOOL CALLBACK FindMainWindow(HWND window, LPARAM param)
{
    MainSearch* const search = reinterpret_cast<MainSearch*>(param);
    DWORD owner = 0;
    GetWindowThreadProcessId(window, &owner);
    if (owner != search->process || !IsWindowVisible(window)) {
        return TRUE;
    }
    wchar_t name[64] = { 0 };
    GetClassNameW(window, name, ARRAYSIZE(name));
    if (wcscmp(name, L"Notepad3") != 0) {
        return TRUE;
    }
    search->window = window;
    return FALSE;
}

static HWND MainWindow(DWORD process)
{
    MainSearch search{ process, nullptr };
    EnumWindows(FindMainWindow, reinterpret_cast<LPARAM>(&search));
    return search.window;
}

static std::string Describe(DWORD process)
{
    struct Dump { DWORD process; std::string text; } dump{ process, {} };
    EnumWindows([](HWND window, LPARAM param) -> BOOL {
        Dump* const state = reinterpret_cast<Dump*>(param);
        DWORD owner = 0;
        GetWindowThreadProcessId(window, &owner);
        if (owner != state->process) {
            return TRUE;
        }
        wchar_t name[128] = { 0 };
        wchar_t title[256] = { 0 };
        GetClassNameW(window, name, ARRAYSIZE(name));
        GetWindowTextW(window, title, ARRAYSIZE(title));
        char line[512] = { 0 };
        std::snprintf(line, sizeof(line), " [%p %ls '%ls' vis=%d]", static_cast<void*>(window), name, title,
                      IsWindowVisible(window));
        state->text += line;
        return TRUE;
    }, reinterpret_cast<LPARAM>(&dump));
    return dump.text;
}

struct ChildSearch { const wchar_t* name; HWND window; };

static BOOL CALLBACK FindChildWindow(HWND window, LPARAM param)
{
    ChildSearch* const search = reinterpret_cast<ChildSearch*>(param);
    wchar_t name[128] = { 0 };
    GetClassNameW(window, name, ARRAYSIZE(name));
    if (lstrcmpiW(name, search->name) == 0) {
        search->window = window;
        return FALSE;
    }
    return TRUE;
}

static HWND Child(HWND parent, const wchar_t* name)
{
    ChildSearch search{ name, nullptr };
    EnumChildWindows(parent, FindChildWindow, reinterpret_cast<LPARAM>(&search));
    return search.window;
}

static HWND ChildById(HWND parent, int id)
{
    return GetDlgItem(parent, id);
}

static bool IsClass(HWND window, const wchar_t* name)
{
    wchar_t actual[128] = { 0 };
    GetClassNameW(window, actual, ARRAYSIZE(actual));
    return lstrcmpiW(actual, name) == 0;
}

template <class F> static bool WaitFor(F condition, DWORD milliseconds)
{
    const ULONGLONG end = GetTickCount64() + milliseconds;
    while (GetTickCount64() < end) {
        if (condition()) {
            return true;
        }
        Sleep(50);
    }
    return condition();
}

// Only pointer-free Scintilla messages are safe across processes: a caller
// buffer in this process is not valid inside the editor process.
static long EditorLength(HWND edit)
{
    return static_cast<long>(SendMessageW(edit, SCI_GETLENGTH, 0, 0));
}

static std::string ReadFile(const std::wstring& path)
{
    HANDLE const file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("open fixture");
    }
    std::string data;
    char buffer[4096];
    DWORD read = 0;
    while (ReadFile(file, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
        data.append(buffer, read);
    }
    CloseHandle(file);
    return data;
}

// A previously abandoned editor forwards the command line to its own instance,
// so a leftover process would make every later run observe the wrong window.
static unsigned TerminateEditors(const std::wstring& exe)
{
    unsigned stopped = 0;
    HANDLE const snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (lstrcmpiW(entry.szExeFile, L"Notepad3.exe") != 0) {
                continue;
            }
            HANDLE const process = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                               entry.th32ProcessID);
            if (!process) {
                continue;
            }
            wchar_t path[1024] = { 0 };
            DWORD size = ARRAYSIZE(path);
            if (QueryFullProcessImageNameW(process, 0, path, &size) && lstrcmpiW(path, exe.c_str()) == 0) {
                TerminateProcess(process, 0);
                WaitForSingleObject(process, 5000);
                ++stopped;
            }
            CloseHandle(process);
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    if (stopped) {
        Sleep(500);
    }
    return stopped;
}

struct App {
    PROCESS_INFORMATION process{};
    HWND window = nullptr;
    HWND edit = nullptr;

    App(const std::wstring& exe, const std::wstring& fixture)
    {
        try {
            std::wstring command = L"\"" + exe + L"\" \"" + fixture + L"\"";
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            if (!CreateProcessW(nullptr, &command[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                                &startup, &process)) {
                throw std::runtime_error("start editor");
            }
            if (!WaitFor([this] { window = MainWindow(process.dwProcessId); return window != nullptr; }, 20000)) {
                throw std::runtime_error("main window");
            }
            HWND const frame = ChildById(window, IDC_EDITFRAME);
            edit = ChildById(window, IDC_EDIT);
            if (!frame || !edit || !IsClass(edit, L"Scintilla")) {
                char info[256] = { 0 };
                wchar_t frameClass[128] = L"?", editClass[128] = L"?";
                if (frame) { GetClassNameW(frame, frameClass, ARRAYSIZE(frameClass)); }
                if (edit) { GetClassNameW(edit, editClass, ARRAYSIZE(editClass)); }
                std::snprintf(info, sizeof(info), "editor controls (frame=%p %ls, edit=%p %ls)",
                              static_cast<void*>(frame), frameClass, static_cast<void*>(edit), editClass);
                throw std::runtime_error(info);
            }
            if (!WaitFor([this] { return EditorLength(edit) > 0; }, 10000)) {
                char info[512] = { 0 };
                wchar_t title[256] = { 0 };
                GetWindowTextW(window, title, ARRAYSIZE(title));
                std::snprintf(info, sizeof(info), "document load (title=%ls, length=%ld)%s", title,
                              EditorLength(edit), Describe(process.dwProcessId).c_str());
                throw std::runtime_error(info);
            }
        } catch (...) {
            Stop(true);
            throw;
        }
    }

    void Stop(bool force)
    {
        if (!process.hProcess) {
            return;
        }
        if (!force && window) {
            PostMessageW(window, WM_CLOSE, 0, 0);
        }
        if (force || WaitForSingleObject(process.hProcess, 5000) != WAIT_OBJECT_0) {
            TerminateProcess(process.hProcess, 0);
            WaitForSingleObject(process.hProcess, 5000);
        }
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        process = PROCESS_INFORMATION{};
    }

    ~App()
    {
        Stop(false);
    }

    HWND Frame() const { return ChildById(window, IDC_EDITFRAME); }
    long ToolbarButtons() const
    {
        HWND const toolbar = Child(window, L"ToolbarWindow32");
        return toolbar ? static_cast<long>(SendMessageW(toolbar, TB_BUTTONCOUNT, 0, 0)) : -1;
    }
    HWND Preview() const { return Child(window, PreviewWindowClass); }
    void Toggle() const
    {
        SendMessageW(window, WM_COMMAND, MAKEWPARAM(IDM_VIEW_MARKDOWN_PREVIEW, 0), 0);
    }
};

int wmain(int argc, wchar_t** argv)
{
    unsigned checks = 0;
    auto check = [&](bool okay, const char* name) {
        ++checks;
        if (!okay) {
            throw std::runtime_error(name);
        }
    };

    try {
        if (argc < 3) {
            throw std::runtime_error("usage: TestMarkdownHost.exe <Notepad3.exe> <fixture.md>");
        }
        const std::wstring exe = argv[1];
        const std::wstring fixture = argv[2];
        TerminateEditors(exe);

        // The optional component must sit beside the editor.
        std::wstring directory = exe.substr(0, exe.find_last_of(L"\\/") + 1);
        check(GetFileAttributesW((directory + PreviewDllName).c_str()) != INVALID_FILE_ATTRIBUTES,
              "preview DLL packaged beside the editor");

        const std::string source = ReadFile(fixture);

        {
            App app(exe, fixture);
            std::fprintf(stderr, "host: editor started\n");
            check(app.Preview() == nullptr, "no renderer before the first preview request");

            HWND const edit = app.edit;
            check(EditorLength(edit) == static_cast<long>(source.size()), "editor holds the Markdown source");
            SendMessageW(edit, SCI_GOTOPOS, 5, 0);
            const long caretBefore = static_cast<long>(SendMessageW(edit, SCI_GETCURRENTPOS, 0, 0));

            std::fprintf(stderr, "host: toggling to preview\n");
            app.Toggle();
            check(WaitFor([&app] { HWND const preview = app.Preview(); return preview && IsWindowVisible(preview); }, 45000),
                  "preview becomes visible");
            check(WaitFor([&app] { return !IsWindowVisible(app.Frame()); }, 5000),
                  "source view hidden while previewing");

            HWND const preview = app.Preview();
            check(preview != nullptr, "preview window exists");
            check(EditorLength(edit) == static_cast<long>(source.size()),
                  "preview does not change the source buffer");

            std::fprintf(stderr, "host: toggling back to source\n");
            app.Toggle();
            check(WaitFor([&app] { return app.Preview() == nullptr && IsWindowVisible(app.Frame()); }, 15000),
                  "toggle returns to source");
            check(static_cast<long>(SendMessageW(edit, SCI_GETCURRENTPOS, 0, 0)) == caretBefore,
                  "source caret restored");
            check(EditorLength(edit) == static_cast<long>(source.size()), "source buffer preserved");
        }

        // Non-Markdown buffers are not eligible and must never start a renderer.
        wchar_t tempDirectory[MAX_PATH] = { 0 };
        GetTempPathW(ARRAYSIZE(tempDirectory), tempDirectory);
        const std::wstring plain = std::wstring(tempDirectory) + L"np3-markdown-host.txt";
        {
            HANDLE const file = CreateFileW(plain.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                            FILE_ATTRIBUTE_NORMAL, nullptr);
            check(file != INVALID_HANDLE_VALUE, "create text fixture");
            const char body[] = "plain text\n";
            DWORD written = 0;
            WriteFile(file, body, sizeof(body) - 1, &written, nullptr);
            CloseHandle(file);
        }
        long plainButtons = -1;
        {
            TerminateEditors(exe);
            App app(exe, plain);
            plainButtons = app.ToolbarButtons();
            check(plainButtons > 0, "toolbar is available before the first preview request");
            app.Toggle();
            Sleep(1500);
            check(app.Preview() == nullptr, "no preview for a non-Markdown buffer");
            check(IsWindowVisible(app.Frame()), "source stays visible for a non-Markdown buffer");
            check(app.ToolbarButtons() == plainButtons, "no preview button for a non-Markdown buffer");
        }
        {
            TerminateEditors(exe);
            App app(exe, fixture);
            check(WaitFor([&app, plainButtons] { return app.ToolbarButtons() == plainButtons + 1; }, 10000),
                  "contextual preview button appears for Markdown buffers");
        }
        DeleteFileW(plain.c_str());

        std::printf("PASS: %u host integration assertions\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL after %u host checks: %s\n", checks, error.what());
        return 1;
    }
}
