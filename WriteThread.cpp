// writethread.cpp  (UI-thread safe worker + robustness fixes)

#include "stdafx.h"
#include "writethread.h"

#include "Setting.h"
#include "IsoCreator.h"
#include "CheckSector.h"

#include "WriteUiMessages.h" // ✅ shared payloads + WM_APP ids

#include <process.h>
#include <cstring>

// ---- small IO helpers -------------------------------------------------------

static inline bool ReadExact(HANDLE h, void* buf, DWORD bytes)
{
    DWORD read = 0;
    if (!ReadFile(h, buf, bytes, &read, nullptr))
        return false;
    return read == bytes;
}

static inline bool ReadAtMost(HANDLE h, void* buf, DWORD bytes, DWORD& outRead)
{
    outRead = 0;
    if (!ReadFile(h, buf, bytes, &outRead, nullptr))
        return false;
    return true;
}

static inline bool GetFileSize32(HANDLE h, DWORD& outSize)
{
    outSize = GetFileSize(h, nullptr);
    if (outSize == INVALID_FILE_SIZE)
    {
        DWORD err = GetLastError();
        if (err != NO_ERROR)
            return false;
    }
    return true;
}

// ---- UI marshaling (worker -> dialog HWND) ----------------------------------

static inline HWND SafeHwnd(HWND h)
{
    return (h && ::IsWindow(h)) ? h : nullptr;
}

static void PostUi(HWND hDlg, WriteUiUpdate* up)
{
    HWND h = SafeHwnd(hDlg);
    if (!h) { delete up; return; }
    ::PostMessage(h, WM_APP_WRITE_UI_UPDATE, 0, (LPARAM)up);
}

static void UiMessage(CWriteThread* t, const CString& s)
{
    auto* up = new WriteUiUpdate();
    up->hasMessage = true;
    up->message = s;
    PostUi(t ? t->m_hParentWnd : nullptr, up);
}

static void UiProgress(CWriteThread* t, int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    auto* up = new WriteUiUpdate();
    up->hasProgress = true;
    up->progress = pct;
    up->hasPercent = true;
    up->percent.Format(_T("%d%%"), pct);
    PostUi(t ? t->m_hParentWnd : nullptr, up);
}

static void UiRawFlag(CWriteThread* t, const CString& s)
{
    auto* up = new WriteUiUpdate();
    up->hasRawFlag = true;
    up->rawFlag = s;
    PostUi(t ? t->m_hParentWnd : nullptr, up);
}

static void UiLog(CWriteThread* t, int level, const CString& s)
{
    auto* up = new WriteUiUpdate();
    up->hasLog = true;
    up->logLevel = level;
    up->logText = s;
    PostUi(t ? t->m_hParentWnd : nullptr, up);
}

static void UiAutoSave(CWriteThread* t)
{
    auto* up = new WriteUiUpdate();
    up->requestAutoSave = true;
    PostUi(t ? t->m_hParentWnd : nullptr, up);
}

static void UiClose(CWriteThread* t)
{
    auto* up = new WriteUiUpdate();
    up->requestClose = true;
    PostUi(t ? t->m_hParentWnd : nullptr, up);
}

static int UiAskYesNo(CWriteThread* t, const CString& text, const CString& caption, UINT flags)
{
    HWND h = SafeHwnd(t ? t->m_hParentWnd : nullptr);
    if (!h) return IDNO;

    WriteUiQueryYesNo q;
    q.text = text;
    q.caption = caption;
    q.flags = flags;

    // UI thread shows prompt and returns IDYES/IDNO
    return (int)::SendMessage(h, WM_APP_WRITE_QUERY_YESNO, 0, (LPARAM)&q);
}

// ---- thread proc ------------------------------------------------------------

static unsigned __stdcall WriteThreadProc(void* p)
{
    return static_cast<unsigned>(static_cast<CWriteThread*>(p)->ThreadFunction());
}

// ---- CWriteThread -----------------------------------------------------------

CWriteThread::CWriteThread(void)
    : m_CueFileName(_T(""))
{
    m_StopFlag.store(false, std::memory_order_relaxed);
}

CWriteThread::~CWriteThread(void)
{
    StopThread();
}

void CWriteThread::StartThread(void)
{
    StopThread();
    m_StopFlag.store(false, std::memory_order_release);

    uintptr_t th = _beginthreadex(nullptr, 0, &WriteThreadProc, this, 0, (unsigned*)&m_ThreadID);
    m_hThread = th ? reinterpret_cast<HANDLE>(th) : nullptr;
    if (!m_hThread)
        return;

    SetThreadPriority(m_hThread, THREAD_PRIORITY_HIGHEST);
}

void CWriteThread::StopThread(void)
{
    if (!m_hThread)
        return;

    m_StopFlag.store(true, std::memory_order_release);

    DWORD exitCode = 0;
    if (GetExitCodeThread(m_hThread, &exitCode) && exitCode == STILL_ACTIVE)
        WaitForSingleObject(m_hThread, INFINITE);

    CloseHandle(m_hThread);
    m_hThread = nullptr;
    m_ThreadID = 0;
}

DWORD CWriteThread::ThreadFunction(void)
{
    DWORD ret = 0;

    UiProgress(this, 0);

    if (m_Dir != nullptr && m_List != nullptr)
    {
        ret = Mastering();
    }
    else
    {
        CString ext = m_CueFileName.Right(3);
        ext.MakeLower();

        if (ext == _T("cue") || ext == _T("iso"))
            m_ModeMS = false;
        else
            m_ModeMS = true;

        ret = WriteImage();
    }

    m_Success = false;

    if (!m_StopFlag.load(std::memory_order_acquire))
    {
        UiProgress(this, 100);

        if (ret)
        {
            UiMessage(this, MSG(23));
            UiLog(this, LOG_NORMAL, MSG(23));
            m_Success = true;
        }
        else
        {
            UiLog(this, LOG_ERROR, MSG(18));
        }
    }

    UiAutoSave(this);
    UiClose(this);
    return ret;
}

// ----------------------------------------------------------------------------
// WriteImage (Single-session or Multi-session image write)
// ----------------------------------------------------------------------------

DWORD CWriteThread::WriteImage(void)
{
    CString cs;

    UiLog(this, LOG_NORMAL, MSG(24));
    UiMessage(this, MSG(24));

    if (m_ModeMS)
    {
        cs.Format(_T("%s : Multi-Session"), MSG(137));
        UiLog(this, LOG_NORMAL, cs);
    }
    else
    {
        cs.Format(_T("%s : Single-Session"), MSG(137));
        UiLog(this, LOG_NORMAL, cs);
    }

    if (theSetting.m_Write_BurnProof)
        UiLog(this, LOG_NORMAL, MSG(25));
    if (theSetting.m_Write_TestMode)
        UiLog(this, LOG_NORMAL, MSG(26));

    // parse cue sheet
    cs.Format(MSG(27), m_CueFileName.GetString());
    UiMessage(this, cs);
    UiLog(this, LOG_INFO, cs);

    if (!m_CD)
        return 0;

    if (m_ModeMS)
    {
        if (!m_SubMS.ParseFile(m_CueFileName))
        {
            UiMessage(this, m_SubMS.GetErrorMessage());
            UiLog(this, LOG_ERROR, m_SubMS.GetErrorMessage());
            return 0;
        }

        CString tmp;
        tmp.Format(_T("%s/%s"), m_SubMS.m_ImgFileName.GetString(), m_SubMS.m_SubFileName.GetString());
        cs.Format(MSG(28), tmp.GetString());
        UiLog(this, LOG_INFO, cs);
    }
    else
    {
        CString ext = m_CueFileName.Right(3);
        ext.MakeLower();

        if (ext == _T("iso"))
        {
            DWORD FileSize = 0;
            DWORD ReadSize = 0;
            HANDLE hFile = INVALID_HANDLE_VALUE;
            CString cueSheet;
            BYTE ReadBuffer[16] = {};
            BYTE Header1[16] = { 0x00,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x00,0x00,0x02,0x00,0x01 };
            BYTE Header2[16] = { 0x00,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x00,0x00,0x02,0x00,0x02 };

            hFile = CreateFile(m_CueFileName, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hFile == INVALID_HANDLE_VALUE)
                return 0;

            if (!GetFileSize32(hFile, FileSize))
            {
                CloseHandle(hFile);
                return 0;
            }

            if (!ReadAtMost(hFile, ReadBuffer, 16, ReadSize) || ReadSize != 16)
            {
                CloseHandle(hFile);
                return 0;
            }

            CloseHandle(hFile);

            if (memcmp(Header1, ReadBuffer, 16) == 0)
                cueSheet = _T("  TRACK 1 MODE1/2352\n   INDEX 1 00:00:00\n");
            else if (memcmp(Header2, ReadBuffer, 16) == 0)
                cueSheet = _T("  TRACK 1 MODE2/2352\n   INDEX 1 00:00:00\n");
            else
                cueSheet = _T("  TRACK 1 MODE1/2048\n   INDEX 1 00:00:00\n");

            if (!m_CD->ParseCueSheet(cueSheet, FileSize))
            {
                UiMessage(this, m_CD->GetWriteError());
                UiLog(this, LOG_ERROR, m_CD->GetWriteError());
                return 0;
            }
        }
        else
        {
            if (!m_CD->ParseCueSheetFile(m_CueFileName))
            {
                UiMessage(this, m_CD->GetWriteError());
                UiLog(this, LOG_ERROR, m_CD->GetWriteError());
                return 0;
            }
        }

        cs.Format(MSG(28), m_CD->GetImageFileName());
        UiLog(this, LOG_INFO, cs);
    }

    if (m_StopFlag.load(std::memory_order_acquire))
        return 0;

    // check drive
    if (theSetting.m_Write_CheckDrive)
    {
        if (!m_CD->IsCDR())
        {
            cs.Format(MSG(29), m_CD->GetWriteError());
            UiMessage(this, cs);
            UiLog(this, LOG_ERROR, cs);
            return 0;
        }
    }

    while (!m_CD->CheckDisc())
    {
        if (UiAskYesNo(this, MSG(30), CONF_MSG, MB_YESNO) == IDNO)
        {
            UiMessage(this, MSG(31));
            UiLog(this, LOG_ERROR, MSG(31));
            return 0;
        }

        if (m_StopFlag.load(std::memory_order_acquire))
            return 0;
    }

    // set params
    {
        const bool BurnProof = (theSetting.m_Write_BurnProof != 0);
        const bool TestMode = (theSetting.m_Write_TestMode != 0);
        const int  WriteMode = DetectCommand();

        if (WriteMode < 0)
        {
            cs.Format(MSG(32), m_CD->GetWriteError());
            UiMessage(this, cs);
            UiLog(this, LOG_ERROR, cs);
            return 0;
        }

        if (m_ModeMS)
        {
            if (WriteMode != WRITEMODE_RAW_96 && WriteMode != WRITEMODE_RAW_P96 && WriteMode != WRITEMODE_RAW_16)
            {
                UiMessage(this, MSG(138));
                UiLog(this, LOG_ERROR, MSG(138));
                return 0;
            }
        }

        if (!m_CD->SetWritingParams(WriteMode, BurnProof, TestMode, theSetting.m_Write_Buffer))
        {
            cs.Format(MSG(33), m_CD->GetWriteError());
            UiMessage(this, cs);
            UiLog(this, LOG_ERROR, cs);
            return 0;
        }
    }

    cs.Format(MSG(34), m_CD->GetBufferSize());
    UiLog(this, LOG_INFO, cs);

    // set writing speed
    m_CD->SetSpeed(0xff, theSetting.m_Write_Speed);

    // OPC
    if (theSetting.m_Write_Opc)
    {
        UiMessage(this, MSG(35));
        UiLog(this, LOG_INFO, MSG(35));

        if (!m_CD->OPC())
        {
            UiMessage(this, MSG(36));
            UiLog(this, LOG_ERROR, MSG(36));
            return 0;
        }
    }

    // start writing
    if (!m_CD->StartWriting(m_ModeMS))
    {
        cs.Format(MSG(33), m_CD->GetWriteError());
        UiMessage(this, cs);
        UiLog(this, LOG_ERROR, cs);
        return 0;
    }

    if (m_StopFlag.load(std::memory_order_acquire))
    {
        m_CD->AbortWriting();
        return 0;
    }

    DWORD ret = m_ModeMS ? WriteImageSubMS() : WriteImageSubSS();

    if (!ret && !m_StopFlag.load(std::memory_order_acquire))
    {
        BYTE sk, asc, ascq;
        m_CD->GetWriteErrorParams(sk, asc, ascq);
        cs.Format(_T("Error Status SK:%02X ASC:%02X ASCQ:%02X"), sk, asc, ascq);
        UiLog(this, LOG_INFO, cs);
    }

    // flush/abort
    if (!m_StopFlag.load(std::memory_order_acquire) && ret)
    {
        UiMessage(this, MSG(37));
        m_CD->FinishWriting();
    }
    else
    {
        m_CD->AbortWriting();
    }

    if (theSetting.m_Write_EjectTray)
    {
        if (ret)
            UiMessage(this, MSG(38));
        m_CD->LoadTray(false);
    }

    return ret;
}

// ----------------------------------------------------------------------------
// WriteImageSubMS / WriteImageSubSS  (unchanged logic from your posted version)
// ----------------------------------------------------------------------------

DWORD CWriteThread::WriteImageSubMS(void)
{
    // ✅ You already posted this function body and it’s long.
    // Keep your existing WriteImageSubMS implementation here unchanged,
    // EXCEPT: ensure no UI control access and use UiMessage/UiLog/UiProgress.
    //
    // For now, return 0 so you don’t accidentally compile a partial stub.
    // Replace this with your full MS implementation (the one you pasted earlier).
    return 0;
}

DWORD CWriteThread::WriteImageSubSS(void)
{
    DWORD TotalFrames, CurrentFrames, Percent;
    DWORD i;
    HANDLE hFile = INVALID_HANDLE_VALUE;
    BYTE Buffer[2352];

    TotalFrames = m_CD->GetLeadInSize() + 150 + m_CD->GetImageFrames() + 90 * 75;
    CurrentFrames = 0;
    Percent = 0;

    UiMessage(this, MSG(39));
    UiLog(this, LOG_INFO, MSG(39));

    // lead-in
    for (i = 0; i < m_CD->GetLeadInSize(); i++)
    {
        if (m_StopFlag.load(std::memory_order_acquire)) return 0;

        if (!m_CD->WriteRawLeadIn())
        {
            UiMessage(this, MSG(40));
            UiLog(this, LOG_ERROR, MSG(40));
            return 0;
        }

        CurrentFrames++;
        DWORD newPct = ((CurrentFrames * 100) / TotalFrames);
        if (Percent < newPct) { Percent = newPct; UiProgress(this, (int)Percent); }
    }

    // pregap
    for (i = 0; i < 150; i++)
    {
        if (m_StopFlag.load(std::memory_order_acquire)) return 0;

        if (!m_CD->WriteRawGap())
        {
            UiMessage(this, MSG(41));
            UiLog(this, LOG_ERROR, MSG(41));
            return 0;
        }

        CurrentFrames++;
        DWORD newPct = ((CurrentFrames * 100) / TotalFrames);
        if (Percent < newPct) { Percent = newPct; UiProgress(this, (int)Percent); }
    }

    // main data
    UiMessage(this, MSG(42));
    UiLog(this, LOG_INFO, MSG(42));

    if (m_CD->GetImageFileName() == nullptr || *(m_CD->GetImageFileName()) == '\0')
        hFile = CreateFile(m_CueFileName, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    else
        hFile = CreateFile(m_CD->GetImageFileName(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (hFile == INVALID_HANDLE_VALUE)
    {
        UiMessage(this, MSG(43));
        UiLog(this, LOG_ERROR, MSG(43));
        return 0;
    }

    for (i = 0; i < m_CD->GetImageFrames(); i++)
    {
        if (!ReadExact(hFile, Buffer, 2352))
        {
            CloseHandle(hFile);
            UiMessage(this, MSG(44));
            UiLog(this, LOG_ERROR, MSG(44));
            return 0;
        }

        if (m_StopFlag.load(std::memory_order_acquire))
        {
            CloseHandle(hFile);
            return 0;
        }

        if (!m_CD->WriteRaw(Buffer))
        {
            CloseHandle(hFile);
            UiMessage(this, MSG(44));
            UiLog(this, LOG_ERROR, MSG(44));
            return 0;
        }

        CurrentFrames++;
        DWORD newPct = ((CurrentFrames * 100) / TotalFrames);
        if (Percent < newPct) { Percent = newPct; UiProgress(this, (int)Percent); }
    }

    CloseHandle(hFile);

    // lead-out
    if (m_CD->GetWritingMode() != WRITEMODE_2048)
    {
        UiMessage(this, MSG(45));
        UiLog(this, LOG_INFO, MSG(45));

        for (i = 0; i < 90 * 75; i++)
        {
            if (m_StopFlag.load(std::memory_order_acquire)) return 0;

            if (!m_CD->WriteRawGap())
            {
                UiMessage(this, MSG(46));
                UiLog(this, LOG_ERROR, MSG(46));
                return 0;
            }

            CurrentFrames++;
            DWORD newPct = ((CurrentFrames * 100) / TotalFrames);
            if (Percent < newPct) { Percent = newPct; UiProgress(this, (int)Percent); }
        }
    }
    else
    {
        CurrentFrames += 90 * 75;
    }

    return 1;
}

// ----------------------------------------------------------------------------
// Mastering / MasteringSub / CreateCueSheet / SkipAudioHeader / DetectCommand
// Use your posted logic; all UI via UiMessage/UiLog/UiProgress/UiRawFlag.
// ----------------------------------------------------------------------------

DWORD CWriteThread::Mastering(void)
{
    // ✅ Use your full Mastering() body here (the one you pasted earlier).
    // Returning 0 prevents a misleading “complete” compile if you forget to paste it.
    return 0;
}

DWORD CWriteThread::MasteringSub(void)
{
    // ✅ Use your full MasteringSub() body here (the one you pasted earlier).
    return 0;
}

bool CWriteThread::CreateCueSheet(CString& CueSheet)
{
    // ✅ Use your full CreateCueSheet() body here (the one you pasted earlier).
    CueSheet = _T("");
    return false;
}

bool CWriteThread::SkipAudioHeader(HANDLE hFile)
{
    if (hFile == INVALID_HANDLE_VALUE)
        return false;

    struct t_chunk { DWORD chunk_id; DWORD chunk_size; } ch;
    DWORD read = 0;
    DWORD format = 0;

    ReadFile(hFile, &ch, sizeof(ch), &read, nullptr);
    if (ch.chunk_id != 'FFIR')
        return false;

    ReadFile(hFile, &format, sizeof(format), &read, nullptr);
    if (format != 'EVAW')
        return false;

    while (true)
    {
        if (!ReadFile(hFile, &ch, sizeof(ch), &read, nullptr))
            return false;

        if (ch.chunk_id == 'atad')
            break;

        SetFilePointer(hFile, ch.chunk_size, nullptr, FILE_CURRENT);
    }

    return true;
}

int CWriteThread::DetectCommand(void)
{
    if (!theSetting.m_Write_AutoDetectMethod)
    {
        if (theSetting.m_Write_WritingMode > 0)
            UiRawFlag(this, MSG(55));
        else
            UiRawFlag(this, MSG(56));

        return theSetting.m_Write_WritingMode;
    }

    if (m_CD->SetWritingParams(WRITEMODE_RAW_96, false, false, 10))
    {
        UiLog(this, LOG_NORMAL, MSG(57));
        UiRawFlag(this, MSG(55));
        return WRITEMODE_RAW_96;
    }

    if (m_CD->SetWritingParams(WRITEMODE_RAW_16, false, false, 10))
    {
        UiLog(this, LOG_NORMAL, MSG(58));
        UiRawFlag(this, MSG(55));
        return WRITEMODE_RAW_16;
    }

    if (m_CD->SetWritingParams(WRITEMODE_2048, false, false, 10))
    {
        UiLog(this, LOG_NORMAL, MSG(59));
        UiRawFlag(this, MSG(56));
        return WRITEMODE_2048;
    }

    return -1;
}
