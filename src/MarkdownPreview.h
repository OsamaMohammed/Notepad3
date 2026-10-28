// encoding: UTF-8
/******************************************************************************
*                                                                             *
* Notepad3                                                                    *
*                                                                             *
* MarkdownPreview.h                                                           *
*   Host adapter for the optional Markdown preview DLL                        *
*                                                                             *
*                                                  (c) Rizonesoft 2008-2026   *
*                                                    https://rizonesoft.com   *
*                                                                             *
*******************************************************************************/

#pragma once
#ifndef _NP3_MARKDOWNPREVIEW_H_
#define _NP3_MARKDOWNPREVIEW_H_

#include "TypeDefs.h"

// Posted by the DLL event callback with a heap copy of NP3PreviewEvent in lParam.
#define WM_MARKDOWNPREVIEW_EVENT (WM_APP + 0x4D50)

bool MarkdownPreview_IsEligible(const HPATHL hpth);
bool MarkdownPreview_IsActive(void);
bool MarkdownPreview_IsBusy(void);
void MarkdownPreview_Toggle(HWND hwnd);
void MarkdownPreview_ReturnToSource(bool bRestoreSelection);
void MarkdownPreview_Resize(void);
void MarkdownPreview_UpdateTheme(void);
LRESULT MarkdownPreview_HandleEvent(LPARAM lParam);
void MarkdownPreview_OnDocumentChanged(void);
void MarkdownPreview_UpdateToolbarButton(void);
void MarkdownPreview_UpdateMenuState(HMENU hmenu);
bool MarkdownPreview_FilterCommand(UINT uCmd);
void MarkdownPreview_BeforeDocumentOperation(void);

// Implemented by the host: the window that owns the visible source editor.
HWND MarkdownPreview_GetSourceHwnd(void);

#endif // _NP3_MARKDOWNPREVIEW_H_
