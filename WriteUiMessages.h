#pragma once
#include <windows.h>
#include <afxstr.h>

// private WM_APP messages used between worker thread and dialog
#ifndef WM_APP_WRITE_UI_UPDATE
#define WM_APP_WRITE_UI_UPDATE   (WM_APP + 101)
#endif

#ifndef WM_APP_WRITE_QUERY_YESNO
#define WM_APP_WRITE_QUERY_YESNO (WM_APP + 102)
#endif

struct WriteUiUpdate
{
    bool hasMessage = false;
    CString message;

    bool hasPercent = false;
    CString percent;

    bool hasRawFlag = false;
    CString rawFlag;

    bool hasProgress = false;
    int progress = 0; // 0..100

    bool hasLog = false;
    int logLevel = 0;
    CString logText;

    bool requestClose = false;
    bool requestAutoSave = false;
};

struct WriteUiQueryYesNo
{
    CString text;
    CString caption;
    UINT flags = MB_YESNO;
};
