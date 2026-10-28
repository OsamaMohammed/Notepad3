#pragma once
#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct TextRun {
    uint32_t id;
    std::string text;
    std::vector<uint64_t> sourceOffsets;
    uint64_t sourceBlockOffset;
};
struct ImageRef {
    uint32_t id;
    std::string destination;
    uint64_t sourceBlockOffset;
};
struct MermaidBlock {
    uint32_t id;
    std::string code;
    uint64_t sourceBlockOffset;
};
struct RenderDocument {
    std::string html;
    std::vector<TextRun> runs;
    std::vector<ImageRef> images;
    std::vector<MermaidBlock> diagrams;
    std::vector<uint64_t> blocks;
};

RenderDocument ParseMarkdown(std::string_view utf8, const std::atomic_bool& cancelled);
std::optional<uint64_t> ResolveSource(const RenderDocument& doc, uint32_t runId, uint32_t utf16Offset);
