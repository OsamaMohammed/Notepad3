#pragma once
#include "MarkdownDocument.h"
#include <filesystem>

enum class ResourceKind { Document, Script, Style, Image, Other };
struct ResourceReply {
    int status = 404;
    std::string mime = "text/plain";
    std::string body;
    std::wstring redirect;
};
bool AllowRemoteImage(std::wstring_view uri);
class PreviewResources {
public:
    PreviewResources(const RenderDocument& document, std::wstring_view sourcePath,
                     const std::filesystem::path& assetRoot);
    ResourceReply Resolve(std::wstring_view uri, ResourceKind kind) const;
private:
    std::filesystem::path assets;
    std::vector<std::wstring> images;
};
