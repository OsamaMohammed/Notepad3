// Compile against the production parser; no duplicate parser or mapping logic.
#include "MarkdownDocument.h"
#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <string>

static unsigned checks = 0;
static void Check(bool condition, const char* name)
{
    ++checks;
    if (!condition) {
        throw std::runtime_error(name);
    }
}

// Locate rendered text only. Expected source offsets below are hand-counted.
static uint64_t Position(const RenderDocument& doc, const std::string& text,
                         unsigned occurrence = 0, uint32_t boundary = 0)
{
    for (const auto& run : doc.runs) {
        for (size_t at = run.text.find(text); at != std::string::npos;
             at = run.text.find(text, at + text.size())) {
            if (occurrence-- != 0) {
                continue;
            }
            // These fixtures use only ASCII before the requested text.
            // UTF-16 boundaries inside the requested text are explicit literals.
            for (size_t i = 0; i < at; ++i) {
                if (static_cast<unsigned char>(run.text[i]) >= 128) {
                    throw std::runtime_error("Fixture needs an explicit UTF-16 prefix");
                }
            }
            const auto result = ResolveSource(doc, run.id, static_cast<uint32_t>(at) + boundary);
            if (result) {
                return *result;
            }
            throw std::runtime_error("Rendered boundary has no source position");
        }
    }
    throw std::runtime_error("Expected rendered text run missing");
}

int main()
{
    try {
        std::atomic_bool cancelled{false};
        const auto repeated = ParseMarkdown("**same** **same**", cancelled);
        Check(Position(repeated, "same", 0) == 2, "first repeated word");
        Check(Position(repeated, "same", 1) == 11, "second repeated word");
        const auto mixed = ParseMarkdown("same **same**", cancelled);
        Check(Position(mixed, "same", 1) == 7, "formatted word excludes delimiters");

        const auto entity = ParseMarkdown("A &amp; B", cancelled);
        Check(Position(entity, "&") == 2, "entity begins at ampersand");
        Check(Position(entity, "&", 0, 1) == 7, "entity ending boundary");
        const auto emoji = ParseMarkdown("a\xF0\x9F\x98\x80" "b", cancelled);
        Check(Position(emoji, "a\xF0\x9F\x98\x80" "b", 0, 3) == 5,
              "UTF-16 boundary maps to UTF-8 byte boundary");
        Check(Position(emoji, "a\xF0\x9F\x98\x80" "b", 0, 2) == 1,
              "interior surrogate maps to codepoint start");

        const auto crlf = ParseMarkdown("one\r\n\r\n**two**", cancelled);
        const auto lf = ParseMarkdown("one\n\n**two**", cancelled);
        Check(Position(crlf, "two") == 9, "CRLF source offsets preserved");
        Check(Position(lf, "two") == 7, "LF source offsets preserved");
        const auto nested = ParseMarkdown("**a *word* z**", cancelled);
        Check(Position(nested, "word") == 5, "nested formatting source offset");
        const auto reference = ParseMarkdown("[word][id]\n\n[id]: https://example.com", cancelled);
        Check(Position(reference, "word") == 1, "reference link label position");
        const auto escaped = ParseMarkdown("\\*word\\*", cancelled);
        Check(Position(escaped, "word") == 2, "escaped delimiter leaves word offset intact");
        Check(Position(escaped, "*") == 1, "escaped character maps after backslash");
        const auto multiline = ParseMarkdown("`a\nb`", cancelled);
        Check(Position(multiline, "b") == 3, "multiline code span offset");
        Check(Position(multiline, " ") == 2, "normalized code-span newline source offset");
        const auto nul = ParseMarkdown(std::string("a\0b", 3), cancelled);
        Check(Position(nul, "\xEF\xBF\xBD") == 1, "replacement character maps to NUL");

        const auto table = ParseMarkdown("| a | b |\n|---|---|\n| **same** | **same** |", cancelled);
        Check(table.html.find("<table") != std::string::npos, "semantic table");
        Check(Position(table, "same", 0) == 24, "first table cell source offset");
        Check(Position(table, "same", 1) == 35, "second table cell source offset");
        const auto task = ParseMarkdown("- [x] **done**\n- [ ] **todo**", cancelled);
        Check(task.html.find("disabled") != std::string::npos, "read-only task checkbox");
        Check(Position(task, "done") == 8, "task label offset");

        const auto image = ParseMarkdown("before\n\n![alt](image.png)", cancelled);
        Check(image.images.size() == 1, "image captured");
        Check(image.images[0].sourceBlockOffset == 8, "image owning block");
        const auto mermaid = ParseMarkdown("text\n\n```mermaid\ngraph TD; A-->B\n```", cancelled);
        Check(mermaid.diagrams.size() == 1, "Mermaid fence captured");
        Check(mermaid.diagrams[0].sourceBlockOffset == 6, "Mermaid opening fence");
        const auto emptyImage = ParseMarkdown("![](image.png)", cancelled);
        Check(emptyImage.images.size() == 1 && emptyImage.images[0].sourceBlockOffset == 0,
              "empty image alt text retains source fallback");
        const auto emptyFence = ParseMarkdown("```mermaid\n```", cancelled);
        Check(emptyFence.diagrams.size() == 1 && emptyFence.diagrams[0].sourceBlockOffset == 0,
              "empty Mermaid block retains opening fence offset");
        const auto raw = ParseMarkdown("<script>alert(1)</script>", cancelled);
        Check(raw.html.find("<script>") == std::string::npos, "raw HTML is inert");
        Check(raw.html.find("&lt;script&gt;") != std::string::npos, "raw HTML remains visible");
        const auto empty = ParseMarkdown("", cancelled);
        Check(empty.runs.empty(), "empty document");
        Check(!ResolveSource(empty, 0, 0), "unknown run rejected");
        Check(!ResolveSource(emoji, emoji.runs.front().id, 999), "invalid boundary rejected");

        cancelled = true;
        bool stopped = false;
        try {
            ParseMarkdown("must not render", cancelled);
        } catch (const std::runtime_error&) {
            stopped = true;
        }
        Check(stopped, "cancelled parse returns no partial document");
        std::printf("PASS: %u Markdown source-mapping assertions\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL after %u checks: %s\n", checks, error.what());
        return 1;
    }
}
