# Bundled MD4C

MD4C 0.6.0, commit `7fc1815a5eeba2af7d6120a76202bf59f3b6e6e4`.
Upstream: https://github.com/mity/md4c. MIT license is preserved at the top of each source file.

`np3_patches/md4c-source-ranges.patch` adds two optional callbacks to the private parser structure: `source_range` reports text input boundaries (including synthesized text), and `source_block` reports leaf-block starts. This copy is statically compiled into the preview DLL; its extended ABI must never be used with an upstream binary.

To update: download the four sources listed in `../../dependencies.json`, verify their upstream SHA-256 values, then apply the patch with `git apply -p2 --directory=src/MarkdownPreview/vendor/md4c np3_patches/md4c-source-ranges.patch` from the repository root. Run `test/MarkdownPreview/TestMarkdownPreview.ps1 -Suite Core` after every parser update. Changes to inline whitespace handling or callback input pointer provenance require particular care.
