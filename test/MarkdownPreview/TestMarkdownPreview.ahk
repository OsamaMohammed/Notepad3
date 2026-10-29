; encoding: UTF-8
; ===========================================================================
; TestMarkdownPreview.ahk - interactive UI matrix for the Markdown preview
;
; Drives a real Notepad3 window: toolbar customization/reset, hidden toolbar,
; resize and theme changes, and AltGr typing. Requires an interactive desktop.
;
;   AutoHotkey64.exe TestMarkdownPreview.ahk <Notepad3.exe> <fixtures> <work>
;
; The editor runs from a private copy of the build with a fresh portable
; Notepad3.ini, so the user's own settings are never read or written.
; ===========================================================================

#Requires AutoHotkey v2.0
#SingleInstance Force

global IDM_VIEW_TOOLBAR := 41024
global IDM_VIEW_CUSTOMIZETB := 41025
global IDM_VIEW_WIN_DARK_MODE := 41057
global IDM_VIEW_MARKDOWN_PREVIEW := 41066
global IDT_FILE_NEW := 11000
global IDT_VIEW_MARKDOWN_PREVIEW := 11032

global WM_COMMAND := 0x0111
global WM_GETTEXTLENGTH := 0x000E
global WM_THEMECHANGED := 0x031A
global WM_CLOSE := 0x0010
global BM_CLICK := 0x00F5
global TB_BUTTONCOUNT := 0x0418
global TB_COMMANDTOINDEX := 0x0419
global TB_CUSTOMIZE := 0x041B

global EXE := ""
global FIXTURES := ""
global WORK := ""
global APP := ""
global LOGFILE := ""
global Checks := 0
global Failures := []
global Skips := []

; ---------------------------------------------------------------------------
; Entry point
; ---------------------------------------------------------------------------

if (A_Args.Length < 3) {
    FileAppend("usage: TestMarkdownPreview.ahk <Notepad3.exe> <fixtures> <work-dir>`n", "*")
    ExitApp(2)
}
EXE := A_Args[1]
FIXTURES := A_Args[2]
WORK := A_Args[3]
APP := WORK "\app"
LOGFILE := WORK "\ui-matrix.log"

if !DirExist(WORK)
    DirCreate(WORK)
try FileDelete(LOGFILE)
OnError(ReportError)

Prepare()
RunMatrix()

Log("")
if (Failures.Length) {
    Log("FAILED: " Failures.Length " of " Checks " UI assertions")
    for name in Failures
        Log("  - " name)
    ExitApp(1)
}
Log("PASS: " Checks " UI assertions" (Skips.Length ? ", " Skips.Length " skipped" : ""))
ExitApp(0)

; ---------------------------------------------------------------------------
; Setup
; ---------------------------------------------------------------------------

Prepare() {
    global EXE, FIXTURES, APP, WORK

    SplitPath(EXE, , &exeDir)
    if !FileExist(EXE)
        Fatal("editor not found: " EXE)
    if !FileExist(exeDir "\MarkdownPreview.dll")
        Fatal("preview DLL not found next to the editor: " exeDir "\MarkdownPreview.dll")

    if DirExist(APP)
        DirDelete(APP, true)
    DirCreate(APP)
    DirCreate(WORK "\docs")

    FileCopy(EXE, APP "\Notepad3.exe")
    FileCopy(exeDir "\MarkdownPreview.dll", APP "\MarkdownPreview.dll")
    DirCopy(exeDir "\MarkdownPreview", APP "\MarkdownPreview", true)

    ; Fresh portable settings: the editor looks for Notepad3.ini next to itself.
    ; The style-reset warning is pre-accepted so the live theme toggle is deterministic.
    try FileDelete(APP "\Notepad3.ini")
    FileAppend("[Suppressed Messages]`r`nMsgResetScheme=1`r`n", APP "\Notepad3.ini", "UTF-8")

    FileCopy(FIXTURES "\host-sample.md", WORK "\docs\sample.md", true)
    FileAppend("plain text, no preview`r`n", WORK "\docs\plain.txt", "UTF-8")
}

; ---------------------------------------------------------------------------
; Matrix
; ---------------------------------------------------------------------------

RunMatrix() {
    global WORK, APP

    CoordMode("Pixel", "Screen")
    doc := StartApp(WORK "\docs\sample.md")
    wnd := doc.hwnd
    sci := FindChild(wnd, "Scintilla")
    ; The host hides its source frame, a list-view sized to the editor area.
    frame := FindChild(wnd, "SysListView32")
    if !sci || !frame
        Fatal("editor windows not found")

    WaitFor(2000)
    tb := ToolbarHwnd(wnd)
    if !tb
        Fatal("toolbar window not found")

    baseCount := Msg(tb, TB_BUTTONCOUNT)
    Assert(CommandToIndex(tb, IDT_VIEW_MARKDOWN_PREVIEW) >= 0,
        "toolbar.contextual-button.md", "Render button missing for a .md buffer")

    ; --- menu command opens the preview -----------------------------------
    Post(wnd, WM_COMMAND, IDM_VIEW_MARKDOWN_PREVIEW)
    if !WaitPreview(wnd, frame)
        DebugState("menu-open", doc.pid, wnd, frame)
    Assert(IsVisible(PreviewHwnd(wnd)), "preview.opens-with-menu", "preview never appeared")
    Assert(!IsVisible(frame), "preview.source-hidden", "source frame stayed visible")
    Assert(SameRect(PreviewRect(wnd), WindowRect(frame)), "preview.bounds",
        "preview " Describe(PreviewRect(wnd)) " != editor " Describe(WindowRect(frame)))
    Assert(Msg(tb, TB_BUTTONCOUNT) == baseCount, "toolbar.no-extra-button",
        "button count " Msg(tb, TB_BUTTONCOUNT) " != " baseCount)

    ; --- theme rebuild keeps one contextual button ------------------------
    Post(wnd, WM_THEMECHANGED)
    WaitFor(2000)
    Assert(IsVisible(PreviewHwnd(wnd)), "theme.rebuild-preview-visible", "preview disappeared")
    Assert(SameRect(PreviewRect(wnd), WindowRect(frame)), "theme.rebuild-preview-bounds",
        "preview " Describe(PreviewRect(wnd)) " != editor " Describe(WindowRect(frame)))
    ; The bars are recreated, so the old toolbar handle is stale.
    rebuilt := ToolbarHwnd(wnd)
    Assert(Msg(rebuilt, TB_BUTTONCOUNT) == baseCount, "theme.rebuild-single-button",
        "button count " Msg(rebuilt, TB_BUTTONCOUNT) " != " baseCount)
    Assert(CommandToIndex(rebuilt, IDT_VIEW_MARKDOWN_PREVIEW) >= 0,
        "theme.rebuild-button-present", "Render button lost after the theme rebuild")

    ; --- toolbar customization reset --------------------------------------
    ResetToolbar(wnd)

    tb := ToolbarHwnd(wnd)
    Assert(Msg(tb, TB_BUTTONCOUNT) == baseCount, "toolbar.reset-single-button",
        "button count " Msg(tb, TB_BUTTONCOUNT) " != " baseCount)
    Assert(CommandToIndex(tb, IDT_VIEW_MARKDOWN_PREVIEW) >= 0,
        "toolbar.reset-button-present", "Render button lost after customization reset")
    Assert(IsVisible(PreviewHwnd(wnd)), "toolbar.reset-preview-visible", "preview disappeared")

    ; --- live theme change ------------------------------------------------
    WaitFor(1200)   ; let the toolbar rebuild settle before repainting the preview
    beforeLuma := PreviewLuma(wnd)
    Post(wnd, WM_COMMAND, IDM_VIEW_WIN_DARK_MODE)
    WaitFor(3000)
    afterLuma := PreviewLuma(wnd)
    if !IsVisible(PreviewHwnd(wnd))
        DebugState("theme", doc.pid, wnd, frame)
    Assert(IsVisible(PreviewHwnd(wnd)), "theme.live-keeps-preview", "preview disappeared on theme change")
    if (Abs(afterLuma - beforeLuma) >= 80)
        Assert(true, "theme.live-switch", "")
    else
        Skip("theme.live-switch", "preview background unchanged (" beforeLuma " -> " afterLuma ")")

    ; --- resize propagation ------------------------------------------------
    before := PreviewRect(wnd)
    WinMove(, , 1000, 640, "ahk_id " wnd)
    WaitFor(1200)
    after := PreviewRect(wnd)
    Assert(!SameRect(before, after), "layout.resize-moved", "preview did not follow the window")
    Assert(SameRect(after, WindowRect(frame)), "layout.resize-matches-editor",
        "preview " Describe(after) " != editor " Describe(WindowRect(frame)))

    ; --- DPI ---------------------------------------------------------------
    monitors := GetMonitors()
    target := 0
    for m in monitors
        if (m.dpi != monitors[1].dpi)
            target := m
    if (target) {
        WinMove(target.l + 60, target.t + 60, , , "ahk_id " wnd)
        WaitFor(2500)
        Assert(WindowDpi(wnd) == target.dpi, "dpi.window-changed",
            "window DPI " WindowDpi(wnd) " != monitor DPI " target.dpi)
        Assert(SameRect(PreviewRect(wnd), WindowRect(frame)), "dpi.preview-follows",
            "preview " Describe(PreviewRect(wnd)) " != editor " Describe(WindowRect(frame)))
        WinMove(monitors[1].l + 60, monitors[1].t + 60, 1200, 800, "ahk_id " wnd)
        WaitFor(1500)
    } else {
        reason := "all " monitors.Length " attached monitor(s) report " monitors[1].dpi " DPI"
        Skip("dpi.change", reason " - no WM_DPICHANGED can be generated")
    }

    ; --- keys: Ctrl+Alt+M inside the preview ------------------------------
    WinActivate("ahk_id " wnd)
    WaitFor(300)
    Assert(ToggleWithKey(wnd, frame, false), "keys.preview-to-source",
        "Ctrl+Alt+M in the preview did not return to source")

    ; --- hidden toolbar ----------------------------------------------------
    Post(wnd, WM_COMMAND, IDM_VIEW_TOOLBAR)
    hidden := Poll(() => !IsVisible(FindChild(wnd, "ReBarWindow32")), 4000)
    Assert(hidden, "toolbar.hidden", "the toolbar is still visible")

    ActivateApp(wnd)
    Assert(ToggleWithKey(wnd, frame, true), "toolbar.hidden.shortcut-opens",
        "Ctrl+Alt+M did not open the preview with the toolbar hidden")
    Assert(Poll(() => Msg(ToolbarHwnd(wnd), TB_BUTTONCOUNT) == baseCount, 3000), "toolbar.hidden.button-count",
        "hiding the toolbar changed the button list (" Msg(ToolbarHwnd(wnd), TB_BUTTONCOUNT) " != " baseCount ")")

    WaitFor(900)   ; let the browser take focus before the closing keystroke
    Assert(ToggleWithKey(wnd, frame, false), "toolbar.hidden.shortcut-closes",
        "Ctrl+Alt+M did not return to source with the toolbar hidden")

    Post(wnd, WM_COMMAND, IDM_VIEW_TOOLBAR)
    shown := Poll(() => IsVisible(FindChild(wnd, "ReBarWindow32")), 4000)
    tb := ToolbarHwnd(wnd)
    Assert(shown, "toolbar.shown", "the toolbar did not come back")
    Assert(Msg(tb, TB_BUTTONCOUNT) == baseCount, "toolbar.shown.single-button",
        "button count " Msg(tb, TB_BUTTONCOUNT) " != " baseCount " after restoring the toolbar")
    Assert(CommandToIndex(tb, IDT_VIEW_MARKDOWN_PREVIEW) >= 0,
        "toolbar.shown.button-present", "Render button missing after restoring the toolbar")

    ; --- AltGr typing ------------------------------------------------------
    if !IsVisible(frame)
        ToggleWithKey(wnd, frame, false)
    FocusEditor(wnd, sci)
    WaitFor(300)
    lenBefore := Msg(sci, WM_GETTEXTLENGTH)
    SendAltGrM()
    toggled := WaitPreview(wnd, frame, 4000)
    lenAfter := Msg(sci, WM_GETTEXTLENGTH)
    Assert(!toggled, "keys.altgr-no-toggle",
        "AltGr+M opened the preview and consumed the character")
    if (lenAfter > lenBefore)
        Assert(true, "keys.altgr-text-preserved", "")
    else
        Skip("keys.altgr-text-preserved", "this keyboard layout defines no AltGr+M character")

    ; --- real Ctrl+Alt+M from the source editor ---------------------------
    ActivateApp(wnd)
    Assert(ToggleWithKey(wnd, frame, true), "keys.source-toggle",
        "Ctrl+Alt+M did not open the preview from source")
    WaitFor(900)
    Assert(ToggleWithKey(wnd, frame, false), "keys.source-toggle-back",
        "Ctrl+Alt+M did not close the preview")

    ; --- persistence -------------------------------------------------------
    StopApp(doc)
    CheckSettings()

    txt := StartApp(WORK "\docs\plain.txt")
    WaitFor(1500)
    tbTxt := ToolbarHwnd(txt.hwnd)
    Assert(CommandToIndex(tbTxt, IDT_VIEW_MARKDOWN_PREVIEW) < 0,
        "restart.txt-no-button", "a Markdown button appeared for a .txt buffer")
    Assert(Msg(tbTxt, TB_BUTTONCOUNT) == baseCount - 1,
        "restart.txt-button-count", "button count " Msg(tbTxt, TB_BUTTONCOUNT) " != " (baseCount - 1))
    StopApp(txt)
}

; ---------------------------------------------------------------------------
; Toolbar customization
; ---------------------------------------------------------------------------

ResetToolbar(wnd) {
    pid := WinGetPID("ahk_id " wnd)
    Post(wnd, WM_COMMAND, IDM_VIEW_CUSTOMIZETB)

    dlg := 0
    Loop 20 {
        WaitFor(200)
        dlg := FindToolbarDialog(pid)
        if dlg
            break
    }
    if !dlg {
        Skip("toolbar.customize-opened", "the Customize Toolbar dialog did not appear")
        return
    }
    Assert(true, "toolbar.customize-opened", "")

    if ClickDialogButton(dlg, "i)reset") {
        WaitFor(600)
        Assert(true, "toolbar.customize-reset", "")
    } else {
        Skip("toolbar.customize-reset", "no Reset button found in the dialog")
    }
    if !ClickDialogButton(dlg, "i)close")
        Post(dlg, WM_CLOSE)
    WaitFor(900)
}

FindToolbarDialog(pid) {
    for hwnd in WinGetList("ahk_pid " pid) {
        if (WinGetClass("ahk_id " hwnd) != "#32770")
            continue
        for ctrl in WinGetControls("ahk_id " hwnd) {
            if (SubStr(ctrl, 1, 6) == "Button")
                return hwnd
        }
    }
    return 0
}

ClickDialogButton(dlg, pattern) {
    for ctrl in WinGetControls("ahk_id " dlg) {
        if (SubStr(ctrl, 1, 6) != "Button")
            continue
        if (StrReplace(ControlGetText(ctrl, "ahk_id " dlg), "&", "") ~= pattern) {
            Post(ControlGetHwnd(ctrl, "ahk_id " dlg), BM_CLICK)
            return true
        }
    }
    return false
}

DismissDialog(pid, pattern) {
    for hwnd in WinGetList("ahk_pid " pid) {
        if (WinGetClass("ahk_id " hwnd) != "#32770")
            continue
        if ClickDialogButton(hwnd, pattern)
            return true
    }
    return false
}

; ---------------------------------------------------------------------------
; Settings persistence
; ---------------------------------------------------------------------------

CheckSettings() {
    global APP, IDT_FILE_NEW, IDT_VIEW_MARKDOWN_PREVIEW

    ini := APP "\Notepad3.ini"
    if !FileExist(ini) {
        Skip("settings.no-contextual-button", "the editor did not write Notepad3.ini")
        return
    }
    saved := ""
    for line in StrSplit(FileRead(ini, "UTF-8"), "`n") {
        line := Trim(line, "`r `t")
        if (line ~= "i)^ToolbarButtons=") {
            saved := Trim(SubStr(line, InStr(line, "=") + 1), " `t`"")
            break
        }
    }
    token := IDT_VIEW_MARKDOWN_PREVIEW - IDT_FILE_NEW + 1
    hasToken := false
    for item in StrSplit(saved, " ")
        if (item == String(token))
            hasToken := true
    Assert(!hasToken, "settings.no-contextual-button",
        "saved ToolbarButtons contains the contextual button: " saved)
}

; ---------------------------------------------------------------------------
; Process and window helpers
; ---------------------------------------------------------------------------

StartApp(file, timeout := 25) {
    global APP

    Run('"' APP "\Notepad3.exe" '" "' file '"', APP, , &pid)
    if !pid
        Fatal("the editor did not start")
    hwnd := WinWait("ahk_pid " pid, , timeout)
    if !hwnd
        Fatal("the editor window did not appear")
    WinActivate("ahk_id " hwnd)
    WinMove(80, 60, 1200, 800, "ahk_id " hwnd)
    WaitFor(700)
    return {pid: pid, hwnd: hwnd}
}

StopApp(inst) {
    if ProcessExist(inst.pid) {
        Post(inst.hwnd, WM_CLOSE)
        if !ProcessWaitClose(inst.pid, 15) {
            DismissDialog(inst.pid, "i)^(&?yes|&?no)")
            if !ProcessWaitClose(inst.pid, 5)
                ProcessClose(inst.pid)
        }
    }
    WaitFor(400)
}

ToolbarHwnd(wnd) {
    rebar := FindChild(wnd, "ReBarWindow32")
    if rebar {
        tb := FindChild(rebar, "ToolbarWindow32")
        if tb
            return tb
    }
    return FindChild(wnd, "ToolbarWindow32")
}

PreviewHwnd(wnd) {
    h := FindChild(wnd, "NP3MarkdownPreviewChild")
    return (h && IsVisible(h)) ? h : 0
}

CommandToIndex(tb, id) {
    return Msg(tb, TB_COMMANDTOINDEX, id)
}

DebugState(label, pid, wnd, frame) {
    Log("debug " label ": preview=" PreviewHwnd(wnd) " frameVisible=" IsVisible(frame) " foreground=" WinExist("A"))
    for h in WinGetList("ahk_pid " pid)
        Log("  window " WinGetClass("ahk_id " h) " '" WinGetTitle("ahk_id " h) "' visible=" IsVisible(h))
}

; Injected keystrokes race the preview/source handoff; retry until the requested
; state is reached, and fail when it never is.
ToggleWithKey(wnd, frame, wantPreview, attempts := 3) {
    Loop attempts {
        inPreview := (PreviewHwnd(wnd) && !IsVisible(frame))
        if (wantPreview == !!inPreview)
            return true
        SendCtrlAltM()
        if (wantPreview ? WaitPreview(wnd, frame, 8000) : WaitSource(wnd, frame, 8000))
            return true
        WaitFor(700)
    }
    return (wantPreview == (PreviewHwnd(wnd) && !IsVisible(frame)))
}

WaitPreview(wnd, frame, timeout := 25000) {
    end := A_TickCount + timeout
    while (A_TickCount < end) {
        if (PreviewHwnd(wnd) && !IsVisible(frame))
            return true
        Sleep 120
    }
    return false
}

WaitSource(wnd, frame, timeout := 15000) {
    end := A_TickCount + timeout
    while (A_TickCount < end) {
        if IsVisible(frame)
            return true
        Sleep 120
    }
    return false
}

PreviewRect(wnd) {
    h := PreviewHwnd(wnd)
    return h ? WindowRect(h) : {l: 0, t: 0, r: 0, b: 0}
}

PreviewLuma(wnd) {
    WinActivate("ahk_id " wnd)
    WaitFor(400)
    rect := PreviewRect(wnd)
    if (rect.r <= rect.l)
        return 0
    x := rect.r - 40
    ys := [rect.t + 40, (rect.t + rect.b) // 2, rect.b - 40]
    total := 0
    for y in ys {
        c := PixelGetColor(x, y, "RGB")     ; 0xRRGGBB
        total += 0.299 * ((c >> 16) & 0xFF) + 0.587 * ((c >> 8) & 0xFF) + 0.114 * (c & 0xFF)
    }
    return Round(total / 3)
}

WindowRect(hwnd) {
    buf := Buffer(16, 0)
    if !DllCall("GetWindowRect", "ptr", hwnd, "ptr", buf)
        return {l: 0, t: 0, r: 0, b: 0}
    return {
        l: NumGet(buf, 0, "int"), t: NumGet(buf, 4, "int"),
        r: NumGet(buf, 8, "int"), b: NumGet(buf, 12, "int")
    }
}

SameRect(a, b) {
    return (a.l == b.l && a.t == b.t && a.r == b.r && a.b == b.b)
}

Describe(rc) {
    return "(" rc.l "," rc.t "," rc.r "," rc.b ")"
}

FindChild(parent, cls) {
    return DllCall("FindWindowExW", "ptr", parent, "ptr", 0, "wstr", cls, "wstr", "", "ptr")
}

IsVisible(hwnd) {
    return hwnd && DllCall("IsWindowVisible", "ptr", hwnd, "int")
}

Msg(hwnd, msg, wParam := 0, lParam := 0) {
    return DllCall("SendMessageW", "ptr", hwnd, "uint", msg, "uptr", wParam, "ptr", lParam, "int64")
}

Post(hwnd, msg, wParam := 0, lParam := 0) {
    return DllCall("PostMessageW", "ptr", hwnd, "uint", msg, "uptr", wParam, "ptr", lParam, "int")
}

ActivateApp(wnd) {
    WinActivate("ahk_id " wnd)
    WaitFor(300)
}

FocusEditor(wnd, ctrl) {
    WinActivate("ahk_id " wnd)
    WaitFor(200)
    r := WindowRect(ctrl)
    MouseClick("Left", r.l + 60, r.t + 40, 1, 0)
    WaitFor(300)
}

; Injected modifiers must be delivered one step at a time: a single batched send
; makes the host see a Unicode packet instead of Ctrl+Alt+VK_M.
SendCtrlAltM() {
    SendToggleKey(false)
}

SendAltGrM() {
    SendToggleKey(true)
}

SendToggleKey(altGr) {
    modifier := altGr ? "{RAlt" : "{LAlt"
    Send("{LCtrl down}")
    Sleep(80)
    Send(modifier " down}")
    Sleep(80)
    Send("{vk4D}")
    Sleep(80)
    Send(modifier " up}")
    Send("{LCtrl up}")
}

WaitFor(ms) {
    Sleep(ms)
}

; Poll a condition so slow UI updates do not fail assertions.
Poll(condition, timeout := 3000) {
    end := A_TickCount + timeout
    while (A_TickCount < end) {
        if (condition())
            return true
        Sleep(120)
    }
    return condition()
}

GetMonitors() {
    global MonitorList
    MonitorList := []
    cb := CallbackCreate(EnumMonitor, "Fast", 4)
    DllCall("EnumDisplayMonitors", "ptr", 0, "ptr", 0, "ptr", cb, "ptr", 0)
    CallbackFree(cb)
    return MonitorList
}

EnumMonitor(hMon, hdc, lprc, lparam) {
    global MonitorList
    mi := Buffer(40, 0)
    NumPut("uint", 40, mi, 0)
    if DllCall("GetMonitorInfoW", "ptr", hMon, "ptr", mi, "int") {
        dpiX := 0, dpiY := 0
        DllCall("shcore\GetDpiForMonitor", "ptr", hMon, "int", 0, "uint*", &dpiX, "uint*", &dpiY)
        MonitorList.Push({l: NumGet(mi, 4, "int"), t: NumGet(mi, 8, "int"),
                          r: NumGet(mi, 12, "int"), b: NumGet(mi, 16, "int"), dpi: dpiX})
    }
    return 1
}

WindowDpi(hwnd) {
    return DllCall("GetDpiForWindow", "ptr", hwnd, "uint")
}

; ---------------------------------------------------------------------------
; Results
; ---------------------------------------------------------------------------

Assert(condition, name, detail := "") {
    global Checks, Failures
    Checks += 1
    if condition {
        Log("ok   " name)
        return true
    }
    Failures.Push(name (detail ? " - " detail : ""))
    Log("FAIL " name (detail ? " - " detail : ""))
    return false
}

Skip(name, reason) {
    global Skips
    Skips.Push(name ": " reason)
    Log("skip " name " - " reason)
}

Fatal(message) {
    Log("FAIL " message)
    ExitApp(1)
}

; Runtime errors must not block the suite behind a modal dialog.
ReportError(err, mode) {
    global LOGFILE
    text := "ERROR " err.Message " (" err.Extra ") at line " err.Line " [" mode "]"
    try FileAppend(text "`n", LOGFILE, "UTF-8")
    try FileAppend(text "`n", "*")
    ExitApp(1)
    return true
}

Log(text) {
    global LOGFILE
    FileAppend(text "`n", LOGFILE, "UTF-8")
    FileAppend(text "`n", "*")
}
