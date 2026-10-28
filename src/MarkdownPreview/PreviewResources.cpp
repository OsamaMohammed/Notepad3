#include "PreviewResources.h"
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#include <shlwapi.h>
#include <algorithm>
#include <cwctype>
#include <fstream>
#include <charconv>

namespace {
std::wstring Wide(std::string_view utf8)
{
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (!size) { return {}; }
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), result.data(), size);
    return result;
}

std::wstring Lower(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
    return value;
}

bool Forbidden(std::wstring_view text)
{
    return std::any_of(text.begin(), text.end(), [](wchar_t ch) { return ch < 32 || ch == 127; });
}

ResourceReply Read(const std::filesystem::path& path, const std::string& mime)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > 32 * 1024 * 1024) { return {}; }
    std::ifstream input(path, std::ios::binary);
    if (!input) { return {}; }
    ResourceReply reply{200, mime, std::string(static_cast<size_t>(size), '\0'), {}};
    if (size && !input.read(reply.body.data(), static_cast<std::streamsize>(size))) { return {}; }
    return reply;
}

std::string ImageMime(const std::filesystem::path& path)
{
    const auto ext = Lower(path.extension().wstring());
    if (ext == L".png") { return "image/png"; }
    if (ext == L".jpg" || ext == L".jpeg") { return "image/jpeg"; }
    if (ext == L".gif") { return "image/gif"; }
    if (ext == L".svg") { return "image/svg+xml"; }
    if (ext == L".webp") { return "image/webp"; }
    if (ext == L".bmp") { return "image/bmp"; }
    if (ext == L".ico") { return "image/x-icon"; }
    if (ext == L".avif") { return "image/avif"; }
    return {};
}
}

bool AllowRemoteImage(std::wstring_view uri)
{
    if (uri.empty() || uri.size() > 32768 || Forbidden(uri) || uri.find(L'\\') != uri.npos) { return false; }
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = parts.dwUserNameLength = parts.dwPasswordLength = static_cast<DWORD>(-1);
    const std::wstring terminated(uri);
    return WinHttpCrackUrl(terminated.c_str(), static_cast<DWORD>(uri.size()), 0, &parts) &&
           parts.nScheme == INTERNET_SCHEME_HTTPS && parts.dwHostNameLength &&
           !parts.dwUserNameLength && !parts.dwPasswordLength;
}

PreviewResources::PreviewResources(const RenderDocument& document, std::wstring_view sourcePath,
                                   const std::filesystem::path& assetRoot) : assets(assetRoot)
{
    const auto parent = std::filesystem::path(sourcePath).parent_path();
    for (const auto& image : document.images) {
        auto value = Wide(image.destination);
        if (AllowRemoteImage(value)) { images.push_back(value); continue; }
        if (value.empty() || Forbidden(value) || value.find(L':') != value.npos ||
            value.front() == L'/' || value.front() == L'\\') { images.emplace_back(); continue; }
        DWORD size = static_cast<DWORD>(value.size() + 1);
        std::wstring decoded(size, L'\0');
        if (FAILED(UrlUnescapeW(value.data(), decoded.data(), &size, 0))) { images.emplace_back(); continue; }
        decoded.resize(size);
        if (decoded.empty() || Forbidden(decoded) || decoded.find(L':') != decoded.npos ||
            decoded.front() == L'/' || decoded.front() == L'\\') { images.emplace_back(); continue; }
        std::error_code error;
        const auto path = std::filesystem::weakly_canonical(parent / decoded, error);
        const auto native = path.wstring();
        // Never resolve a local reference through a junction to a UNC/device path.
        if (error || native.compare(0, 2, L"\\\\") == 0 || ImageMime(path).empty()) { images.emplace_back(); }
        else { images.push_back(native); }
    }
}

ResourceReply PreviewResources::Resolve(std::wstring_view uri, ResourceKind kind) const
{
    constexpr std::wstring_view origin = L"https://np3-preview.invalid/";
    if (uri.substr(0, origin.size()) != origin) { return {}; }
    const auto path = uri.substr(origin.size());
    if (path.substr(0, 6) == L"image/") {
        if (kind != ResourceKind::Image) { return {}; }
        const auto idText = path.substr(6);
        if (idText.empty() || idText.size() > 9 || !std::all_of(idText.begin(), idText.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; })) { return {}; }
        const auto id = std::stoul(std::wstring(idText));
        if (id >= images.size() || images[id].empty()) { return {}; }
        if (AllowRemoteImage(images[id])) { return {302, "text/plain", {}, images[id]}; }
        return Read(images[id], ImageMime(images[id]));
    }
    if (path.empty() || path.find(L'\\') != path.npos || path.find(L'%') != path.npos || path.find(L':') != path.npos || Forbidden(path)) { return {}; }
    const std::filesystem::path relative(path);
    if (relative.is_absolute()) { return {}; }
    for (const auto& part : relative) { if (part == L".." || part == L".") { return {}; } }
    if (path == L"index.html" && kind == ResourceKind::Document) { return Read(assets / relative, "text/html; charset=utf-8"); }
    if (path == L"preview.css" && kind == ResourceKind::Style) { return Read(assets / relative, "text/css"); }
    if (kind == ResourceKind::Script && (path == L"preview.js" ||
        (path.substr(0, 15) == L"vendor/mermaid/" && relative.extension() == L".mjs"))) {
        return Read(assets / relative, "text/javascript");
    }
    return {};
}
