#include "PreviewResources.h"
#include <fstream>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv)
{
    unsigned checks = 0;
    auto check = [&](bool condition, const char* name) { ++checks; if (!condition) { throw std::runtime_error(name); } };
    try {
        if (argc != 2) { throw std::runtime_error("Fixture directory argument required"); }
        const auto root = std::filesystem::absolute(argv[1]);
        std::filesystem::create_directories(root / "assets");
        std::filesystem::create_directories(root / "docs");
        std::ofstream(root / "assets/index.html") << "<!doctype html><title>Preview</title>";
        std::ofstream(root / "assets/preview.js") << "// packaged script";
        std::ofstream(root / "docs/photo #1.png", std::ios::binary) << "image fixture";
        std::ofstream(root / "outside.png", std::ios::binary) << "explicit parent image";
        std::ofstream(root / "docs/shape.svg") << "<svg xmlns=\"http://www.w3.org/2000/svg\"/>";
        std::ofstream(root / std::filesystem::path(L"docs/\u0645\u062b\u0627\u0644.png")) << "Unicode image";
        RenderDocument doc;
        doc.images = {{0,"photo%20%231.png",0}, {1,"../outside.png",0}, {2,"shape.svg",0},
                      {3,"https://example.com/pic.png",0}, {4,"http://example.com/pic.png",0},
                      {5,"//server/share/pic.png",0}, {6,"file:///C:/Windows/win.ini",0},
                      {7,"\xD9\x85\xD8\xAB\xD8\xA7\xD9\x84.png",0}};
        PreviewResources resources(doc, (root / "docs/test.md").wstring(), root / "assets");
        const std::wstring base = L"https://np3-preview.invalid";
        check(resources.Resolve(base + L"/index.html", ResourceKind::Document).status == 200, "known document asset");
        check(resources.Resolve(base + L"/preview.js", ResourceKind::Script).mime == "text/javascript", "script MIME");
        check(resources.Resolve(base + L"/image/0", ResourceKind::Image).body == "image fixture", "escaped local filename");
        check(resources.Resolve(base + L"/image/1", ResourceKind::Image).body == "explicit parent image", "explicit parent reference");
        check(resources.Resolve(base + L"/image/2", ResourceKind::Image).mime == "image/svg+xml", "SVG as image");
        check(resources.Resolve(base + L"/image/2", ResourceKind::Document).status == 404, "SVG document denied");
        check(resources.Resolve(base + L"/image/3", ResourceKind::Image).redirect == L"https://example.com/pic.png", "HTTPS image redirect");
        check(resources.Resolve(base + L"/image/7", ResourceKind::Image).body == "Unicode image", "Unicode local filename");
        for (const auto id : {4,5,6,999}) {
            check(resources.Resolve(base + L"/image/" + std::to_wstring(id), ResourceKind::Image).status == 404, "unauthorized image denied");
        }
        check(resources.Resolve(base + L"/%2e%2e/outside.png", ResourceKind::Image).status == 404, "encoded resource traversal denied");
        check(resources.Resolve(base + L"/../outside.png", ResourceKind::Image).status == 404, "resource traversal denied");
        check(resources.Resolve(base + L"/missing.js", ResourceKind::Script).status == 404, "unknown script denied");
        check(resources.Resolve(L"file:///C:/Windows/win.ini", ResourceKind::Document).status == 404, "file navigation denied");
        check(resources.Resolve(base + L"/preview.js", ResourceKind::Image).status == 404, "script as image denied");
        check(AllowRemoteImage(L"HTTPS://example.com/a.png"), "HTTPS scheme is case insensitive");
        check(!AllowRemoteImage(L"https://user:password@example.com/a.png"), "credentialed URL denied");
        check(!AllowRemoteImage(L"https://example.com/a\r\nInjected: header"), "URL header injection denied");
        std::cout << "PASS: " << checks << " preview resource assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " resource checks: " << error.what() << '\n';
        return 1;
    }
}
