#include "MarkdownDocument.h"
#include <algorithm>
#include <charconv>
#include <limits>
#include <stdexcept>
extern "C" {
#include "vendor/md4c/md4c.h"
#include "vendor/md4c/entity.h"
}

namespace {
std::string Escape(std::string_view text)
{
    std::string result;
    for (const char ch : text) {
        switch (ch) {
        case '&': result += "&amp;"; break;
        case '<': result += "&lt;"; break;
        case '>': result += "&gt;"; break;
        case '"': result += "&quot;"; break;
        case '\'': result += "&#39;"; break;
        default: result += ch; break;
        }
    }
    return result;
}

void Utf8(std::string& out, uint32_t cp)
{
    if (cp <= 0x7f) { out += static_cast<char>(cp); }
    else if (cp <= 0x7ff) {
        out += static_cast<char>(0xc0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 63));
    } else if (cp <= 0xffff) {
        out += static_cast<char>(0xe0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 63));
        out += static_cast<char>(0x80 | (cp & 63));
    } else {
        out += static_cast<char>(0xf0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 63));
        out += static_cast<char>(0x80 | ((cp >> 6) & 63));
        out += static_cast<char>(0x80 | (cp & 63));
    }
}

uint32_t Decode(std::string_view text, size_t& offset)
{
    const auto first = static_cast<unsigned char>(text[offset++]);
    if (first < 128) { return first; }
    const unsigned count = first >= 0xf0 && first <= 0xf4 ? 3 :
                           first >= 0xe0 && first <= 0xef ? 2 :
                           first >= 0xc2 && first <= 0xdf ? 1 : 0;
    if (!count || text.size() - offset < count) { return 0xfffd; }
    uint32_t cp = first & (0x7f >> count);
    for (unsigned i = 0; i < count; ++i) {
        const auto ch = static_cast<unsigned char>(text[offset + i]);
        if ((ch & 0xc0) != 0x80) { return 0xfffd; }
        cp = (cp << 6) | (ch & 63);
    }
    if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff) ||
        (count == 2 && cp < 0x800) || (count == 3 && cp < 0x10000)) { return 0xfffd; }
    offset += count;
    return cp;
}

std::string Entity(std::string_view text)
{
    if (text.size() > 3 && text[0] == '&' && text[1] == '#') {
        size_t start = 2;
        int base = 10;
        if (text[start] == 'x' || text[start] == 'X') { base = 16; ++start; }
        uint32_t cp = 0;
        const auto parsed = std::from_chars(text.data() + start, text.data() + text.size() - 1, cp, base);
        if (parsed.ec != std::errc{} || !cp || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) { cp = 0xfffd; }
        // HTML's legacy numeric character references in the C1 range.
        static const uint32_t c1[] = {0x20ac,0x81,0x201a,0x192,0x201e,0x2026,0x2020,0x2021,
            0x2c6,0x2030,0x160,0x2039,0x152,0x8d,0x17d,0x8f,0x90,0x2018,0x2019,0x201c,
            0x201d,0x2022,0x2013,0x2014,0x2dc,0x2122,0x161,0x203a,0x153,0x9d,0x17e,0x178};
        if (cp >= 0x80 && cp <= 0x9f) { cp = c1[cp - 0x80]; }
        std::string decoded;
        Utf8(decoded, cp);
        return decoded;
    }
    if (const ENTITY* entry = entity_lookup(text.data(), text.size())) {
        std::string decoded;
        Utf8(decoded, entry->codepoints[0]);
        if (entry->codepoints[1]) { Utf8(decoded, entry->codepoints[1]); }
        return decoded;
    }
    return std::string(text);
}

std::string Attribute(const MD_ATTRIBUTE& attr)
{
    std::string value;
    for (unsigned i = 0; attr.text && attr.substr_offsets[i] < attr.size; ++i) {
        const auto begin = attr.substr_offsets[i];
        const auto end = attr.substr_offsets[i + 1];
        const std::string_view text(attr.text + begin, end - begin);
        if (attr.substr_types[i] == MD_TEXT_ENTITY) { value += Entity(text); }
        else if (attr.substr_types[i] == MD_TEXT_NULLCHAR) { Utf8(value, 0xfffd); }
        else { value.append(text); }
    }
    return value;
}

struct Parser {
    std::string_view source;
    const std::atomic_bool& cancelled;
    RenderDocument doc;
    uint64_t begin = 0, end = 0, blockOffset = 0;
    uint32_t blockId = 0;
    bool diagram = false;
    unsigned imageDepth = 0;
    std::string imageAlt;
    std::exception_ptr error;

    std::string BlockAttribute() const { return " data-block=\"" + std::to_string(blockId) + "\""; }
    void Open(std::string_view tag) { doc.html += '<'; doc.html += tag; doc.html += BlockAttribute() + '>'; }
    void Close(std::string_view tag) { doc.html += "</"; doc.html += tag; doc.html += '>'; }

    int Block(bool entering, MD_BLOCKTYPE type, void* detail)
    {
        switch (type) {
        case MD_BLOCK_DOC: break;
        case MD_BLOCK_CODE:
            if (entering) {
                const auto& code = *static_cast<MD_BLOCK_CODE_DETAIL*>(detail);
                diagram = Attribute(code.lang) == "mermaid";
                if (diagram) {
                    const auto id = static_cast<uint32_t>(doc.diagrams.size());
                    doc.diagrams.push_back({id, {}, blockOffset});
                    doc.html += "<div class=\"mermaid-block\" data-diagram=\"" + std::to_string(id) + "\"" + BlockAttribute() + "></div>";
                } else { Open("pre"); Open("code"); }
            } else {
                if (!diagram) { Close("code"); Close("pre"); }
                diagram = false;
            }
            break;
        case MD_BLOCK_H: {
            const auto tag = "h" + std::to_string(static_cast<MD_BLOCK_H_DETAIL*>(detail)->level);
            if (entering) { Open(tag); } else { Close(tag); }
            break;
        }
        case MD_BLOCK_LI:
            if (entering) {
                Open("li");
                const auto& li = *static_cast<MD_BLOCK_LI_DETAIL*>(detail);
                if (li.is_task) {
                    doc.html += "<input type=\"checkbox\" disabled";
                    if (li.task_mark != ' ') { doc.html += " checked"; }
                    doc.html += "> ";
                }
            } else { Close("li"); }
            break;
        case MD_BLOCK_OL:
            if (entering) { doc.html += "<ol start=\"" + std::to_string(static_cast<MD_BLOCK_OL_DETAIL*>(detail)->start) + "\"" + BlockAttribute() + ">"; }
            else { Close("ol"); }
            break;
        case MD_BLOCK_HR: if (entering) { Open("hr"); } break;
        default: {
            const char* tag = nullptr;
            switch (type) {
            case MD_BLOCK_P: tag = "p"; break;
            case MD_BLOCK_UL: tag = "ul"; break;
            case MD_BLOCK_QUOTE: tag = "blockquote"; break;
            case MD_BLOCK_TABLE: tag = "table"; break;
            case MD_BLOCK_THEAD: tag = "thead"; break;
            case MD_BLOCK_TBODY: tag = "tbody"; break;
            case MD_BLOCK_TR: tag = "tr"; break;
            case MD_BLOCK_TH: tag = "th"; break;
            case MD_BLOCK_TD: tag = "td"; break;
            default: break;
            }
            if (tag) { if (entering) { Open(tag); } else { Close(tag); } }
        }
        }
        return 0;
    }

    int Span(bool entering, MD_SPANTYPE type, void* detail)
    {
        if (type == MD_SPAN_IMG) {
            if (entering) {
                if (imageDepth++ == 0) {
                    const auto& image = *static_cast<MD_SPAN_IMG_DETAIL*>(detail);
                    doc.images.push_back({static_cast<uint32_t>(doc.images.size()), Attribute(image.src), blockOffset});
                    imageAlt.clear();
                }
            } else if (--imageDepth == 0) {
                doc.html += "<img src=\"/image/" + std::to_string(doc.images.back().id) + "\" alt=\"" + Escape(imageAlt) + "\"" + BlockAttribute() + ">";
            }
            return 0;
        }
        if (imageDepth) { return 0; }
        const char* tag = type == MD_SPAN_EM ? "em" : type == MD_SPAN_STRONG ? "strong" :
                          type == MD_SPAN_CODE ? "code" : type == MD_SPAN_DEL ? "del" : nullptr;
        if (type == MD_SPAN_A) {
            if (entering) {
                auto href = Attribute(static_cast<MD_SPAN_A_DETAIL*>(detail)->href);
                // Navigation is separately checked by the preview host.
                if (!(href.compare(0, 8, "https://") == 0 || href.compare(0, 7, "http://") == 0 ||
                      (!href.empty() && href[0] == '#'))) { href.clear(); }
                doc.html += "<a href=\"" + Escape(href) + "\">";
            } else { Close("a"); }
        } else if (tag) { if (entering) { Open(tag); } else { Close(tag); } }
        return 0;
    }

    int Text(MD_TEXTTYPE type, std::string_view input)
    {
        std::string decoded;
        if (type == MD_TEXT_ENTITY) { decoded = Entity(input); }
        else if (type == MD_TEXT_NULLCHAR) { Utf8(decoded, 0xfffd); }
        else if (type == MD_TEXT_BR || type == MD_TEXT_SOFTBR) { decoded = "\n"; }
        else { decoded.assign(input); }
        if (imageDepth) { imageAlt += decoded; return 0; }
        if (diagram) { doc.diagrams.back().code += decoded; return 0; }
        TextRun run{static_cast<uint32_t>(doc.runs.size()), {}, {}, blockOffset};
        const bool transformed = type == MD_TEXT_ENTITY || type == MD_TEXT_NULLCHAR || end - begin != decoded.size();
        size_t pos = 0;
        run.sourceOffsets.push_back(begin);
        while (pos < decoded.size()) {
            const auto start = pos;
            const auto cp = Decode(decoded, pos);
            Utf8(run.text, cp);
            if (cp > 0xffff) { run.sourceOffsets.push_back(transformed ? begin : begin + start); }
            run.sourceOffsets.push_back(transformed ? (pos == decoded.size() ? end : begin) : begin + pos);
        }
        doc.html += "<span data-run=\"" + std::to_string(run.id) + "\">" + Escape(run.text) + "</span>";
        if (type == MD_TEXT_BR) { doc.html += "<br>"; }
        doc.runs.push_back(std::move(run));
        return 0;
    }

    template<class F> int Guard(F&& action)
    {
        if (cancelled.load(std::memory_order_relaxed)) { return 1; }
        try { return action(); }
        catch (...) { error = std::current_exception(); return 1; }
    }
};
}

RenderDocument ParseMarkdown(std::string_view utf8, const std::atomic_bool& cancelled)
{
    if (cancelled.load()) { throw std::runtime_error("Markdown rendering cancelled"); }
    if (utf8.size() > 16 * 1024 * 1024) { throw std::runtime_error("Markdown exceeds the 16 MiB preview limit"); }
    Parser state{utf8, cancelled};
    MD_PARSER parser{};
    parser.flags = MD_FLAG_TABLES | MD_FLAG_TASKLISTS | MD_FLAG_STRIKETHROUGH | MD_FLAG_NOHTML;
    parser.enter_block = [](MD_BLOCKTYPE type, void* detail, void* ctx) {
        auto& p = *static_cast<Parser*>(ctx); return p.Guard([&] { return p.Block(true, type, detail); });
    };
    parser.leave_block = [](MD_BLOCKTYPE type, void* detail, void* ctx) {
        auto& p = *static_cast<Parser*>(ctx); return p.Guard([&] { return p.Block(false, type, detail); });
    };
    parser.enter_span = [](MD_SPANTYPE type, void* detail, void* ctx) {
        auto& p = *static_cast<Parser*>(ctx); return p.Guard([&] { return p.Span(true, type, detail); });
    };
    parser.leave_span = [](MD_SPANTYPE type, void* detail, void* ctx) {
        auto& p = *static_cast<Parser*>(ctx); return p.Guard([&] { return p.Span(false, type, detail); });
    };
    parser.text = [](MD_TEXTTYPE type, const char* text, MD_SIZE size, void* ctx) {
        auto& p = *static_cast<Parser*>(ctx); return p.Guard([&] { return p.Text(type, {text, size}); });
    };
    parser.source_range = [](MD_OFFSET begin, MD_OFFSET end, void* ctx) {
        auto& p = *static_cast<Parser*>(ctx); p.begin = begin; p.end = end;
    };
    parser.source_block = [](MD_OFFSET begin, void* ctx) {
        auto& p = *static_cast<Parser*>(ctx);
        // Include Markdown prefixes in the fallback for the owning source line.
        while (begin && p.source[begin - 1] != '\n' && p.source[begin - 1] != '\r') { --begin; }
        p.blockOffset = begin;
        p.blockId = static_cast<uint32_t>(p.doc.blocks.size());
        p.Guard([&] { p.doc.blocks.push_back(begin); return 0; });
    };
    const int result = md_parse(utf8.data(), static_cast<MD_SIZE>(utf8.size()), &parser, &state);
    if (state.error) { std::rethrow_exception(state.error); }
    if (result || cancelled.load()) { throw std::runtime_error("Markdown rendering cancelled or failed"); }
    return std::move(state.doc);
}

std::optional<uint64_t> ResolveSource(const RenderDocument& doc, uint32_t runId, uint32_t utf16Offset)
{
    if (runId >= doc.runs.size() || utf16Offset >= doc.runs[runId].sourceOffsets.size()) { return std::nullopt; }
    return doc.runs[runId].sourceOffsets[utf16Offset];
}
