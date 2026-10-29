# Markdown preview

Optional reading view for `.md` documents. The feature is an add-on: the editor builds, starts, and edits normally when the component is absent.

## Behavior

- Eligible buffer: current filename ends in `.md` (case-insensitive). Untitled buffers and other extensions stay ineligible during the initial release.
- Toggle: View > Markdown Preview, `Ctrl+Alt+M`, or the contextual Render/Source toolbar button. Inside the preview, `Ctrl+Alt+M` is handled by the DLL's `AcceleratorKeyPressed` so it works while the browser has focus. An AltGr+M keystroke is document input, not the shortcut.
- The toolbar button is inserted at runtime for eligible documents only. It is not offered in the Customize Toolbar list and is never written to the saved toolbar layout, so it survives customization, reset, and theme rebuilds without becoming a user setting.
- The source editor stays visible until the DLL reports `Ready`; only then is `s_hwndEditFrame` hidden. Returning restores the caret, anchor, and first visible line captured when preview started.
- Double-clicking rendered text returns to the mapped source byte offset; images, Mermaid output, and generated decorations fall back to their owning block.
- Preview is read-only: task checkboxes keep their state, links do not navigate on single click, and Copy routes browser selection to the clipboard. Other commands leave preview first (`MarkdownPreview_FilterCommand`) so no command can edit a hidden buffer.

## Integration points

| Location | Role |
|---|---|
| `src/MarkdownPreview.h/.c` | Loads the DLL, copies the UTF-8 buffer, filters commands, restores source state, posts events |
| `src/MarkdownPreview/PreviewApi.h` | Shared C ABI (version 1) between the C host and C++ DLL |
| `src/MarkdownPreview/PreviewWindow.cpp` | DLL exports, child window, WebView2 lifecycle, resource and message policy |
| `src/MarkdownPreview/MarkdownDocument.cpp` | MD4C parsing, escaped HTML, rendered-text-to-source mappings |
| `src/MarkdownPreview/PreviewResources.cpp` | Virtual `https://np3-preview.invalid/` origin, asset/image policy |
| `src/Notepad3.c` | `MarkdownPreview_GetSourceHwnd()`, `MsgSize`, `MsgCommand`, `MsgInitMenu`, toolbar button, `FileLoad`/`FileRevert`/shutdown hooks |
| `src/TypeDefs.h` | `MARKDOWNPREVIEW_STATE_T` stored in `Globals.MarkdownPreview` |
| `language/common_res.h` | `IDM_VIEW_MARKDOWN_PREVIEW` (41066), `IDT_VIEW_MARKDOWN_PREVIEW` (11032), `IDS_MUI_*_MARKDOWN_PREVIEW*` strings |

`src/SciCall.h` still targets `g_hwndEditWindow`; the preview child window is laid out separately and never redirected through those wrappers.

## Host/DLL contract

- ABI version 1, `__cdecl`, `NP3Preview_GetApi(version, apiSize, api)` as the only export.
- `Begin` copies the UTF-8 snapshot, filename, and labels before returning; parsing runs on a cancellable worker and never calls the host.
- Every event carries a `generation`; the host ignores events after cancel, document replacement, or close (`MarkdownPreview_ReturnToSource` increments the generation before `Close`).
- DLL callbacks post `WM_MARKDOWNPREVIEW_EVENT` with a heap copy of `NP3PreviewEvent`, so the controller is never closed reentrantly.
- The DLL stays loaded for the process lifetime; the host never calls `FreeLibrary` once a preview started.
- Limits: 16 MiB source snapshot, 30 s initial load/render timeout, 50,000 characters and 500 edges per Mermaid diagram. Each is a named constant in `MarkdownPreview.c`/`PreviewWindow.cpp`.

## Security model

- Document HTML is escaped; MD4C runs with raw HTML disabled.
- Only packaged renderer code executes: CSP, no frames/forms/connect requests, no host objects, dev tools off, downloads/new windows/permissions denied.
- Resources resolve only through opaque asset/image IDs. Local images must appear in the parsed snapshot; SVGs are served as images; non-HTTPS remote images are blocked.
- Web messages are validated against page origin, schema version, generation, and text-run/block IDs; offsets are resolved natively with `ResolveSource`.
- Mermaid runs in strict mode; `%%{init: ...}%%` overrides are rejected per fixture.

## Deployment

- Runtime prerequisite: Microsoft Edge WebView2 Evergreen Runtime. The editor never installs it; a missing runtime shows a localized message with an explicit link to the official download page.
- Ship `MarkdownPreview.dll` plus its `MarkdownPreview\` asset folder next to `Notepad3.exe`, or as an add-on ZIP from `Build\scripts\PackageMarkdownPreview.ps1`.
- No browser runtime is bundled; the ZIP intentionally contains no `WebView2Loader.dll` or `msedgewebview2*` files.

## Dependency updates

`src/MarkdownPreview/dependencies.json` pins MD4C (commit + SHA-256), Mermaid (integrity + per-file SHA-256), and the WebView2 SDK version. `src/MarkdownPreview/THIRD-PARTY-NOTICES.txt` carries the matching licenses (MD4C, Mermaid, WebView2 SDK).

- MD4C: vendored under `src/MarkdownPreview/vendor/md4c/` with a source-range patch (`np3_patches/md4c-source-ranges.patch`). Re-verify the patch after upstream updates.
- Mermaid: extracted from the pinned npm tarball into `assets/vendor/mermaid/`; keep the chunk set and `LICENSE`.
- WebView2 SDK: restored per architecture from `packages.config` into `packages\Microsoft.Web.WebView2.<version>\`.

## Build and verification

The DLL is intentionally not part of `Notepad3.sln`: the solution also builds `ARM` (32-bit), for which the WebView2 SDK has no libraries. Build it per architecture:

```
msbuild src\MarkdownPreview\MarkdownPreview.vcxproj /p:Configuration=Release /p:Platform=x64
```

Suites (`-VisualStudioPath` is needed when `vswhere` is unavailable):

```
powershell -NoProfile -ExecutionPolicy Bypass -File test\MarkdownPreview\TestMarkdownPreview.ps1 -Suite Core
powershell -NoProfile -ExecutionPolicy Bypass -File test\MarkdownPreview\TestMarkdownPreview.ps1 -Suite WebView
powershell -NoProfile -ExecutionPolicy Bypass -File test\MarkdownPreview\TestMarkdownPreview.ps1 -Suite Host
powershell -NoProfile -ExecutionPolicy Bypass -File test\MarkdownPreview\TestMarkdownPreview.ps1 -Suite UI
powershell -NoProfile -ExecutionPolicy Bypass -File Build\scripts\PackageMarkdownPreview.ps1 -Platform x64 -OutputDirectory out
powershell -NoProfile -ExecutionPolicy Bypass -File test\MarkdownPreview\TestMarkdownPreview.ps1 -Suite Package -PackagePath out\MarkdownPreview-Release-x64.zip
```

- `Host` launches a real editor and asserts the no-renderer-before-first-toggle rule, preview visibility, hidden source, preserved buffer, restored caret, and the contextual button.
- `UI` drives the same build through AutoHotkey v2 from a private copy with a fresh portable `Notepad3.ini`, so it never reads or writes the user's settings. It covers the contextual button, toolbar customization and reset, a hidden toolbar, live theme switching with sampled preview pixels, resize propagation, `Ctrl+Alt+M` from both views, and AltGr input. It needs an interactive desktop (it is not part of CI) and skips checks the machine cannot produce, such as a `WM_DPICHANGED` when every monitor reports the same DPI.
- `TestMarkdownHost.exe` reads editor state only through pointer-free messages; `SCI_GETTEXT` across processes corrupts the editor process. The UI suite likewise only sends messages whose parameters are integers, plus `BM_CLICK` on standard dialog buttons.

## Measured baseline (Release/x64, Windows 10, 10 samples, `WaitForInputIdle`)

| Build | Median | Min | Max |
|---|---|---|---|
| Untouched baseline (`baseline-Notepad3.exe`) | 271.0 ms | 255.1 ms | 407.4 ms |
| Feature build | 279.0 ms | 254.8 ms | 391.0 ms |

Repeated rounds gave 266–287 ms for both binaries, so the difference is inside run-to-run spread; no startup regression is demonstrated. At ordinary startup the process loads no `MarkdownPreview`/`EmbeddedBrowserWebView` module and starts no WebView2 child process.
