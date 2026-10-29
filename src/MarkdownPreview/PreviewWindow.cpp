#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "PreviewApi.h"
#include "MarkdownDocument.h"
#include "PreviewResources.h"
#include <objbase.h>
#include <WebView2.h>
#include <wrl.h>
#include <wrl/wrappers/corewrappers.h>
#include <windows.data.json.h>
#include <roapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <cmath>

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Callback;
using Microsoft::WRL::Wrappers::HString;
using Microsoft::WRL::Wrappers::HStringReference;
using ABI::Windows::Data::Json::IJsonObject;
using ABI::Windows::Data::Json::IJsonObjectStatics;

namespace {
constexpr UINT ParsedMessage = WM_APP + 1;
constexpr UINT MenuMessage = WM_APP + 2;
constexpr wchar_t Origin[] = L"https://np3-preview.invalid/";
constexpr wchar_t Page[] = L"https://np3-preview.invalid/index.html";

void Check(HRESULT result) { if (FAILED(result)) { throw result; } }
std::wstring Take(wchar_t* text)
{
    std::wstring result = text ? text : L"";
    CoTaskMemFree(text);
    return result;
}
std::wstring Wide(std::string_view text)
{
    if (text.empty()) { return {}; }
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!count) { throw E_INVALIDARG; }
    std::wstring result(count, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), count);
    return result;
}
std::string Json(std::string_view text)
{
    std::string out = "\"";
    const char digits[] = "0123456789abcdef";
    for (unsigned char ch : text) {
        if (ch == '"' || ch == '\\') { out += '\\'; out += static_cast<char>(ch); }
        else if (ch < 32) { out += "\\u00"; out += digits[ch >> 4]; out += digits[ch & 15]; }
        else { out += static_cast<char>(ch); }
    }
    return out + '"';
}
HMODULE Module()
{
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&Module), &module);
    return module;
}
std::filesystem::path AssetRoot()
{
    std::wstring path(32768, L'\0');
    const DWORD count = GetModuleFileNameW(Module(), path.data(), static_cast<DWORD>(path.size()));
    if (!count || count == path.size()) { throw E_FAIL; }
    path.resize(count);
    return std::filesystem::path(path).parent_path() / L"MarkdownPreview";
}
std::wstring String(IJsonObject* object, const wchar_t* name)
{
    HString value;
    Check(object->GetNamedString(HStringReference(name).Get(), value.GetAddressOf()));
    UINT32 size = 0;
    const wchar_t* text = value.GetRawBuffer(&size);
    return std::wstring(text, size);
}
uint32_t Number(IJsonObject* object, const wchar_t* name)
{
    double value = 0;
    Check(object->GetNamedNumber(HStringReference(name).Get(), &value));
    if (!std::isfinite(value) || value < 0 || value > UINT32_MAX || std::floor(value) != value) { throw E_INVALIDARG; }
    return static_cast<uint32_t>(value);
}

struct Preview : std::enable_shared_from_this<Preview> {
    HWND window = nullptr;
    RECT bounds{};
    uint64_t generation = 0;
    uintptr_t id = 0;
    NP3PreviewCallback callback = nullptr;
    void* context = nullptr;
    std::atomic_bool cancelled{false};
    bool dark = false, ready = false, sent = false;
    std::string source;
    std::wstring sourcePath, contextLink, copyLabel = L"Copy", openLinkLabel = L"Open Link";
    POINT menuPoint{};
    std::mutex mutex;
    std::unique_ptr<RenderDocument> document;
    HRESULT parseResult = S_OK;
    std::unique_ptr<PreviewResources> resources;
    ComPtr<ICoreWebView2Environment> environment;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> view;

    void Report(uint32_t kind, HRESULT result = S_OK, uint64_t offset = 0, uint32_t command = 0)
    {
        if (cancelled || !callback) { return; }
        const NP3PreviewEvent event{sizeof(event), generation, kind, result, offset, command};
        callback(context, &event);
    }
    template<class F> HRESULT Guard(F action)
    {
        if (cancelled) { return S_OK; }
        try { action(); }
        catch (HRESULT result) { Report(NP3PreviewError, result); }
        catch (...) { Report(NP3PreviewError, E_FAIL); }
        return S_OK;
    }
    void Stop()
    {
        cancelled = true;
        callback = nullptr;
        if (window) { KillTimer(window, 1); }
        if (controller) { controller->Close(); }
        view.Reset(); controller.Reset(); environment.Reset();
        if (window) { HWND old = window; window = nullptr; DestroyWindow(old); }
        resources.reset();
    }
    // A child window starts at the bottom of its siblings' z-order, so the
    // editor control stays above it: the preview would paint but receive no
    // mouse input. Keep it on top whenever it is sized or shown.
    void Raise()
    {
        if (window) { SetWindowPos(window, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); }
    }
    void Size(const RECT& rectangle)
    {
        bounds = rectangle;
        if (window) {
            SetWindowPos(window, nullptr, bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top, SWP_NOZORDER | SWP_NOACTIVATE);
            Raise();
        }
        if (controller) {
            RECT client{0, 0, bounds.right - bounds.left, bounds.bottom - bounds.top};
            Check(controller->put_Bounds(client));
        }
    }
    std::wstring Payload()
    {
        std::string json = "{\"kind\":\"render\",\"generation\":\"" + std::to_string(generation) + "\",\"dark\":" + (dark ? "true" : "false") + ",\"html\":" + Json(document->html) + ",\"diagrams\":[";
        for (const auto& diagram : document->diagrams) {
            if (diagram.id) { json += ','; }
            json += "{\"id\":" + std::to_string(diagram.id) + ",\"code\":" + Json(diagram.code) + '}';
        }
        return Wide(json + "]}");
    }
    void Message(ICoreWebView2WebMessageReceivedEventArgs* args)
    {
        wchar_t* raw = nullptr;
        Check(args->get_Source(&raw));
        const auto uri = Take(raw);
        if (uri != Page && uri.compare(0, wcslen(Page) + 1, std::wstring(Page) + L"#") != 0) { return; }
        Check(args->get_WebMessageAsJson(&raw));
        const auto json = Take(raw);
        if (json.size() > 4096) { return; }
        ComPtr<IJsonObjectStatics> factory;
        Check(RoGetActivationFactory(HStringReference(RuntimeClass_Windows_Data_Json_JsonObject).Get(), IID_PPV_ARGS(&factory)));
        ComPtr<IJsonObject> object;
        Check(factory->Parse(HStringReference(json.c_str()).Get(), &object));
        if (Number(object.Get(), L"version") != 1) { return; }
        const auto kind = String(object.Get(), L"kind");
        if (kind == L"loaded" && !sent) {
            sent = true;
            Check(view->PostWebMessageAsJson(Payload().c_str()));
            return;
        }
        if (String(object.Get(), L"generation") != std::to_wstring(generation)) { return; }
        if (kind == L"ready") {
            KillTimer(window, 1);
            if (!ready) { ready = true; Raise(); ShowWindow(window, SW_SHOWNA); Report(NP3PreviewReady); }
        } else if (kind == L"source" && ready) {
            uint64_t offset;
            ComPtr<ABI::Windows::Data::Json::IJsonValue> runValue;
            const bool hasRun = SUCCEEDED(object->GetNamedValue(HStringReference(L"runId").Get(), &runValue));
            if (hasRun) {
                const auto position = ResolveSource(*document, Number(object.Get(), L"runId"), Number(object.Get(), L"utf16Offset"));
                if (!position) { return; }
                offset = *position;
            } else {
                const auto block = Number(object.Get(), L"blockId");
                if (block >= document->blocks.size()) { return; }
                offset = document->blocks[block];
            }
            if (offset <= source.size()) { Report(NP3PreviewNavigateSource, S_OK, offset); }
        } else if (kind == L"error") { Report(NP3PreviewError, E_FAIL); }
    }
    void Resource(ICoreWebView2WebResourceRequestedEventArgs* args)
    {
        ComPtr<ICoreWebView2WebResourceRequest> request;
        Check(args->get_Request(&request));
        wchar_t* raw = nullptr;
        Check(request->get_Uri(&raw));
        const auto uri = Take(raw);
        COREWEBVIEW2_WEB_RESOURCE_CONTEXT type;
        Check(args->get_ResourceContext(&type));
        if (type == COREWEBVIEW2_WEB_RESOURCE_CONTEXT_IMAGE && uri.compare(0, wcslen(Origin), Origin) != 0 && AllowRemoteImage(uri)) { return; }
        const auto kind = type == COREWEBVIEW2_WEB_RESOURCE_CONTEXT_DOCUMENT ? ResourceKind::Document :
                          type == COREWEBVIEW2_WEB_RESOURCE_CONTEXT_SCRIPT ? ResourceKind::Script :
                          type == COREWEBVIEW2_WEB_RESOURCE_CONTEXT_STYLESHEET ? ResourceKind::Style :
                          type == COREWEBVIEW2_WEB_RESOURCE_CONTEXT_IMAGE ? ResourceKind::Image : ResourceKind::Other;
        const auto reply = resources->Resolve(uri, kind);
        ComPtr<IStream> stream;
        stream.Attach(SHCreateMemStream(reinterpret_cast<const BYTE*>(reply.body.data()), static_cast<UINT>(reply.body.size())));
        std::wstring headers = L"Content-Type: " + Wide(reply.mime) + L"\r\nX-Content-Type-Options: nosniff\r\nCache-Control: no-store\r\n";
        if (!reply.redirect.empty()) { headers += L"Location: " + reply.redirect + L"\r\n"; }
        ComPtr<ICoreWebView2WebResourceResponse> response;
        Check(environment->CreateWebResourceResponse(stream.Get(), reply.status, reply.status == 200 ? L"OK" : reply.status == 302 ? L"Found" : L"Not Found", headers.c_str(), &response));
        Check(args->put_Response(response.Get()));
    }
    void Copy()
    {
        if (!view) { return; }
        auto self = shared_from_this();
        Check(view->ExecuteScript(L"window.getSelection().toString()", Callback<ICoreWebView2ExecuteScriptCompletedHandler>([self](HRESULT result, LPCWSTR json) -> HRESULT {
            return self->Guard([&] {
                Check(result);
                ComPtr<ABI::Windows::Data::Json::IJsonValueStatics> factory;
                Check(RoGetActivationFactory(HStringReference(RuntimeClass_Windows_Data_Json_JsonValue).Get(), IID_PPV_ARGS(&factory)));
                ComPtr<ABI::Windows::Data::Json::IJsonValue> value;
                Check(factory->Parse(HStringReference(json).Get(), &value));
                HString text; Check(value->GetString(text.GetAddressOf()));
                UINT32 length = 0; const wchar_t* chars = text.GetRawBuffer(&length);
                HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (static_cast<size_t>(length) + 1) * sizeof(wchar_t));
                if (!memory) { throw E_OUTOFMEMORY; }
                void* buffer = GlobalLock(memory);
                if (!buffer) { GlobalFree(memory); throw E_OUTOFMEMORY; }
                memcpy(buffer, chars, length * sizeof(wchar_t));
                static_cast<wchar_t*>(buffer)[length] = 0;
                GlobalUnlock(memory);
                if (OpenClipboard(self->window)) {
                    EmptyClipboard();
                    if (SetClipboardData(CF_UNICODETEXT, memory)) { memory = nullptr; }
                    CloseClipboard();
                }
                if (memory) { GlobalFree(memory); }
            });
        }).Get()));
    }
    void Configure()
    {
        Check(controller->get_CoreWebView2(&view));
        ComPtr<ICoreWebView2Settings> settings;
        Check(view->get_Settings(&settings));
        settings->put_AreDevToolsEnabled(FALSE);
        settings->put_AreDefaultScriptDialogsEnabled(FALSE);
        settings->put_AreHostObjectsAllowed(FALSE);
        settings->put_AreDefaultContextMenusEnabled(FALSE);
        ComPtr<ICoreWebView2Settings3> settings3;
        if (SUCCEEDED(settings.As(&settings3))) { settings3->put_AreBrowserAcceleratorKeysEnabled(FALSE); }
        auto self = shared_from_this();
        EventRegistrationToken token{};
        Check(view->AddWebResourceRequestedFilter(L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL));
        Check(view->add_WebResourceRequested(Callback<ICoreWebView2WebResourceRequestedEventHandler>([self](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) { return self->Guard([&] { self->Resource(args); }); }).Get(), &token));
        Check(view->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>([self](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
            // Malformed/unrecognized messages cannot change preview lifecycle.
            try { if (!self->cancelled) { self->Message(args); } } catch (...) {}
            return S_OK;
        }).Get(), &token));
        Check(view->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>([](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
            wchar_t* raw = nullptr; if (FAILED(args->get_Uri(&raw))) { return E_FAIL; }
            const auto uri = Take(raw);
            if (uri != Page && uri.compare(0, wcslen(Page) + 1, std::wstring(Page) + L"#") != 0) { args->put_Cancel(TRUE); }
            return S_OK;
        }).Get(), &token));
        Check(view->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>([](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT { return args->put_Handled(TRUE); }).Get(), &token));
        Check(view->add_PermissionRequested(Callback<ICoreWebView2PermissionRequestedEventHandler>([](ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs* args) -> HRESULT { return args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY); }).Get(), &token));
        Check(view->add_ProcessFailed(Callback<ICoreWebView2ProcessFailedEventHandler>([self](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs*) -> HRESULT { self->Report(NP3PreviewError, E_FAIL); return S_OK; }).Get(), &token));
        Check(controller->add_AcceleratorKeyPressed(Callback<ICoreWebView2AcceleratorKeyPressedEventHandler>([self](ICoreWebView2Controller*, ICoreWebView2AcceleratorKeyPressedEventArgs* args) -> HRESULT {
            COREWEBVIEW2_KEY_EVENT_KIND kind; UINT key; COREWEBVIEW2_PHYSICAL_KEY_STATUS status{};
            args->get_KeyEventKind(&kind); args->get_VirtualKey(&key); args->get_PhysicalKeyStatus(&status);
            if (kind != COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN && kind != COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN) { return S_OK; }
            if ((GetKeyState(VK_CONTROL) & 0x8000) && (GetKeyState(VK_MENU) & 0x8000) && !(GetKeyState(VK_RMENU) & 0x8000) && key == 'M') {
                args->put_Handled(TRUE);
                if (!status.WasKeyDown) { self->Report(NP3PreviewToggleSource); }
            }
            return S_OK;
        }).Get(), &token));
        ComPtr<ICoreWebView2_4> view4;
        if (SUCCEEDED(view.As(&view4))) {
            Check(view4->add_DownloadStarting(Callback<ICoreWebView2DownloadStartingEventHandler>([](ICoreWebView2*, ICoreWebView2DownloadStartingEventArgs* args) -> HRESULT { return args->put_Cancel(TRUE); }).Get(), &token));
        }
        ComPtr<ICoreWebView2_11> view11;
        if (SUCCEEDED(view.As(&view11))) {
            Check(view11->add_ContextMenuRequested(Callback<ICoreWebView2ContextMenuRequestedEventHandler>([self](ICoreWebView2*, ICoreWebView2ContextMenuRequestedEventArgs* args) -> HRESULT {
                return self->Guard([&] {
                    args->put_Handled(TRUE);
                    ComPtr<ICoreWebView2ContextMenuTarget> target;
                    Check(args->get_ContextMenuTarget(&target));
                    BOOL hasLink = FALSE; target->get_HasLinkUri(&hasLink);
                    self->contextLink.clear();
                    if (hasLink) { wchar_t* raw = nullptr; Check(target->get_LinkUri(&raw)); self->contextLink = Take(raw); }
                    Check(args->get_Location(&self->menuPoint));
                    ClientToScreen(self->window, &self->menuPoint);
                    PostMessageW(self->window, MenuMessage, self->id, 0);
                });
            }).Get(), &token));
        }
        Size(bounds);
        Check(controller->put_IsVisible(TRUE));
        Check(view->Navigate(Page));
    }
    void StartBrowser()
    {
        { std::lock_guard<std::mutex> lock(mutex); Check(parseResult); }
        resources = std::make_unique<PreviewResources>(*document, sourcePath, AssetRoot());
        PWSTR local = nullptr;
        Check(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &local));
        const auto folder = std::filesystem::path(Take(local)) / L"Rizonesoft/Notepad3/MarkdownPreview";
        std::filesystem::create_directories(folder);
        auto self = shared_from_this();
        Check(CreateCoreWebView2EnvironmentWithOptions(nullptr, folder.c_str(), nullptr, Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([self](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
            return self->Guard([&] {
                Check(result); self->environment = environment;
                Check(environment->CreateCoreWebView2Controller(self->window, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>([self](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                    if (self->cancelled) { if (controller) { controller->Close(); } return S_OK; }
                    return self->Guard([&] { Check(result); self->controller = controller; self->Configure(); });
                }).Get()));
            });
        }).Get()));
    }
};

struct Runtime {
    uintptr_t next = 0;
    std::unordered_map<uintptr_t, std::shared_ptr<Preview>> instances;
};
Runtime& Instances() { static Runtime runtime; return runtime; }
std::shared_ptr<Preview> Find(NP3PreviewHandle handle)
{
    const auto it = Instances().instances.find(reinterpret_cast<uintptr_t>(handle));
    return it == Instances().instances.end() ? nullptr : it->second;
}
LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_NCCREATE) { SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams)); }
    auto state = reinterpret_cast<Preview*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (state) {
        if (message == ParsedMessage && wparam == state->id) { state->Guard([&] { state->StartBrowser(); }); return 0; }
        if (message == MenuMessage && wparam == state->id) {
            auto hold = state->shared_from_this();
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, 1, state->copyLabel.c_str());
            if (AllowRemoteImage(state->contextLink) || state->contextLink.compare(0, 7, L"http://") == 0) { AppendMenuW(menu, MF_STRING, 2, state->openLinkLabel.c_str()); }
            const auto selected = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, state->menuPoint.x, state->menuPoint.y, 0, window, nullptr);
            DestroyMenu(menu);
            if (selected == 1) { state->Guard([&] { state->Copy(); }); }
            if (selected == 2) { ShellExecuteW(window, L"open", state->contextLink.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
            return 0;
        }
        if (message == WM_TIMER && wparam == 1) { KillTimer(window, 1); state->Report(NP3PreviewError, HRESULT_FROM_WIN32(ERROR_TIMEOUT)); return 0; }
        if (message == WM_NCDESTROY) { state->cancelled = true; state->callback = nullptr; state->window = nullptr; SetWindowLongPtrW(window, GWLP_USERDATA, 0); }
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
HRESULT __cdecl Begin(const NP3PreviewRequest* request, NP3PreviewHandle* handle)
{
    if (!handle) { return E_POINTER; }
    *handle = nullptr;
    if (!request || request->cbSize != sizeof(*request) || !IsWindow(request->parent) || !request->callback ||
        !request->sourcePath || (!request->utf8 && request->byteCount) || request->byteCount > 16 * 1024 * 1024) { return E_INVALIDARG; }
    try {
        auto state = std::make_shared<Preview>();
        state->generation = request->generation; state->callback = request->callback; state->context = request->context;
        state->source.assign(request->utf8 ? request->utf8 : "", static_cast<size_t>(request->byteCount));
        state->sourcePath = request->sourcePath; state->dark = !!request->dark; state->bounds = request->bounds;
        state->id = ++Instances().next;
        if (request->copyLabel) { state->copyLabel = request->copyLabel; }
        if (request->openLinkLabel) { state->openLinkLabel = request->openLinkLabel; }
        WNDCLASSW cls{}; cls.hInstance = Module(); cls.lpfnWndProc = WindowProc; cls.lpszClassName = L"NP3MarkdownPreviewChild";
        if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) { return HRESULT_FROM_WIN32(GetLastError()); }
        state->window = CreateWindowW(cls.lpszClassName, L"", WS_CHILD | WS_CLIPCHILDREN, request->bounds.left, request->bounds.top,
            request->bounds.right - request->bounds.left, request->bounds.bottom - request->bounds.top, request->parent, nullptr, cls.hInstance, state.get());
        if (!state->window) { return HRESULT_FROM_WIN32(GetLastError()); }
        const auto id = state->id;
        Instances().instances.emplace(id, state);
        *handle = reinterpret_cast<NP3PreviewHandle>(id);
        SetTimer(state->window, 1, 30000, nullptr);
        const HWND target = state->window;
        std::thread([state, target] {
            try {
                auto document = std::make_unique<RenderDocument>(ParseMarkdown(state->source, state->cancelled));
                std::lock_guard<std::mutex> lock(state->mutex); state->document = std::move(document);
            } catch (...) { std::lock_guard<std::mutex> lock(state->mutex); state->parseResult = E_FAIL; }
            if (!state->cancelled) { PostMessageW(target, ParsedMessage, state->id, 0); }
        }).detach();
        return S_OK;
    } catch (...) { return E_OUTOFMEMORY; }
}
HRESULT __cdecl Resize(NP3PreviewHandle handle, const RECT* bounds)
{
    auto state = Find(handle); if (!state || !bounds) { return E_INVALIDARG; }
    return state->Guard([&] { state->Size(*bounds); });
}
HRESULT __cdecl SetDark(NP3PreviewHandle handle, BOOL dark)
{
    auto state = Find(handle); if (!state) { return E_INVALIDARG; }
    return state->Guard([&] { state->dark = !!dark; if (state->ready) { Check(state->view->PostWebMessageAsJson(dark ? L"{\"kind\":\"theme\",\"dark\":true}" : L"{\"kind\":\"theme\",\"dark\":false}")); } });
}
HRESULT __cdecl Copy(NP3PreviewHandle handle)
{
    auto state = Find(handle); if (!state) { return E_INVALIDARG; }
    return state->Guard([&] { state->Copy(); });
}
HRESULT __cdecl Focus(NP3PreviewHandle handle)
{
    auto state = Find(handle); if (!state || !state->controller) { return E_INVALIDARG; }
    return state->controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
}
void __cdecl Close(NP3PreviewHandle handle)
{
    auto state = Find(handle); if (!state) { return; }
    state->Stop(); Instances().instances.erase(reinterpret_cast<uintptr_t>(handle));
}
}

HRESULT __cdecl NP3Preview_GetApi(uint32_t version, uint32_t apiSize, NP3PreviewApi* api)
{
    if (version != NP3_PREVIEW_ABI || apiSize != sizeof(*api) || !api) { return E_INVALIDARG; }
    *api = {sizeof(*api), Begin, Resize, SetDark, Copy, Focus, Close};
    return S_OK;
}
