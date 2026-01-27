#pragma once
#include <atomic>
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
    CLogWindow* m_LogWnd;
    CCDController* m_CD;
    CString m_CueFileName;
    CWnd* m_ParentWnd;
    CDirStructure* m_Dir;
    CListCtrl* m_List;
    CString m_VolumeLabel;
    DWORD m_TotalFrames;

protected:
    HANDLE m_hThread = nullptr;
    DWORD m_ThreadID = 0;
    bool m_ModeMS = false;

public:
    std::atomic_bool m_StopFlag{ false };
    void StartThread(void);
    void StopThread(void);
    DWORD ThreadFunction(void);
    DWORD WriteImage(void);
    DWORD WriteImageSubSS(void);
    DWORD WriteImageSubMS(void);
    bool m_Success;
    DWORD Mastering(void);
    bool CreateCueSheet(CString& CueSheet);
    bool SkipAudioHeader(HANDLE hFile);
    DWORD MasteringSub(void);
    int DetectCommand(void);
    CSubcodeGeneratorMS m_SubMS;
};
