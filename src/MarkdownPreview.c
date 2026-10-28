// encoding: UTF-8
/******************************************************************************
*                                                                             *
* Notepad3                                                                    *
*                                                                             *
* MarkdownPreview.c                                                           *
*   Host adapter for the optional Markdown preview DLL                        *
*                                                                             *
*                                                  (c) Rizonesoft 2008-2026   *
*                                                    https://rizonesoft.com   *
*                                                                             *
*******************************************************************************/

#include "Helpers.h"
#include <stdlib.h>
#include <shellapi.h>
#include <shlwapi.h>

#include "PathLib.h"
#include "Dialogs.h"
#include "MuiLanguage.h"
#include "Notepad3.h"
#include "DarkMode/DarkMode.h"
#include "MarkdownPreview.h"

#define NP3_PREVIEW_MAX_SOURCE_BYTES (16 * 1024 * 1024)
#define NP3_PREVIEW_RUNTIME_URL L"https://go.microsoft.com/fwlink/p/?LinkId=2124703"

static inline PMARKDOWNPREVIEW_STATE_T _State(void)
{
    return &Globals.MarkdownPreview;
}

static inline bool _IsBusy(void)
{
    PMARKDOWNPREVIEW_STATE_T const st = _State();
    return (st->bActive || st->bLoading);
}

static bool _IsSameText(LPCWSTR pszText, LPCWSTR pszFind)
{
    return (pszText && pszFind && (StrCmpIW(pszText, pszFind) == 0));
}

static void _SourceRect(HWND hwnd, RECT* prc)
{
    HWND const hwndSource = MarkdownPreview_GetSourceHwnd();
    if (!IsWindow(hwndSource)) {
        SetRectEmpty(prc);
        return;
    }
    GetWindowRect(hwndSource, prc);
    MapWindowPoints(HWND_DESKTOP, hwnd, (LPPOINT)prc, 2);
}

static void _ShowSourceWindow(bool bShow)
{
    HWND const hwndSource = MarkdownPreview_GetSourceHwnd();
    if (IsWindow(hwndSource)) {
        ShowWindow(hwndSource, bShow ? SW_SHOW : SW_HIDE);
    }
}

static void _ShowError(HWND hwnd, UINT uId)
{
    InfoBoxLng(MB_OK | MB_ICONERROR, NULL, uId);
    UNREFERENCED_PARAMETER(hwnd);
}

static bool _LoadApi(void)
{
    PMARKDOWNPREVIEW_STATE_T const st = _State();

    if (st->hModule) {
        return true;
    }
    if (st->bUnavailable) {
        return false;
    }

    HPATHL const hpth = Path_Copy(Paths.ModuleDirectory);
    Path_Append(hpth, L"MarkdownPreview.dll");

    // Never search the current working directory for the optional component.
    HMODULE const hmod = LoadLibraryExW(Path_Get(hpth), NULL,
                                        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    Path_Release(hpth);

    if (hmod) {
        NP3PreviewApi api;
        ZeroMemory(&api, sizeof(api));
        NP3PreviewGetApi const pGetApi = (NP3PreviewGetApi)GetProcAddress(hmod, "NP3Preview_GetApi");
        if (pGetApi && SUCCEEDED(pGetApi(NP3_PREVIEW_ABI, sizeof(api), &api)) &&
            api.Begin && api.Close && api.Resize) {
            st->hModule = hmod;
            st->Api = api;
            return true;
        }
    }

    st->bUnavailable = true;
    return false;
}

bool MarkdownPreview_IsEligible(const HPATHL hpth)
{
    if (!hpth || Path_IsEmpty(hpth)) {
        return false;
    }
    LPCWSTR const ext = Path_FindExtension(hpth);
    return _IsSameText(ext, L".md");
}

bool MarkdownPreview_IsActive(void)
{
    return _State()->bActive;
}

bool MarkdownPreview_IsBusy(void)
{
    return _IsBusy();
}

static void _RestoreSourceSelection(void)
{
    PMARKDOWNPREVIEW_STATE_T const st = _State();
    SciCall_SetSel(st->posAnchor, st->posCaret);
    SciCall_SetFirstVisibleLine(st->iFirstVisibleLine);
}

void MarkdownPreview_ReturnToSource(bool bRestoreSelection)
{
    PMARKDOWNPREVIEW_STATE_T const st = _State();

    bool const bWasBusy = _IsBusy();

    // Invalidate pending callbacks before the DLL can deliver them.
    ++st->uGeneration;

    if (st->hPreview && st->Api.Close) {
        st->Api.Close(st->hPreview);
    }
    st->hPreview = NULL;
    st->bActive = false;
    st->bLoading = false;

    _ShowSourceWindow(true);

    if (bWasBusy && bRestoreSelection) {
        _RestoreSourceSelection();
    }
    if (IsWindow(Globals.hwndEdit)) {
        SetFocus(Globals.hwndEdit);
    }
    UpdateToolbar();
}

static void _NavigateToSource(unsigned long long uSourceOffset)
{
    DocPos const iDocLength = SciCall_GetTextLength();
    DocPos const iPos = (uSourceOffset > (unsigned long long)iDocLength) ? iDocLength : (DocPos)uSourceOffset;

    MarkdownPreview_ReturnToSource(false);

    SciCall_SetSel(iPos, iPos);
    DocLn const iLine = SciCall_LineFromPosition(iPos);
    SciCall_EnsureVisible(iLine);
    SciCall_ScrollRange(iPos, iPos);
    SetFocus(Globals.hwndEdit);
}

static void __cdecl _OnPreviewEvent(void* pContext, const NP3PreviewEvent* pEvent)
{
    UNREFERENCED_PARAMETER(pContext);

    if (!pEvent || (pEvent->cbSize != sizeof(NP3PreviewEvent))) {
        return;
    }
    NP3PreviewEvent* const pCopy = (NP3PreviewEvent*)malloc(sizeof(NP3PreviewEvent));
    if (!pCopy) {
        return;
    }
    CopyMemory(pCopy, pEvent, sizeof(NP3PreviewEvent));

    // Post a private message with the copied POD payload: the DLL callback must
    // not close the controller while it is still on the stack.
    if (!PostMessage(Globals.hwndMain, WM_MARKDOWNPREVIEW_EVENT, 0, (LPARAM)pCopy)) {
        free(pCopy);
    }
}

static void _BeginPreview(HWND hwnd)
{
    PMARKDOWNPREVIEW_STATE_T const st = _State();

    if (!MarkdownPreview_IsEligible(Paths.CurrentFile)) {
        SimpleBeep();
        return;
    }
    if (!_LoadApi()) {
        _ShowError(hwnd, IDS_MUI_ERROR_MARKDOWN_PREVIEW_DLL);
        return;
    }

    DocPos const iDocLength = SciCall_GetTextLength();
    if (iDocLength > (DocPos)NP3_PREVIEW_MAX_SOURCE_BYTES) {
        _ShowError(hwnd, IDS_MUI_ERROR_MARKDOWN_PREVIEW_SIZE);
        return;
    }

    char* const pchSnapshot = (char*)calloc((size_t)iDocLength + 1, sizeof(char));
    if (!pchSnapshot) {
        return;
    }
    struct Sci_TextRangeFull tr;
    tr.chrg.cpMin = 0;
    tr.chrg.cpMax = iDocLength;
    tr.lpstrText = pchSnapshot;
    SciCall_GetTextRangeFull(&tr);

    st->posCaret = SciCall_GetCurrentPos();
    st->posAnchor = SciCall_GetAnchor();
    st->iFirstVisibleLine = SciCall_GetFirstVisibleLine();

    WCHAR tchCopy[MIDSZ_BUFFER] = { L'\0' };
    WCHAR tchOpenLink[MIDSZ_BUFFER] = { L'\0' };
    GetLngString(IDS_MUI_MARKDOWN_PREVIEW_COPY, tchCopy, COUNTOF(tchCopy));
    GetLngString(IDS_MUI_MARKDOWN_PREVIEW_OPENLINK, tchOpenLink, COUNTOF(tchOpenLink));

    NP3PreviewRequest request;
    ZeroMemory(&request, sizeof(request));
    request.cbSize = sizeof(request);
    request.parent = hwnd;
    request.generation = ++st->uGeneration;
    request.utf8 = pchSnapshot;
    request.byteCount = (unsigned long long)iDocLength;
    request.sourcePath = Path_Get(Paths.CurrentFile);
    _SourceRect(hwnd, &request.bounds);
    request.dark = UseDarkMode() ? TRUE : FALSE;
    request.callback = _OnPreviewEvent;
    request.context = NULL;
    request.copyLabel = tchCopy;
    request.openLinkLabel = tchOpenLink;

    NP3PreviewHandle hPreview = NULL;
    HRESULT const hr = st->Api.Begin(&request, &hPreview);

    free(pchSnapshot);

    if (FAILED(hr) || !hPreview) {
        _ShowError(hwnd, IDS_MUI_ERROR_MARKDOWN_PREVIEW_DLL);
        return;
    }
    st->hPreview = hPreview;
    st->bLoading = true;
    UpdateToolbar();
}

void MarkdownPreview_Toggle(HWND hwnd)
{
    if (_IsBusy()) {
        MarkdownPreview_ReturnToSource(true);
        return;
    }
    _BeginPreview(hwnd);
}

void MarkdownPreview_Resize(void)
{
    PMARKDOWNPREVIEW_STATE_T const st = _State();

    if (!st->hPreview || !st->Api.Resize) {
        return;
    }
    RECT rc;
    _SourceRect(Globals.hwndMain, &rc);
    if (EqualRect(&rc, &st->rcBounds)) {
        return;
    }
    st->rcBounds = rc;
    st->Api.Resize(st->hPreview, &rc);
}

void MarkdownPreview_UpdateTheme(void)
{
    PMARKDOWNPREVIEW_STATE_T const st = _State();

    if (st->hPreview && st->Api.SetDark) {
        st->Api.SetDark(st->hPreview, UseDarkMode() ? TRUE : FALSE);
    }
}

LRESULT MarkdownPreview_HandleEvent(LPARAM lParam)
{
    PMARKDOWNPREVIEW_STATE_T const st = _State();

    NP3PreviewEvent* const pCopy = (NP3PreviewEvent*)lParam;
    if (!pCopy) {
        return FALSE;
    }
    NP3PreviewEvent const event = *pCopy;
    free(pCopy);


    if (!st->hPreview || (event.generation != st->uGeneration)) {
        return FALSE; // stale event from a cancelled or replaced snapshot
    }

    switch (event.kind) {

    case NP3PreviewReady:
        st->bLoading = false;
        st->bActive = true;
        _ShowSourceWindow(false);
        if (st->Api.Focus) {
            st->Api.Focus(st->hPreview);
        }
        UpdateToolbar();
        return TRUE;

    case NP3PreviewNavigateSource:
        _NavigateToSource(event.sourceOffset);
        return TRUE;

    case NP3PreviewToggleSource:
        MarkdownPreview_ReturnToSource(true);
        return TRUE;

    case NP3PreviewError: {
        bool const bRuntimeMissing = (event.result == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
        bool const bTimedOut = (event.result == HRESULT_FROM_WIN32(ERROR_TIMEOUT));
        MarkdownPreview_ReturnToSource(true);
        if (bRuntimeMissing) {
            if (IsYesOkay(InfoBoxLng(MB_YESNO | MB_ICONWARNING, NULL, IDS_MUI_ERROR_MARKDOWN_PREVIEW_RUNTIME))) {
                ShellExecuteW(Globals.hwndMain, L"open", NP3_PREVIEW_RUNTIME_URL, NULL, NULL, SW_SHOWNORMAL);
            }
        } else if (bTimedOut) {
            InfoBoxLng(MB_OK | MB_ICONERROR, NULL, IDS_MUI_ERROR_MARKDOWN_PREVIEW_TIMEOUT);
        } else {
            InfoBoxLng(MB_OK | MB_ICONERROR, NULL, IDS_MUI_ERROR_MARKDOWN_PREVIEW_RENDER);
        }
        return TRUE;
    }

    default:
        break;
    }
    return FALSE;
}

void MarkdownPreview_OnDocumentChanged(void)
{
    if (_IsBusy()) {
        // Editing cancels loading and invalidates mapping for a shown preview.
        MarkdownPreview_ReturnToSource(false);
    }
}

void MarkdownPreview_BeforeDocumentOperation(void)
{
    if (_IsBusy()) {
        MarkdownPreview_ReturnToSource(true);
    }
}

static bool _IsSourceEditingCommand(UINT uCmd)
{
    return ((uCmd >= IDM_EDIT_BOOKMARKTOGGLE && uCmd <= BME_EDIT_BOOKMARKPREV) ||
            (uCmd >= IDM_EDIT_UNDO && uCmd <= IDM_EDIT_LAST) ||
            (uCmd == IDM_EDIT_INSERT_GUID) ||
            (uCmd == IDM_EDIT_TOGGLE_PASTEBOARD) ||
            (uCmd >= IDM_ENCODING_ANSI && uCmd <= IDM_LINEENDINGS_SETDEFAULT));
}

static bool _IsPreviewNeutralCommand(UINT uCmd)
{
    switch (uCmd) {
    case IDM_VIEW_MARKDOWN_PREVIEW:
    case IDT_VIEW_MARKDOWN_PREVIEW:
    case IDM_VIEW_TOOLBAR:
    case IDM_VIEW_STATUSBAR:
    case IDM_VIEW_MENUBAR:
    case IDM_VIEW_TITLEBAR:
    case IDM_SET_ALWAYSONTOP:
    case IDM_HELP_ABOUT:
    case IDM_HELP_ONLINEDOCUMENTATION:
        return true;
    default:
        return false;
    }
}

bool MarkdownPreview_FilterCommand(UINT uCmd)
{
    PMARKDOWNPREVIEW_STATE_T const st = _State();

    if (!_IsBusy()) {
        return false;
    }

    if (_IsPreviewNeutralCommand(uCmd)) {
        return false;
    }

    // Timers, toolbar ids and control ids are not document commands.
    if (uCmd < 20000) {
        return false;
    }

    if ((uCmd == IDM_EDIT_COPY) && st->bActive && st->hPreview && st->Api.Copy) {
        st->Api.Copy(st->hPreview);
        return true;
    }


    // Every other command acts on the source document: reveal it first so no
    // command can mutate or reinterpret a hidden buffer.
    if (_IsSourceEditingCommand(uCmd)) {
        SimpleBeep();
        return true;
    }
    MarkdownPreview_ReturnToSource(true);
    return false;
}

void MarkdownPreview_UpdateMenuState(HMENU hmenu)
{
    PMARKDOWNPREVIEW_STATE_T const st = _State();

    bool const bEligible = MarkdownPreview_IsEligible(Paths.CurrentFile);
    EnableCmd(hmenu, IDM_VIEW_MARKDOWN_PREVIEW, bEligible || _IsBusy());
    CheckCmd(hmenu, IDM_VIEW_MARKDOWN_PREVIEW, _IsBusy());

    if (_IsBusy()) {
        for (UINT uCmd = IDM_EDIT_UNDO; uCmd <= IDM_EDIT_LAST; ++uCmd) {
            EnableCmd(hmenu, uCmd, false);
        }
        for (UINT uCmd = IDM_EDIT_BOOKMARKTOGGLE; uCmd <= BME_EDIT_BOOKMARKPREV; ++uCmd) {
            EnableCmd(hmenu, uCmd, false);
        }
        for (UINT uCmd = IDM_ENCODING_ANSI; uCmd <= IDM_LINEENDINGS_SETDEFAULT; ++uCmd) {
            EnableCmd(hmenu, uCmd, false);
        }
        EnableCmd(hmenu, IDM_EDIT_COPY, true);
    }
    UNREFERENCED_PARAMETER(st);
}

void MarkdownPreview_UpdateToolbarButton(void)
{
    HWND const hwndToolbar = (HWND)Globals.hwndToolbar;
    if (!IsWindow(hwndToolbar) || !Settings.ShowToolbar) {
        return;
    }

    bool const bWanted = MarkdownPreview_IsEligible(Paths.CurrentFile);
    bool const bSourceLabel = _IsBusy();

    int iIndex = (int)SendMessage(hwndToolbar, TB_COMMANDTOINDEX, (WPARAM)IDT_VIEW_MARKDOWN_PREVIEW, 0);

    if (!bWanted) {
        if (iIndex >= 0) {
            SendMessage(hwndToolbar, TB_DELETEBUTTON, (WPARAM)iIndex, 0);
        }
        return;
    }

    WCHAR tchLabel[SMALL_BUFFER] = { L'\0' };
    if (!GetLngString(bSourceLabel ? IDS_MUI_MARKDOWN_PREVIEW_SOURCE : IDT_VIEW_MARKDOWN_PREVIEW,
                      tchLabel, COUNTOF(tchLabel))) {
        return;
    }

    if (iIndex >= 0) {
        TBBUTTONINFO tbi;
        ZeroMemory(&tbi, sizeof(tbi));
        tbi.cbSize = sizeof(tbi);
        tbi.dwMask = TBIF_TEXT;
        tbi.pszText = tchLabel;
        if (SendMessage(hwndToolbar, TB_SETBUTTONINFO, (WPARAM)IDT_VIEW_MARKDOWN_PREVIEW, (LPARAM)&tbi) != FALSE) {
            return;
        }
        SendMessage(hwndToolbar, TB_DELETEBUTTON, (WPARAM)iIndex, 0);
    }

    TBBUTTON tbb;
    ZeroMemory(&tbb, sizeof(tbb));
    tbb.iBitmap = I_IMAGENONE;
    tbb.idCommand = IDT_VIEW_MARKDOWN_PREVIEW;
    tbb.fsState = TBSTATE_ENABLED;
    tbb.fsStyle = BTNS_BUTTON | BTNS_AUTOSIZE | BTNS_SHOWTEXT;
    tbb.iString = SendMessage(hwndToolbar, TB_ADDSTRINGW, 0, (LPARAM)tchLabel);

    SendMessage(hwndToolbar, TB_INSERTBUTTONW, (WPARAM)SendMessage(hwndToolbar, TB_BUTTONCOUNT, 0, 0),
                (LPARAM)&tbb);
}
