#pragma once

#include <atomic>
#include <windows.h>
#include <afxstr.h>
#include <afxcmn.h>

#include "logwindow.h"
#include "cdwriter.h"
#include "DirStructure.h"
#include "SubcodeGeneratorMS.h"
#include "CDController.h"

class CWriteThread
{
public:
    CWriteThread(void);
    ~CWriteThread(void);

    CWriteThread(const CWriteThread&) = delete;
    CWriteThread& operator=(const CWriteThread&) = delete;

    // inputs (set by UI before StartThread)
    CLogWindow* m_LogWnd = nullptr;
    CCDController* m_CD = nullptr;
    CString          m_CueFileName;

    // ✅ UI thread handle only (worker thread posts to HWND)
    HWND             m_hParentWnd = nullptr;

    // mastering mode inputs
    CDirStructure* m_Dir = nullptr;
    CListCtrl* m_List = nullptr;
    CString          m_VolumeLabel;
    DWORD            m_TotalFrames = 0;

    // state/results
    std::atomic_bool m_StopFlag{ false };
    bool             m_Success = false;

public:
    void  StartThread(void);
    void  StopThread(void);

    DWORD ThreadFunction(void);

    DWORD WriteImage(void);
    DWORD WriteImageSubSS(void);
    DWORD WriteImageSubMS(void);

    DWORD Mastering(void);
    DWORD MasteringSub(void);

    bool  CreateCueSheet(CString& CueSheet);
    bool  SkipAudioHeader(HANDLE hFile);
    int   DetectCommand(void);

    CSubcodeGeneratorMS m_SubMS;

private:
    HANDLE m_hThread = nullptr;
    DWORD  m_ThreadID = 0;
    bool   m_ModeMS = false;
};
