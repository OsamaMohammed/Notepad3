# Markdown preview DLL

Status: proposed; awaiting user approval.

## Intent and agreed requirements

Add a reading view for `.md` files inside Notepad3. A button replaces the source editor with rendered Markdown. Double-clicking rendered text returns to source and places the caret at the corresponding text. The preview is read-only, including task checkboxes.

Support headings, paragraphs, emphasis, lists, block quotes, links, fenced code, tables, task lists, images, and Mermaid fenced blocks. Render the current editor buffer, including unsaved edits. Keep parsing and rendering in a separately built DLL loaded on demand to avoid adding rendering initialization to normal editing.

The user selected a DLL using the installed WebView2 runtime rather than distributing a fixed browser runtime. Mermaid support is explicitly confirmed. A keyboard shortcut is required; Ctrl+Alt+M is the proposed binding, unused in the current main accelerator table in `src/Notepad3.rc`.

## Approaches

1. **Selected: on-demand DLL with installed Evergreen WebView2.** Smaller add-on distribution; requires the installed runtime. Bundle the Markdown parser, Mermaid, CSS, and fonts needed by the preview locally, with no CDN dependency.
2. **Rejected: DLL with a bundled fixed WebView2 runtime.** Avoids a separate runtime installation but substantially increases distribution size and makes browser updates our responsibility.

Reference: https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/distribution

## User behavior

The following details are proposed implementation decisions, not additional user requirements:

- Show the Render button for a current filename ending in `.md`, case-insensitively. Other lexer-associated extensions and untitled buffers are outside the initial scope. Re-evaluate after Open, New, Save As, and file reload.
- Treat the Render button as a contextual control independent of a user's saved toolbar button list, so existing toolbar customizations do not hide the feature. When the toolbar itself is hidden, the View menu and shortcut remain available.
- In preview, change the button label/tooltip to Source. The same button, View command, and Ctrl+Alt+M toggle back. Handle the shortcut even when WebView2 has keyboard focus.
- Entering preview captures the source selection and scroll position. Returning via the toggle restores them. Double-clicking returns to the mapped source caret position and scrolls it into view.
- Double-clicking an image or Mermaid output returns to the start of its defining source block. Double-clicking generated decorations without source text uses the nearest owning block.
- Task boxes display their state but cannot be toggled. Selection, copying, scrolling, and zooming in the preview are allowed.
- Use the editor's light/dark appearance and respond to window resize and DPI changes. Preserve source editor content, undo history, modified state, encoding, and line endings.
- A link's single click does not navigate out of the preview; offer Open Link through a context menu so double-clicking linked text reliably returns to source. Open only approved HTTP/HTTPS destinations in the system browser. In-document fragment links may scroll within the preview.
- Relative images resolve against the Markdown file's directory. Support local images and HTTPS images; remote images require connectivity. Missing images show an unobtrusive placeholder. No network requests are needed for Markdown or Mermaid rendering itself.
- An invalid Mermaid block shows an error beside its original code; other content still renders.

## Architecture and DLL boundary

Add a C++ preview DLL with a small versioned C ABI callable by the existing C host. The host owns the source document, active-view state, command routing, and source navigation. The DLL owns the preview child window, WebView2 controller, HTML generation, bundled assets, and source mapping.

The host loads the DLL from the application directory using an explicit absolute path and safe dependency search rules, never by searching the working directory. Avoid a static import dependency from the editor to the preview DLL or WebView2 loader. A missing add-on must not prevent editor startup.

The interface needs version negotiation, asynchronous creation/render completion, resize/theme updates, destruction, error reporting, and a source-navigation callback. Pass a copied immutable UTF-8 source snapshot, source filename/base directory, document generation, and appearance settings. Document ownership and lengths in the interface; never free memory across module boundaries with a different allocator. The DLL must not read Scintilla directly.

Represent lifecycle as source, loading, preview, and closing states. Keep the source visible and usable until the preview is ready. Cancelling or editing while loading invalidates the pending snapshot. Every asynchronous callback must carry a generation token and be ignored after cancellation, document replacement, or shutdown.

Keep the source editor alive while previewing. Do not repurpose its text or styles. Hide it only after successful rendering. On return, close the WebView2 controller and release preview-owned browser resources. Keep the DLL loaded until application exit if necessary to ensure callbacks cannot execute unloaded code; all rendering work must stop on close. A DLL alone does not guarantee zero active-preview CPU or memory cost, and immediate disappearance of shared runtime processes is not a requirement.

## Mapping rendered text to source

Map positions during parsing and HTML generation, not by searching for clicked words. Repeated text, hidden Markdown syntax, entity decoding, Unicode, and table cells must remain distinguishable.

Emit identifiers for rendered text runs with mappings from DOM UTF-16 character offsets to original UTF-8 source byte offsets. Account for escapes, entities, normalized whitespace, and removed formatting delimiters. Obtain the DOM text position at the double-click location and resolve it through this mapping. Generated text falls back to its owning source block.

Convert UTF-8 snapshot offsets to the current Scintilla document byte positions when the document's internal code page differs. Validate all incoming offsets against the snapshot and generation. Never navigate using stale mappings. Use existing `SciCall.h` wrappers for caret placement, unfolding, and visibility.

Parser choice is left to the builder, subject to source-range fidelity, tables/task-list support, compatible licensing, offline packaging, and focused mapping tests. Preserve line-ending and Unicode offset correctness through conversion.

## Commands and document lifecycle

Centralize preview transitions and command restrictions rather than scattering special cases across individual editing actions. While preview owns focus, route copying to the preview and prevent source-mutating menu commands, toolbar commands, and accelerators from changing the hidden document. Disable source editing commands; returning to source restores existing command state rules. Source-oriented Find/Replace, Go To, and Print commands return to source before running; preview printing/search UI is outside scope.

File Save continues to save the underlying document. Open, New, Revert/reload, external-change reload, Save As, and shutdown leave or cancel preview before replacing or reinterpreting the source snapshot. Existing save prompts and file-watching behavior remain authoritative. A cancelled file operation may leave the source view visible.

Integration points to inspect during planning:

- `src/Notepad3.c`: editor creation, `MsgSize`, `MsgCommand`, `MsgInitMenu`, `_HandleToolbarCommands`, `s_ToolbarDispatch`, `_UpdateToolbarDelayed`, message-loop accelerator processing, file lifecycle.
- `src/Notepad3.h`: existing global state structs for preview lifecycle fields; no new free-standing globals.
- `src/Notepad3.rc`, `language/common_res.h`, locale resource files: command, shortcut, label, and errors.
- `src/Notepad3Util.c`: toolbar construction/customization helpers.
- `src/SciCall.h`: document extraction/navigation wrappers.
- `Notepad3.sln`, `src/Notepad3.vcxproj`, and `Build/` scripts: optional preview build and distribution for Win32, x64, x64_AVX2, and ARM64.

`src/SciCall.h` initializes `g_hwndEditWindow` as the Scintilla handle. Keep that association intact; lay out the preview child separately rather than redirecting Scintilla wrappers to it.

## Content isolation and failures

Treat document contents as data. Escape raw HTML in Markdown for this first version. Disable arbitrary document scripts, embedded frames, forms, and navigation. Only packaged renderer code executes. Use Mermaid's strict security configuration and disallow external diagram directives that relax it. Restrict resource requests to packaged assets, supported image requests, and the explicitly resolved local image resources; do not expose a general filesystem browsing endpoint or native execution bridge.

Validate WebView messages against the current preview origin/session, expected message schema, and document generation. Allow only source navigation and intended UI messages. Use a writable per-user preview data directory, not the program installation directory.

If the DLL is missing, incompatible, or fails to initialize, stay in source and show a localized actionable error. If WebView2 is missing, explain the runtime requirement and offer the official download link; do not install it automatically. Render failures must leave the original buffer untouched. Allow cancellation during slow rendering, ignore late results, and stop runaway rendering after a bounded timeout. Exact limits and timeout are left to the builder with large-document measurements.

## Performance and delivery

Before the first preview request, do not load the preview DLL, initialize WebView2, parse Markdown, or start renderer processes. The only ongoing host work is eligibility/command-state checking. First preview can take longer while the runtime starts; show a cancellable loading indication without blocking typing.

Build the DLL and its assets as an optional distributable component. Package matching architectures, pinned library versions, and required license notices. Ordinary editing must work when the component is absent. Runtime installation is a deployment prerequisite for preview only.

Measure ordinary editor startup before and after the change using the same build configuration and repeated runs. Check loaded modules/processes before preview, during preview, and after return. Report measurements rather than claiming zero overhead from the DLL structure alone.

## Verification and acceptance

- Focused automated parser/mapping tests: repeated words, nested emphasis, links, entities, escapes, non-ASCII text, surrogate pairs, CRLF/LF, tables, code blocks, task items, and source code-page conversion.
- Render fixtures for all requested Markdown features, local/HTTPS/missing images, valid/invalid Mermaid, and light/dark mode.
- UI coverage: `.md`/`.MD` eligibility, non-Markdown and untitled buffers, customized/hidden toolbar, button/menu/shortcut toggles, preview keyboard focus, exact double-click source navigation, and block fallback.
- Lifecycle coverage: unsaved edits, preserved undo/modified state, cancellation during load, repeated toggles, commands while hidden, Open/New/Save As/reload, external changes, and shutdown with callbacks pending.
- Failure coverage: absent/incompatible DLL, absent runtime, render timeout, malformed content/messages, and prohibited resource/navigation attempts.
- Run the narrow relevant tests and full solution build for the primary development architecture, then compile the new DLL/integration across all shipped architectures before release. Follow `docs/build-and-test/windows-build-and-test.md`.

## Scope and next step

This is a feature project spanning rendering, source mapping, Win32 integration, and packaging. It does not include editing inside preview, live split views, PDF/HTML export, MDX execution, a general plugin manager, or bundled browser runtime maintenance.

After user approval, write the implementation plan using the writing-plans skill. No product code or dependencies are added before that approval.
