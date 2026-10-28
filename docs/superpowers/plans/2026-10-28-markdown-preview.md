# Markdown Preview Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add optional Markdown rendering with Mermaid and double-click navigation to source, without starting a renderer during ordinary editing.

**Architecture:** A small C host adapter dynamically loads a C++ DLL through a versioned C interface. The DLL parses an immutable UTF-8 snapshot, serves local assets to installed Evergreen WebView2, and returns validated source offsets. Scintilla remains the document owner and keeps its existing handle and undo history.

**Tech Stack:** Win32 C/C++, Scintilla wrappers, MD4C with a narrow source-range extension if required, WebView2 SDK/WRL, bundled Mermaid JavaScript, native C++ regression harnesses, PowerShell and existing AutoHotkey UI-test conventions.

**Spec:** [Approved Markdown preview design](../specs/2026-10-28-markdown-preview-design.md).

## Global Constraints

- DLL filename: `MarkdownPreview.dll`; no editor import dependency on it or the WebView2 loader.
- Eligibility: current filename ends in `.md`, case-insensitively; exclude untitled buffers and other extensions.
- Ctrl+Alt+M and the Render/Source button toggle views; the View menu works with a hidden toolbar.
- Preview current unsaved buffer; task checkboxes remain read-only.
- Exact text-position mapping; images, Mermaid, and generated decorations fall back to the owning source block.
- Source editor content, undo history, modified state, encoding, and line endings are preserved.
- Win32, x64, x64_AVX2, and ARM64; use the repository toolset/output conventions.
- Missing DLL/runtime must leave ordinary editing usable. Runtime installation is never automatic.
- No renderer initialization or parsing before a preview request; release preview resources on return.
- Preserve existing user edits; use PathLib on HPATHL and SciCall wrappers; store host state in existing global state structs.
- Resources are UTF-8 without BOM and CRLF; new localized string IDs require all 26 locale entries.

## Review Focus

1. Empty/repeated text and entity/surrogate boundaries must resolve deterministically without word searches (Task 1).
2. Closing a window or replacing a file during asynchronous creation must not revive preview or call freed code (Tasks 3 and 5).
3. Toolbar customization, reset, and hidden-toolbar layouts must preserve saved user settings (Task 4).
4. AltGr typing and held toggle keys must not repeatedly switch views or consume ordinary source input (Tasks 3 and 4).
5. Image redirects, escaped paths, SVG content, and Mermaid configuration directives must not escape the approved resource policy (Tasks 2 and 3).

## File responsibilities and execution order

Tasks are sequential because parser output, DLL events, host lifecycle, and packaging share contracts. Execute in an isolated worktree using the worktree skill. Do not split this into independently shipped subsystems.

Before Task 1, build and retain an untouched Release/x64 baseline in the execution worktree and record the startup samples described in Task 6. Do not wait until after host edits to collect the baseline.

New files:

- `src/MarkdownPreview.h`, `src/MarkdownPreview.c`: host adapter, eligibility, lazy loading, lifecycle, command policy; no renderer implementation.
- `src/MarkdownPreview/PreviewApi.h`: shared C ABI; included by C host and C++ DLL.
- `src/MarkdownPreview/MarkdownDocument.h/.cpp`: parser, escaped HTML, mapping runs and resource records.
- `src/MarkdownPreview/PreviewWindow.cpp`: DLL exports, child window, controller, asynchronous lifecycle, host event delivery.
- `src/MarkdownPreview/PreviewResources.h/.cpp`: packaged asset serving and image request policy.
- `src/MarkdownPreview/assets/preview.js`, `preview.css`, `index.html`: DOM hit testing, Mermaid, selection, and presentation.
- `src/MarkdownPreview/vendor/`, `dependencies.json`, `THIRD-PARTY-NOTICES.txt`: pinned parser/browser SDK/JS versions, hashes, licenses; MD4C source lives in `vendor/md4c/`, bundled Mermaid assets in `assets/vendor/mermaid/`.
- `src/MarkdownPreview/MarkdownPreview.vcxproj`, `.vcxproj.filters`, `packages.config`, `MarkdownPreview.def`: optional DLL build, SDK restore, stable export.
- `test/MarkdownPreview/`: `TestMarkdownDocument.cpp`, `TestPreviewResources.cpp`, `TestPreviewWindow.cpp`, `TestMarkdownHost.cpp`, `TestMarkdownPreview.ahk`, `TestMarkdownPreview.ps1`, and `fixtures/`.
- `Build/scripts/PackageMarkdownPreview.ps1`: optional architecture-specific add-on ZIP.
- `docs/domain/markdown-preview.md`: shipped behavior, DLL contract, dependency updates, verification instructions.

Existing integration files: `src/Notepad3.c/.h/.rc`, `src/Notepad3.vcxproj/.filters`, `src/Notepad3Util.c`, `language/common_res.h`, all locale `strings_*.rc` and `menu_*.rc`, `Notepad3.sln`, `.github/workflows/build.yml`, `docs/README.md`. Inspect actual menu filenames before editing locale files. Keep portable editor archives unchanged; distribute the add-on ZIP beside them.

## Shared data contracts

Define ABI version 1 with `__cdecl` functions and sized structures. Export exactly `HRESULT NP3Preview_GetApi(uint32_t version, uint32_t apiSize, NP3PreviewApi* api)` through the `.def` file, including an undecorated x86 export. Use opaque `NP3PreviewHandle` (`void*`), fixed-width integers, `HWND`, and HRESULT; no STL objects or cross-module allocation/free.

`NP3PreviewRequest` contains `cbSize`, parent HWND, generation (`uint64_t`), UTF-8 pointer/byte count (`uint64_t`), absolute source filename, initial rectangle, dark flag, and callback/context. `Begin(const NP3PreviewRequest*, NP3PreviewHandle*)` copies all borrowed strings before returning and starts asynchronous creation. All ABI calls and callbacks run on the host UI thread. Parsing occurs on a cancellable worker; it never calls the host.

`NP3PreviewEvent` contains size, generation, kind, HRESULT, UTF-8 byte offset, and command ID. Kinds: Ready, Error, NavigateSource, ToggleSource, HostCommand. The API table exposes `Begin`, `Resize(handle,const RECT*)`, `SetDark(handle,BOOL)`, `Copy(handle)`, `Focus(handle)`, and `Close(handle)`; each returns HRESULT except idempotent `Close`, which returns void. Ready means HTML and Mermaid processing completed; missing images do not delay readiness. Event data is callback-lifetime-only.

`Close` detaches host callbacks immediately and cancels background work. Async COM closures retain only shared internal state until they complete; they cannot touch a destroyed HWND/context. Keep the DLL loaded until process exit, and do not call FreeLibrary while internal work may exist. Host callback posts a private host message with copied POD event fields to avoid closing the controller reentrantly.

## Task 1: Markdown parsing and source mapping

**Files:** Create `MarkdownDocument.h/.cpp`, `vendor/md4c/`, dependency manifest/notices, `TestMarkdownDocument.cpp`, fixtures, and `TestMarkdownPreview.ps1` (Core suite).

**Interfaces:** `RenderDocument ParseMarkdown(std::string_view utf8, const std::atomic_bool& cancelled)`; `RenderDocument` owns HTML, `std::vector<TextRun> runs`, `std::vector<ImageRef> images`, and `std::vector<MermaidBlock> diagrams`. `TextRun` has numeric id, decoded UTF-8 text, per-UTF-16-boundary source byte offsets, and owning block offset. `ImageRef` has id, decoded destination, and source block offset. `MermaidBlock` has id, decoded code, and opening-fence offset. `std::optional<uint64_t> ResolveSource(const RenderDocument&,uint32_t runId,uint32_t utf16Offset)` rejects unknown/out-of-range positions.

- [ ] Add a native assertion harness, following tool discovery in `test/test_files/encoding/TestEncodingDetection.ps1`; compile the actual implementation files, not extracted duplicate logic. `TestMarkdownPreview.ps1 -Suite Core` must fail nonzero when sources/assertions fail, and report test counts on success.
- [ ] Add failing assertions for `"same **same**"`: the second rendered `same` begins at source byte 7, not byte 0; `"A &amp; B"`: the decoded `&` maps to byte 2; `"a😀b"`: UTF-16 boundary 3 maps to byte 5 and an interior surrogate boundary maps to the emoji start. Add CRLF/LF, backslash escapes, nested emphasis, multiline code spans, reference links, tables with repeated cells, task items, NUL replacement, empty input, and image/Mermaid block-start fixtures. Verify parse cancellation exits without partial accepted output.
- [ ] Run Core and confirm the new assertions fail for the intended missing implementation.
- [ ] Vendor a stable MD4C release at an exact commit/hash with its license. Enable tables/task lists/strikethrough and disable raw HTML interpretation. Use parser callbacks and source ranges; do not search rendered text in the input. Audit callback pointer provenance, especially synthesized spaces/newlines. If required, add explicit block/span/text source-range metadata in a small documented vendor patch under `np3_patches/md4c-source-ranges.patch`; include that extension in the mapping tests. Decode entities using MD4C's entity data and preserve source boundaries across decoding. Escape HTML and attributes and emit numeric image/diagram identifiers.
- [ ] Implement `ParseMarkdown` and `ResolveSource`; for text boundaries produced from one entity map interior boundaries to its source start and the ending boundary to its source end. Derive owning-block fallback from parser block positions, including empty images and fences. Keep mapping arrays out of DOM attributes; host-side lookup validates run id and UTF-16 offset.
- [ ] Run Core; require all assertions pass. Commit parser, mappings, harness, and pinned dependency together.

## Task 2: Packaged resources and preview frontend

**Files:** Create `PreviewResources.h/.cpp`, assets, `TestPreviewResources.cpp`; extend the Core harness, manifest, and notices.

**Interfaces:** `PreviewResources(const RenderDocument&,std::wstring_view sourcePath)` owns an immutable resource table. `ResourceReply Resolve(std::wstring_view uri, ResourceKind kind) const` returns status, content type, and bytes for packaged assets/authorized local images. `ResourceKind` distinguishes Document, Script, Style, Image, Other; unauthorized requests return a denial, not a filesystem fallback. `bool AllowRemoteImage(std::wstring_view uri)` permits HTTPS image requests only. Browser messages have `{version:1,generation,kind,runId,utf16Offset}`; image/diagram navigation uses `{blockId}` instead. Generation is a decimal string to avoid JavaScript integer truncation.

- [ ] Add failing tests for allowed package assets, local paths containing spaces/Unicode/`#`, `../` image references explicitly present in Markdown, unknown asset IDs, percent-encoded traversal, arbitrary `file:` navigation, blocked UNC image paths, and remote non-HTTPS schemes. Explicit parent-directory images may resolve, but only to exact image entries parsed from this snapshot; never grant directory-wide access. SVGs are served strictly as images and never as documents.
- [ ] Run Core to confirm the policy assertions fail before implementation.
- [ ] Implement resources under `https://np3-preview.invalid/`, serving only known opaque asset/image IDs through WebResourceRequested. Pin a current stable Mermaid bundle with transitive chunks and license notices; no CDN. Store package versions, source URLs, and SHA-256 hashes in the manifest. Select exact releases during this task after checking upstream advisories; never use an unpinned `latest` during normal builds.
- [ ] Implement frontend `renderPreview(payload)` and `sourceHitAt(clientX,clientY)` in `preview.js`. Payload contains generation (decimal string), escaped generated HTML, diagrams (`id`, `code`), and dark (boolean); transfer as JSON data, never by interpolating document contents into executable script. Hit results are either `{runId,utf16Offset}`, `{blockId}`, or null. Use DOM caret hit-testing and text-run IDs; merge adjacent text nodes only if mappings are merged too. Render read-only checkboxes, semantic tables, images, code, and Mermaid. Render each invalid diagram as escaped source plus an error. Mermaid uses strict mode and disallows document-supplied init/config overrides; include frontmatter config attempts in fixtures. Add dark CSS, scalable font sizing, wrapped prose, and horizontally scrollable code/tables.
- [ ] Prevent page navigation on external link clicks; offer Open Link through a controlled context action and allow in-page fragments. Add double-click handlers for text and block fallbacks. Copy uses browser selection. Source files never become script, HTML, or CSS. CSP permits only packaged scripts/styles, required Mermaid styling, approved images, and denies frames/forms/connect requests. Set no-referrer behavior on remote images.
- [ ] Run Core and commit. Frontend browser execution and DOM hit-testing are verified by Task 3, not claimed from native policy tests.

## Task 3: Asynchronous WebView2 DLL

**Files:** Create `PreviewApi.h`, `PreviewWindow.cpp`, DLL project/filters/def/packages.config, `TestPreviewWindow.cpp`; extend harness with `-Suite WebView`.

**Interfaces:** Implement the shared ABI exactly. Internal state holds generation, cancellation flag, child HWND, controller/environment, pending worker result, and event registrations. Consume Task 1 parser and Task 2 resource resolver.

- [ ] Add integration harness assertions for ABI rejection, null/invalid requests, Begin/Ready/Close, invalid generation, Close during startup, closed parent, rapid reopen, parse failure, and missing-runtime handling. Missing WebView2 must be reported as a prerequisite failure for the browser suite, never as a passing skip.
- [ ] Run WebView and confirm expected failures before the DLL implementation.
- [ ] Add architecture-matched pinned Microsoft.Web.WebView2 SDK restore; statically link its loader only into this DLL. Use WRL COM helpers already supplied by the Windows SDK. Create the environment asynchronously in a writable per-user preview directory. Build an optional standalone DLL project using v145 and the repository Debug/Release/Release_AVX2 output conventions, without an editor link dependency.
- [ ] Implement worker parsing, UI-thread result delivery, virtual-origin resource handling, asynchronous controller creation, and callback cancellation. Set a 30-second initial load/render timeout and an initial 16 MiB source cap, both named constants with localized errors; measure in Task 6 and revise only if evidence warrants it. Mermaid has per-diagram limits of 50,000 characters and 500 edges. A timeout or renderer process failure closes only this controller and returns an error; never kill shared browser processes.
- [ ] Deny arbitrary navigation/new windows, downloads, permission requests, web host objects, document scripts, and invalid web messages. Validate origin, schema, generation, text-run IDs, and offsets; resolve source positions natively. Intercept non-image external requests and validate image redirects as well as initial URLs. Reject unsupported/redirection cases safely with a placeholder.
- [ ] Implement AcceleratorKeyPressed for Ctrl+Alt+M, host command forwarding, copy, scrolling, and zoom. Ignore autorepeat for toggle. Do not interpret AltGr-generated input as Ctrl+Alt+M; preserve normal source typing. Handle focus and resizing without changing the Scintilla handle.
- [ ] Run WebView: real DOM text-hit assertions for repeated words/entities/emoji/tables; Mermaid SVG and invalid-diagram fallback; image and diagram source fallback; denied resource requests/config injection; shortcut while browser focused; cancellation and closed-window callbacks. Check child visibility and cleanup over 25 toggle cycles. Run Core again only if shared parser/resources changed. Commit DLL and integration tests.

## Task 4: Host adapter, contextual button, and toggle

**Files:** Create `src/MarkdownPreview.c/.h`, `TestMarkdownHost.cpp`; modify `src/Notepad3.h/.c/.rc`, project/filters, `language/common_res.h`, locale menus/strings, and toolbar helpers only where necessary. Extend harness with `-Suite Host` and `TestMarkdownPreview.ahk`.

**Interfaces:** `bool MarkdownPreview_IsEligible(HPATHL path)`; `void MarkdownPreview_Toggle(HWND parent)`; `void MarkdownPreview_Close(bool restoreSelection)`; `void MarkdownPreview_Resize(const RECT* rect)`; `void MarkdownPreview_SetDark(bool dark)`; `void MarkdownPreview_OnEvent(const NP3PreviewEvent* event)`; `void MarkdownPreview_OnDocumentChanged(void)`. Store module/API, lifecycle, generation, selection/scroll state, and preview HWND/handle in a `MarkdownPreviewState` member of Globals. Source navigation uses `DocPos` after range checks.

- [ ] Add Host assertions for `.md`, `.MD`, `.markdown`, untitled, missing/mismatched DLL, load-on-first-toggle, and generation filtering. Add AHK scenarios checking button visibility and Ctrl+Alt+M source/preview toggles without requiring a saved buffer.
- [ ] Run Host and confirm the adapter is absent/fails those assertions.
- [ ] Implement absolute application-directory DLL loading, ABI checks, and UTF-8 buffer copying with SciCall wrappers. Keep source visible during loading and cancel pending rendering on source text changes. Errors return to source; offer the official runtime download URL only through an explicit user action. Copy event payloads before posting to the main window.
- [ ] Add command `IDM_VIEW_MARKDOWN_PREVIEW`, toolbar ID `IDT_VIEW_MARKDOWN_PREVIEW`, and Ctrl+Alt+M accelerator using unused resource IDs determined from `common_res.h`. Add menu label, Render/Source strings, and error messages in every locale with English placeholders. Existing enabled-state predicates must agree with shortcut behavior.
- [ ] Append a contextual text button after toolbar configuration, excluding it from serialization, customization indices, and reset lists. Reinsert/re-evaluate after customize/reset/theme reconstruction. Do not shift existing numeric button IDs. Fit the preview into MsgSize's editor rectangle and hide/show Scintilla only on successful transitions; keep `g_hwndEditWindow` unchanged. Restore selection/scroll on toggle or unfold/show the mapped caret on double-click.
- [ ] Run Host plus AHK cases for toolbar reset/customization persistence, toolbar hidden, DPI and theme changes, focus, source buffer unchanged, disk-encoded files loaded as UTF-8, and AltGr input. Commit adapter/UI with all required resource entries.

## Task 5: Command and file lifecycle integration

**Files:** Modify `MarkdownPreview.c/.h`, `src/Notepad3.c`; extend Host/AHK tests.

**Interfaces:** `bool MarkdownPreview_FilterCommand(HWND hwnd, UINT command)` returns true only when the command was consumed/blocked. `bool MarkdownPreview_CommandEnabled(UINT command)` is shared by menu/toolbar state. `void MarkdownPreview_BeforeDocumentOperation(void)` closes/cancels preview and invalidates generation.

- [ ] Add failing assertions that edit commands, including direct WM_COMMAND, cannot mutate hidden source. Check preview Copy copies browser selection, while Find/Replace/Go To/Print return to source then run. Save saves current source. Check New/Open/Save As/Revert/external reload and shutdown during creation; cancelled file operations may leave source visible.
- [ ] Run Host/AHK to demonstrate failures before lifecycle hooks.
- [ ] Gate commands at MsgCommand before individual handlers, normalize toolbar IDs to their command mapping, and share the policy with MsgInitMenu/toolbar updates. Explicitly audit mutating `CMD_*`, encoding, line-ending, sorting, undo/redo, drag/drop, pasteboard, and file-monitor paths. An unexpected SCN_MODIFIED while preview is active invalidates mapping and returns to source immediately; do not treat hiding the editor as a write lock.
- [ ] Call BeforeDocumentOperation at the converged load/save-as/reload/shutdown points, preserving existing save prompts. Maintain saved editor read-only state without confusing file read-only with preview mode. Stop accepting stale callbacks before HWND destruction. Ensure all legitimate exit paths restore editor focus and command state.
- [ ] Run Host/AHK and WebView cancellation cases. Assert buffer bytes, undo availability, dirty flag, selection, and source scroll before/after transitions. Commit command/lifecycle integration.

## Task 6: Packaging, build matrix, and measured verification

**Files:** Add `Build/scripts/PackageMarkdownPreview.ps1`, `docs/domain/markdown-preview.md`; modify `Notepad3.sln`, `.github/workflows/build.yml`, docs index, test harness Package suite. Keep packaging of unrelated editor files unchanged.

**Interfaces:** `PackageMarkdownPreview.ps1 -Platform <Win32|x64|ARM64> -Configuration <Release|Release_AVX2> -OutputDirectory <path>` produces a ZIP containing matching DLL, locally bundled assets, and notices. `TestMarkdownPreview.ps1 -Suite Package -PackagePath <zip>` checks expected files, hashes, licenses, and PE architecture. x64_AVX2 uses Release_AVX2/x64.

- [ ] Add failing Package assertions for wrong/missing DLL architecture, missing Mermaid chunks/licenses, and unexpected browser runtime binaries. Add opt-in CI builds/tests for the DLL plus all architecture compilation; require browser integration on a Windows runner with installed runtime. Record any unavailable runtime/architecture execution as unverified, not passed.
- [ ] Add the DLL as a solution project with correct configuration mappings, allowing standalone editor builds without the optional package. Package DLL/assets without a fixed browser runtime. Use native PowerShell filesystem operations and validate staging paths before recursive cleanup. Run Package until all assertions pass.
- [ ] Capture a baseline editor build before host changes in the execution worktree and a same-configuration final build. Run 10 startup samples each using the same fixture and WaitForInputIdle boundary; report medians and spread. Inspect loaded modules and child processes before the first preview. Require no MarkdownPreview/WebView2 modules/processes due to the feature at ordinary startup. Record first-preview latency and memory, return-to-source resource release, and source responsiveness during a large render.
- [ ] Run focused Core, Host, WebView, and Package suites once on final code. Run the AHK feature matrix in an interactive Windows session and inspect real preview output for all fixtures. Measure 16 MiB boundary/cancellation, long single lines, and several Mermaid blocks; document any limit changes and rerun affected cases only.
- [ ] Restore packages, run `Version.ps1` following existing build instructions, and build the full `Notepad3.sln` default target with MSBuild for Release/x64. Compile DLL/integration for Win32, ARM64, and Release_AVX2/x64, checking exit codes and output timestamps; document configurations that cannot be verified locally. Do not use `/t:Notepad3` on the solution. Retain unrelated generated/version changes only if required for delivery.
- [ ] Document runtime prerequisites, install/uninstall of optional DLL/assets, shortcut, resource rules, source navigation, limits, dependency regeneration, test commands, and measured performance. Update the docs index and replace proposed integration notes with canonical links where appropriate. Commit packaging/docs/tests, then follow the required code-review and branch-completion workflow before integration.

## Verification commands

From the execution worktree, run `powershell -NoProfile -ExecutionPolicy Bypass -File test/MarkdownPreview/TestMarkdownPreview.ps1 -Suite Core` (or Host/WebView/Package as defined above). The harness must print assertion totals and exit zero only on success. Locate MSBuild with vswhere as documented in `docs/build-and-test/windows-build-and-test.md`, then use `& $msbuild Notepad3.sln /m /p:Configuration=Release /p:Platform=x64 /nologo /v:minimal`. Use the new DLL project path for narrow standalone DLL builds.

## Technical references

- [MD4C source and API](https://github.com/mity/md4c): parser callbacks and table/task-list extensions; verify source-position assumptions against the pinned source, especially synthesized text.
- [WebView2 controller API](https://learn.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/icorewebview2controller): close/focus/accelerator integration.
- [Mermaid configuration](https://mermaid.js.org/config/schema-docs/config): strict mode and diagram limits; secure configuration remains controlled by the application.

## Self-review

Spec coverage: parser/features/mapping in Tasks 1–2; DLL isolation/rendering/errors/security in Task 3; button/menu/shortcut/source navigation in Task 4; hidden-editor safety and file operations in Task 5; architecture support, optional delivery, docs, and performance in Task 6. Each Review Focus item has an owning test step. API event fields and function names are shared by declaration instead of independently redefined. Exact dependency versions are deliberately selected and locked during their dependency tasks after upstream checks.

## Execution handoff

Recommended method: native execution in this session, sequentially through the six tasks, followed by an independent whole-branch review. The ABI and generation lifecycle make these tasks tightly coupled. Await user review of this plan and choice of execution method as required by writing-plans before adding product code or dependencies.
