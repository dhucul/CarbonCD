// writethread.cpp  (updated  UI-thread safe + safety/robustness fixes)
//
// What changed (UI safety):
// - Worker thread NEVER touches any dialog controls or dialog CString members.
// - Worker thread NEVER calls MessageBox directly.
// - Worker thread NEVER calls CLogWindow UI methods directly.
// - Worker thread POSTS updates to the dialog (UI thread applies them).
// - Worker thread SENDS Yes/No queries to the dialog (UI thread shows prompt).
//
// What remains from your patch:
// - Correct CreateThread failure check (NULL, not INVALID_HANDLE_VALUE)
// - StopThread cooperative stop (no TerminateThread)
// - CreateFile / CloseHandle safety
// - ReadFile/GetFileSize error checks in key spots

#include "writethread.h"
#include "WriteProgressDialog.h"   // brings WM_APP_WRITE_UI_UPDATE / WM_APP_WRITE_QUERY_YESNO
#include "Setting.h"
#include "IsoCreator.h"
#include "CheckSector.h"

#include <windows.h>
#include <process.h>
#include <cstring>

// ---- small helpers ----------------------------------------------------------

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
        const DWORD err = GetLastError();
        if (err != NO_ERROR)
            return false;
    }
    return true;
}

// ---- UI marshaling helpers (worker -> dialog UI thread) ---------------------

struct WriteUiUpdate
{
    bool hasMessage = false;
    CString message;

    bool hasPercent = false;
    CString percent;

    bool hasRawFlag = false;
    CString rawFlag;

    bool hasProgress = false;
    int progress = 0;

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

static inline HWND SafeHwnd(CWnd* w)
{
    return (w && ::IsWindow(w->GetSafeHwnd())) ? w->GetSafeHwnd() : nullptr;
}

static void PostUi(CWriteThread* t, WriteUiUpdate* up)
{
    HWND h = SafeHwnd(t ? t->m_ParentWnd : nullptr);
    if (!h) { delete up; return; }
    ::PostMessage(h, WM_APP_WRITE_UI_UPDATE, 0, (LPARAM)up);
}

static void UiMessage(CWriteThread* t, const CString& s)
{
    auto* up = new WriteUiUpdate();
    up->hasMessage = true;
    up->message = s;
    PostUi(t, up);
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
    PostUi(t, up);
}

static void UiRawFlag(CWriteThread* t, const CString& s)
{
    auto* up = new WriteUiUpdate();
    up->hasRawFlag = true;
    up->rawFlag = s;
    PostUi(t, up);
}

static void UiLog(CWriteThread* t, int level, const CString& s)
{
    auto* up = new WriteUiUpdate();
    up->hasLog = true;
    up->logLevel = level;
    up->logText = s;
    PostUi(t, up);
}

static void UiAutoSave(CWriteThread* t)
{
    auto* up = new WriteUiUpdate();
    up->requestAutoSave = true;
    PostUi(t, up);
}

static void UiClose(CWriteThread* t)
{
    auto* up = new WriteUiUpdate();
    up->requestClose = true;
    PostUi(t, up);
}

static int UiAskYesNo(CWriteThread* t, const CString& text, const CString& caption, UINT flags)
{
    HWND h = SafeHwnd(t ? t->m_ParentWnd : nullptr);
    if (!h) return IDNO;

    WriteUiQueryYesNo q;
    q.text = text;
    q.caption = caption;
    q.flags = flags;

    // This runs the prompt on the UI thread and returns IDYES/IDNO to worker.
    return (int)::SendMessage(h, WM_APP_WRITE_QUERY_YESNO, 0, (LPARAM)&q);
}

// ---- thread proc ------------------------------------------------------------

// Thread function for CWriteThread (use _beginthreadex for CRT safety)
static unsigned __stdcall WriteThreadProc(void* p)
{
    return static_cast<unsigned>(static_cast<CWriteThread*>(p)->ThreadFunction());
}

CWriteThread::CWriteThread(void)
    : m_CueFileName(_T(""))
{
    m_StopFlag.store(false, std::memory_order_relaxed);
    m_hThread = nullptr;
    m_List = nullptr;
    m_Dir = nullptr;
}

CWriteThread::~CWriteThread(void)
{
    StopThread();
}

void CWriteThread::StartThread(void)
{
    StopThread();
    m_StopFlag.store(false, std::memory_order_release);

    uintptr_t th = _beginthreadex(nullptr, 0, &WriteThreadProc, this, 0, nullptr);
    m_hThread = th ? reinterpret_cast<HANDLE>(th) : nullptr;

    if (!m_hThread)
        return;

    SetThreadPriority(m_hThread, THREAD_PRIORITY_HIGHEST);
}


void CWriteThread::StopThread(void)
{
    if (!m_hThread)
        return;

    // Cooperative cancellation only.
    m_StopFlag.store(true, std::memory_order_release);

    DWORD exitCode = 0;
    if (GetExitCodeThread(m_hThread, &exitCode) && exitCode == STILL_ACTIVE)
        WaitForSingleObject(m_hThread, INFINITE);

    CloseHandle(m_hThread);
    m_hThread = nullptr;
}



DWORD CWriteThread::ThreadFunction(void)
{
    DWORD RetValue = 0;

    UiProgress(this, 0);

    if (m_Dir != nullptr && m_List != nullptr)
    {
        RetValue = Mastering();
    }
    else
    {
        CString ext = m_CueFileName.Right(3);
        ext.MakeLower();

        if (ext == _T("cue") || ext == _T("iso"))
        {
            m_ModeMS = false;
            RetValue = WriteImage();
        }
        else
        {
            m_ModeMS = true;
            RetValue = WriteImage();
        }
    }

    m_Success = false;

    if (!m_StopFlag.load(std::memory_order_acquire))
    {
        UiProgress(this, 100);

        if (RetValue)
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
    return RetValue;
}

DWORD CWriteThread::WriteImage(void)
{
    CString cs;
    DWORD RetVal = 0;

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
            BYTE Header1[16] = { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x02, 0x00, 0x01 };
            BYTE Header2[16] = { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x02, 0x00, 0x02 };

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

    // writing
    RetVal = m_ModeMS ? WriteImageSubMS() : WriteImageSubSS();

    if (!RetVal && !m_StopFlag.load(std::memory_order_acquire))
    {
        BYTE sk, asc, ascq;
        m_CD->GetWriteErrorParams(sk, asc, ascq);
        cs.Format(_T("Error Status SK:%02X ASC:%02X ASCQ:%02X"), sk, asc, ascq);
        UiLog(this, LOG_INFO, cs);
    }

    // flush/abort
    if (!m_StopFlag.load(std::memory_order_acquire) && RetVal)
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
        if (RetVal)
            UiMessage(this, MSG(38));

        m_CD->LoadTray(false);
    }

    return RetVal;
}

#define WRITE_HDD 0

DWORD CWriteThread::WriteImageSubMS(void)
{
    CString cs;

    int session;
    DWORD lba;
    BYTE* Sub96;
    BYTE Buffer[2352 + 96], SubBuffer[96];

    HANDLE hImg = INVALID_HANDLE_VALUE;
    HANDLE hSub = INVALID_HANDLE_VALUE;
    HANDLE hPre = INVALID_HANDLE_VALUE;

    DWORD ret = 1;
    DWORD FrameCount = 0;
    DWORD MaxFrames = 0;
    DWORD Percent = 0;
    bool Scramble = false;
    int TrackNo = 0;
    DWORD size = 0;

    hImg = CreateFile(m_SubMS.m_ImgFileName, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hImg == INVALID_HANDLE_VALUE)
    {
        UiMessage(this, MSG(43));
        UiLog(this, LOG_ERROR, MSG(43));
        return 0;
    }

    {
        DWORD fileSize = 0;
        if (!GetFileSize32(hImg, fileSize))
        {
            CloseHandle(hImg);
            UiMessage(this, MSG(43));
            UiLog(this, LOG_ERROR, MSG(43));
            return 0;
        }
        size = fileSize / 2352;
    }

    hSub = CreateFile(m_SubMS.m_SubFileName, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hSub == INVALID_HANDLE_VALUE)
    {
        UiMessage(this, MSG(43));
        UiLog(this, LOG_ERROR, MSG(43));
        CloseHandle(hImg);
        return 0;
    }

    if (m_SubMS.m_PreFileName != "")
    {
        hPre = CreateFile(m_SubMS.m_PreFileName, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hPre != INVALID_HANDLE_VALUE)
        {
            cs.Format(_T("PregapFile:%s"), m_SubMS.m_PreFileName.GetString());
            UiLog(this, LOG_INFO, cs);
        }
    }

    m_SubMS.CalcPositions(0 - m_CD->GetLeadInSize());

    cs.Format(MSG(143), m_SubMS.m_ImageVersion);
    UiLog(this, LOG_INFO, cs);

    if (m_SubMS.m_CDM_Extension)
    {
        cs.Format(MSG(144), m_SubMS.m_ProductName);
        UiLog(this, LOG_INFO, cs);
        cs.Format(MSG(145), m_SubMS.m_VendorName);
        UiLog(this, LOG_INFO, cs);
        cs.Format(MSG(146), m_SubMS.m_Revision);
        UiLog(this, LOG_INFO, cs);
    }

    for (session = 0; session < m_SubMS.GetSessionCount(); session++)
    {
        cs.Format(MSG(140), session + 1);
        UiLog(this, LOG_INFO, cs);
        UiMessage(this, cs);
        UiProgress(this, 0);

        FrameCount = 0;
        Percent = 0;

        if (session == 0)
            MaxFrames = m_CD->GetLeadInSize() + (m_SubMS.m_LeadInLBA[1] - m_SubMS.m_PregapLBA[0]);
        else
            MaxFrames = m_SubMS.m_LeadInLBA[session + 1] - m_SubMS.m_LeadInLBA[session];

        // lead-in
        m_SubMS.ResetGenerator(m_SubMS.m_LeadInLBA[session], SUBTYPE_LEADIN, session);
        lba = m_SubMS.m_LeadInLBA[session];

        for (lba = m_SubMS.m_LeadInLBA[session]; lba != m_SubMS.m_PregapLBA[session]; lba++)
        {
            if (ret == 0 || m_StopFlag.load(std::memory_order_acquire))
                break;

            Sub96 = m_SubMS.GenerateLeadIn();
            m_SubMS.CreateZeroData(Buffer, lba);

            if (m_CD->GetWritingMode() != WRITEMODE_RAW_16)
            {
                m_SubMS.EncodeSub96(Buffer + 2352, Sub96);
            }
            else
            {
                memset(Buffer + 2352, 0, 96);
                memcpy(Buffer + 2352, Sub96 + 12, 12);
                if (Sub96[0] != 0x00 && Sub96[1] != 0x00)
                    Buffer[2352 + 15] = 0x80;
            }

            if (m_SubMS.m_PreGapMode[session])
                m_CD->ForceScramble(Buffer);

            if (!m_CD->WriteRaw96(Buffer, lba))
            {
                UiMessage(this, MSG(40));
                UiLog(this, LOG_ERROR, MSG(40));
                ret = 0;
            }

            FrameCount++;
            const DWORD newPct = ((FrameCount * 100) / MaxFrames);
            if (Percent < newPct)
            {
                Percent = newPct;
                UiProgress(this, (int)Percent);
            }
        }

        // pregap
        m_SubMS.ResetGenerator(m_SubMS.m_PregapLBA[session], SUBTYPE_PREGAP, session);

        for (; lba != m_SubMS.m_MainDataLBA[session]; lba++)
        {
            if (ret == 0 || m_StopFlag.load(std::memory_order_acquire))
                break;

            Sub96 = m_SubMS.GeneratePreGap();

            if (session == 0 && hPre != INVALID_HANDLE_VALUE)
            {
                if (!ReadExact(hPre, Buffer, 2352))
                {
                    UiMessage(this, MSG(41));
                    UiLog(this, LOG_ERROR, MSG(41));
                    ret = 0;
                    break;
                }
            }
            else
            {
                m_SubMS.CreateZeroData(Buffer, lba);
            }

            if (m_CD->GetWritingMode() != WRITEMODE_RAW_16)
            {
                m_SubMS.EncodeSub96(Buffer + 2352, Sub96);
            }
            else
            {
                memset(Buffer + 2352, 0, 96);
                memcpy(Buffer + 2352, Sub96 + 12, 12);
                if (Sub96[0] != 0x00 && Sub96[1] != 0x00)
                    Buffer[2352 + 15] = 0x80;
            }

            if (m_SubMS.m_PreGapMode[session])
                m_CD->ForceScramble(Buffer);

            if (!m_CD->WriteRaw96(Buffer, lba))
            {
                UiMessage(this, MSG(41));
                UiLog(this, LOG_ERROR, MSG(41));
                ret = 0;
            }

            FrameCount++;
            const DWORD newPct = ((FrameCount * 100) / MaxFrames);
            if (Percent < newPct)
            {
                Percent = newPct;
                UiProgress(this, (int)Percent);
            }
        }

        // main data
        if (ret && !m_StopFlag.load(std::memory_order_acquire))
        {
            cs.Format(MSG(141), session + 1);
            UiLog(this, LOG_INFO, cs);
            UiMessage(this, cs);
        }

        for (;; lba++)
        {
            if (!m_SubMS.m_AbnormalImageSize)
            {
                if (lba == m_SubMS.m_LeadOutLBA[session])
                    break;
            }
            else
            {
                if (size == 0)
                    break;
                size--;
            }

            if (ret == 0 || m_StopFlag.load(std::memory_order_acquire))
                break;

            if (!ReadExact(hSub, SubBuffer, 96) || !ReadExact(hImg, Buffer, 2352))
            {
                UiMessage(this, MSG(44));
                UiLog(this, LOG_ERROR, MSG(44));
                ret = 0;
                break;
            }

            if (m_CD->GetWritingMode() != WRITEMODE_RAW_16)
            {
                m_SubMS.EncodeSub96(Buffer + 2352, SubBuffer);
            }
            else
            {
                memset(Buffer + 2352, 0, 96);
                memcpy(Buffer + 2352, SubBuffer + 12, 12);
                if (SubBuffer[0] != 0x00 && SubBuffer[1] != 0x00)
                    Buffer[2352 + 15] = 0x80;
            }

            if ((SubBuffer[12] & 0x0f) == 0x01)
            {
                if (SubBuffer[13] != TrackNo)
                {
                    Scramble = (SubBuffer[12] & 0x40) != 0;
                    TrackNo = SubBuffer[13];
                }
            }

            if (Scramble)
                m_CD->ForceScramble(Buffer);

            if (!m_CD->WriteRaw96(Buffer, lba))
            {
                UiMessage(this, MSG(44));
                UiLog(this, LOG_ERROR, MSG(44));
                ret = 0;
            }

            FrameCount++;
            const DWORD newPct = ((FrameCount * 100) / MaxFrames);
            if (Percent < newPct)
            {
                Percent = newPct;
                UiProgress(this, (int)Percent);
            }
        }

        // lead-out
        if (ret && !m_StopFlag.load(std::memory_order_acquire))
        {
            cs.Format(MSG(142), session + 1);
            UiLog(this, LOG_INFO, cs);
            UiMessage(this, cs);
        }

        m_SubMS.ResetGenerator(m_SubMS.m_LeadOutLBA[session], SUBTYPE_LEADOUT, session);

        if (!m_SubMS.m_AbnormalImageSize)
            size = m_SubMS.m_LeadInLBA[session + 1];
        else
            size = lba + 90 * 75;

        for (;; lba++)
        {
            if (lba >= size)
                break;
            if (ret == 0 || m_StopFlag.load(std::memory_order_acquire))
                break;

            Sub96 = m_SubMS.GenerateLeadOut();
            m_SubMS.CreateZeroData(Buffer, lba);

            if (m_CD->GetWritingMode() != WRITEMODE_RAW_16)
            {
                m_SubMS.EncodeSub96(Buffer + 2352, Sub96);
            }
            else
            {
                memset(Buffer + 2352, 0, 96);
                memcpy(Buffer + 2352, Sub96 + 12, 12);
                if (Sub96[0] != 0x00 && Sub96[1] != 0x00)
                    Buffer[2352 + 15] = 0x80;
            }

            if (m_SubMS.m_PreGapMode[session])
                m_CD->ForceScramble(Buffer);

            if (!m_CD->WriteRaw96(Buffer, lba))
            {
                UiMessage(this, MSG(46));
                UiLog(this, LOG_ERROR, MSG(46));
                ret = 0;
            }

            FrameCount++;
            const DWORD newPct = ((FrameCount * 100) / MaxFrames);
            if (Percent < newPct)
            {
                Percent = newPct;
                UiProgress(this, (int)Percent);
            }
        }

        if (ret == 0 || m_StopFlag.load(std::memory_order_acquire))
            break;
    }

    if (hSub != INVALID_HANDLE_VALUE) CloseHandle(hSub);
    if (hImg != INVALID_HANDLE_VALUE) CloseHandle(hImg);
    if (hPre != INVALID_HANDLE_VALUE) CloseHandle(hPre);

    if (m_StopFlag.load(std::memory_order_acquire))
        return 0;

    return ret;
}

// Single-session writing (UI-thread safe progress/log updates)
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
        const DWORD newPct = ((CurrentFrames * 100) / TotalFrames);
        if (Percent < newPct)
        {
            Percent = newPct;
            UiProgress(this, (int)Percent);
        }
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
        const DWORD newPct = ((CurrentFrames * 100) / TotalFrames);
        if (Percent < newPct)
        {
            Percent = newPct;
            UiProgress(this, (int)Percent);
        }
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
        const DWORD newPct = ((CurrentFrames * 100) / TotalFrames);
        if (Percent < newPct)
        {
            Percent = newPct;
            UiProgress(this, (int)Percent);
        }
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
            const DWORD newPct = ((CurrentFrames * 100) / TotalFrames);
            if (Percent < newPct)
            {
                Percent = newPct;
                UiProgress(this, (int)Percent);
            }
        }
    }
    else
    {
        CurrentFrames += 90 * 75;
    }

    return 1;
}

// Everything below: keep behavior; just marshal logs/rawflag and remove dialog touches

DWORD CWriteThread::Mastering(void)
{
    CString cs, CueSheet;
    DWORD RetVal = 0;

    UiMessage(this, MSG(47));
    UiLog(this, LOG_NORMAL, MSG(47));

    cs.Format(_T("%s : Mastering"), MSG(137));
    UiLog(this, LOG_NORMAL, cs);

    UiMessage(this, MSG(48));
    UiLog(this, LOG_NORMAL, MSG(48));

    if (!CreateCueSheet(CueSheet))
    {
        UiMessage(this, MSG(48));
        UiLog(this, LOG_ERROR, MSG(48));
        return 0;
    }

    UiLog(this, LOG_NORMAL, MSG(24));
    UiMessage(this, MSG(24));

    if (theSetting.m_Write_BurnProof)
        UiLog(this, LOG_NORMAL, MSG(25));

    if (theSetting.m_Write_TestMode)
        UiLog(this, LOG_NORMAL, MSG(26));

    UiMessage(this, MSG(50));
    UiLog(this, LOG_INFO, MSG(50));

    if (!m_CD->ParseCueSheet(CueSheet, m_TotalFrames * 2352))
    {
        UiMessage(this, m_CD->GetWriteError());
        UiLog(this, LOG_ERROR, m_CD->GetWriteError());
        return 0;
    }

    if (m_StopFlag.load(std::memory_order_acquire))
        return 0;

    // check drive
    if (theSetting.m_Write_CheckDrive)
    {
        if (!m_CD->IsCDR())
        {
            UiMessage(this, MSG(29));
            UiLog(this, LOG_ERROR, MSG(29));
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
        bool BurnProof = (theSetting.m_Write_BurnProof != 0);
        bool TestMode = (theSetting.m_Write_TestMode != 0);
        int  WriteMode = DetectCommand();

        if (WriteMode < 0)
        {
            UiMessage(this, MSG(32));
            UiLog(this, LOG_ERROR, MSG(32));
            return 0;
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

    m_CD->SetSpeed(0xff, theSetting.m_Write_Speed);

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

    if (!m_CD->StartWriting(false))
    {
        cs.Format(MSG(33), m_CD->GetWriteError());
        UiMessage(this, cs);
        UiLog(this, LOG_ERROR, cs);
        return 0;
    }

    if (m_StopFlag.load(std::memory_order_acquire))
        return 0;

    RetVal = MasteringSub();

    if (!m_StopFlag.load(std::memory_order_acquire) && RetVal)
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
        UiMessage(this, MSG(38));
        m_CD->LoadTray(false);
    }

    if (!RetVal && !m_StopFlag.load(std::memory_order_acquire))
    {
        BYTE sk, asc, ascq;
        m_CD->GetWriteErrorParams(sk, asc, ascq);
        cs.Format(_T("Error Status SK:%02X ASC:%02X ASCQ:%02X"), sk, asc, ascq);
        UiLog(this, LOG_INFO, cs);
    }

    return RetVal;
}

DWORD CWriteThread::MasteringSub(void)
{
    DWORD TotalFrames, CurrentFrames, Percent;
    DWORD i;
    CIsoCreator iso;
    DWORD TrackType;
    CString cs;
    DWORD PrevTrackType;
    DWORD read;
    BYTE Buffer[2352];
    CCheckSector edc;
    MSFAddress msf;

    if (m_List->GetItemData(0) == 0)
    {
        iso.SetParams(m_VolumeLabel, theSetting.m_CopyProtectionSize);
        iso.CreateJolietHeader(m_Dir);
        iso.InitializeReading();
    }

    TotalFrames = m_CD->GetLeadInSize() + 150 + m_TotalFrames + 90 * 75;
    CurrentFrames = 0;
    Percent = 0;

    UiMessage(this, MSG(39));
    UiLog(this, LOG_INFO, MSG(39));

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
        const DWORD newPct = ((CurrentFrames * 100) / TotalFrames);
        if (Percent < newPct)
        {
            Percent = newPct;
            UiProgress(this, (int)Percent);
        }
    }

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
        const DWORD newPct = ((CurrentFrames * 100) / TotalFrames);
        if (Percent < newPct)
        {
            Percent = newPct;
            UiProgress(this, (int)Percent);
        }
    }

    UiMessage(this, MSG(42));
    UiLog(this, LOG_INFO, MSG(42));

    PrevTrackType = 0;

    for (i = 0; i < static_cast<DWORD>(m_List->GetItemCount()); i++)
    {
        TrackType = m_List->GetItemData(i);

        if (m_StopFlag.load(std::memory_order_acquire)) break;

        if (TrackType == 0)
        {
            CString tt;
            UiLog(this, LOG_INFO, MSG(51));
            tt = m_List->GetItemText(0, 1);

            if (tt == "Mastering")
            {
                while (iso.GetHeaderFrame(Buffer))
                {
                    if (m_StopFlag.load(std::memory_order_acquire)) break;

                    if (!m_CD->WriteRaw(Buffer))
                    {
                        UiMessage(this, MSG(44));
                        UiLog(this, LOG_ERROR, MSG(44));
                        return 0;
                    }

                    CurrentFrames++;
                    const DWORD newPct = ((CurrentFrames * 100) / TotalFrames);
                    if (Percent < newPct)
                    {
                        Percent = newPct;
                        UiProgress(this, (int)Percent);
                    }
                }

#if COPY_PROTECTION
                while (iso.GetProtectionArea(Buffer))
                {
                    if (m_StopFlag.load(std::memory_order_acquire)) break;

                    if (!m_CD->WriteRaw(Buffer))
                    {
                        UiMessage(this, MSG(44));
                        UiLog(this, LOG_ERROR, MSG(44));
                        return 0;
                    }

                    CurrentFrames++;
                    const DWORD newPct = ((CurrentFrames * 100) / TotalFrames);
                    if (Percent < newPct)
                    {
                        Percent = newPct;
                        UiProgress(this, (int)Percent);
                    }
                }
#endif

                while (iso.OpenReadFile())
                {
                    if (m_StopFlag.load(std::memory_order_acquire)) break;

                    while (iso.GetFrame(Buffer))
                    {
                        if (m_StopFlag.load(std::memory_order_acquire)) break;

                        if (!m_CD->WriteRaw(Buffer))
                        {
                            UiMessage(this, MSG(44));
                            UiLog(this, LOG_ERROR, MSG(44));
                            return 0;
                        }

                        CurrentFrames++;
                        const DWORD newPct = ((CurrentFrames * 100) / TotalFrames);
                        if (Percent < newPct)
                        {
                            Percent = newPct;
                            UiProgress(this, (int)Percent);
                        }
                    }

                    iso.CloseReadFile();
                }
            }
            else if (tt == "MODE1/2048")
            {
                HANDLE hFileRead;
                CString FileName;
                DWORD lba = 150;
                FileName = m_List->GetItemText(i, 2);
                hFileRead = CreateFile(FileName, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

                if (hFileRead == INVALID_HANDLE_VALUE)
                    return false;

                while (true)
                {
                    if (m_StopFlag.load(std::memory_order_acquire)) break;

                    ReadFile(hFileRead, Buffer + 16, 2048, &read, nullptr);
                    if (read < 2048) break;

                    msf = lba;
                    edc.Mode1Raw(Buffer, msf.Minute, msf.Second, msf.Frame);

                    if (!m_CD->WriteRaw(Buffer))
                    {
                        UiMessage(this, MSG(44));
                        UiLog(this, LOG_ERROR, MSG(44));
                        CloseHandle(hFileRead);
                        return false;
                    }

                    CurrentFrames++;
                    lba++;

                    const DWORD newPct = ((CurrentFrames * 100) / TotalFrames);
                    if (Percent < newPct)
                    {
                        Percent = newPct;
                        UiProgress(this, (int)Percent);
                    }
                }

                CloseHandle(hFileRead);
            }
            else if (tt == "MODE1/2352" || tt == "MODE2/2352")
            {
                HANDLE hFileRead;
                CString FileName;
                FileName = m_List->GetItemText(i, 2);
                hFileRead = CreateFile(FileName, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

                if (hFileRead == INVALID_HANDLE_VALUE)
                    return false;

                while (true)
                {
                    if (m_StopFlag.load(std::memory_order_acquire)) break;

                    ReadFile(hFileRead, Buffer, 2352, &read, nullptr);
                    if (read < 2352) break;

                    if (!m_CD->WriteRaw(Buffer))
                    {
                        UiMessage(this, MSG(44));
                        UiLog(this, LOG_ERROR, MSG(44));
                        CloseHandle(hFileRead);
                        return false;
                    }

                    CurrentFrames++;

                    const DWORD newPct = ((CurrentFrames * 100) / TotalFrames);
                    if (Percent < newPct)
                    {
                        Percent = newPct;
                        UiProgress(this, (int)Percent);
                    }
                }

                CloseHandle(hFileRead);
            }

            if (m_StopFlag.load(std::memory_order_acquire)) break;
            PrevTrackType = TrackType;
        }
        else
        {
            cs.Format(MSG(52), i + 1);
            UiLog(this, LOG_INFO, cs);

            HANDLE hFileRead;
            CString FileName;

            if (i == 1 && PrevTrackType == 0)
            {
                int j;
                memset(Buffer, 0, 2352);

                for (j = 0; j < 150; j++)
                {
                    if (!m_CD->WriteRaw(Buffer))
                    {
                        UiMessage(this, MSG(44));
                        UiLog(this, LOG_ERROR, MSG(44));
                        return 0;
                    }

                    CurrentFrames++;
                    const DWORD newPct = ((CurrentFrames * 100) / TotalFrames);
                    if (Percent < newPct)
                    {
                        Percent = newPct;
                        UiProgress(this, (int)Percent);
                    }
                }
            }

            FileName = m_List->GetItemText(i, 2);
            hFileRead = CreateFile(FileName, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

            if (SkipAudioHeader(hFileRead))
            {
                while (true)
                {
                    if (m_StopFlag.load(std::memory_order_acquire)) break;

                    ReadFile(hFileRead, Buffer, 2352, &read, nullptr);

                    if (read == 2352)
                    {
                        if (!m_CD->WriteRaw(Buffer))
                        {
                            UiMessage(this, MSG(44));
                            UiLog(this, LOG_ERROR, MSG(44));
                            CloseHandle(hFileRead);
                            return 0;
                        }

                        CurrentFrames++;
                        const DWORD newPct = ((CurrentFrames * 100) / TotalFrames);
                        if (Percent < newPct)
                        {
                            Percent = newPct;
                            UiProgress(this, (int)Percent);
                        }
                    }
                    else if (read > 0)
                    {
                        memset(Buffer + read, 0, 2352 - read);

                        if (!m_CD->WriteRaw(Buffer))
                        {
                            UiMessage(this, MSG(44));
                            UiLog(this, LOG_ERROR, MSG(44));
                            CloseHandle(hFileRead);
                            return 0;
                        }

                        CurrentFrames++;
                        const DWORD newPct = ((CurrentFrames * 100) / TotalFrames);
                        if (Percent < newPct)
                        {
                            Percent = newPct;
                            UiProgress(this, (int)Percent);
                        }
                    }
                    else
                    {
                        break;
                    }
                }
            }
            else
            {
                UiLog(this, LOG_INFO, FileName);
            }

            CloseHandle(hFileRead);
            PrevTrackType = TrackType;
        }

        if (m_StopFlag.load(std::memory_order_acquire)) break;
    }

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
            const DWORD newPct = ((CurrentFrames * 100) / TotalFrames);
            if (Percent < newPct)
            {
                Percent = newPct;
                UiProgress(this, (int)Percent);
            }
        }
    }
    else
    {
        CurrentFrames += 90 * 75;
    }

    return 1;
}

bool CWriteThread::CreateCueSheet(CString& CueSheet)
{
    int i;
    DWORD TrackType;
    DWORD lba;
    CString cs;
    DWORD PrevTrackType;
    MSFAddress msf;

    CueSheet = "";
    PrevTrackType = 0;
    lba = 0;

    for (i = 0; i < m_List->GetItemCount(); i++)
    {
        TrackType = m_List->GetItemData(i);

        if (TrackType == 0)
        {
            CString tt;
            tt = m_List->GetItemText(0, 1);

            if (i != 0)
            {
                UiLog(this, LOG_ERROR, MSG(53));
                return false;
            }

            if (tt == "Mastering")
            {
                CIsoCreator iso;
                cs.Format(_T("  TRACK 01 MODE1/2352\n"));
                CueSheet += cs;
                cs.Format(_T("    INDEX 01 00:00:00\n"));
                CueSheet += cs;
                iso.SetParams(m_VolumeLabel, theSetting.m_CopyProtectionSize);
                iso.CreateJolietHeader(m_Dir);
                lba += iso.GetImageSize();
            }
            else
            {
                DWORD FileSize;
                HANDLE hFile;
                CString fn;
                fn = m_List->GetItemText(i, 2);
                hFile = CreateFile(fn, 0, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                FileSize = GetFileSize(hFile, nullptr);
                CloseHandle(hFile);

                if (tt == "MODE1/2048")
                {
                    cs.Format(_T("  TRACK 01 MODE1/2352\n"));
                    lba += FileSize / 2048;
                }
                else if (tt == "MODE1/2352")
                {
                    cs.Format(_T("  TRACK 01 MODE1/2352\n"));
                    lba += FileSize / 2352;
                }
                else if (tt == "MODE2/2352")
                {
                    cs.Format(_T("  TRACK 01 MODE2/2352\n"));
                    lba += FileSize / 2352;
                }

                CueSheet += cs;
                cs.Format(_T("    INDEX 01 00:00:00\n"));
                CueSheet += cs;
            }

            PrevTrackType = TrackType;
        }
        else
        {
            HANDLE hFile;
            CString FileName;

            cs.Format(_T("  TRACK %02d AUDIO\n"), i + 1);
            CueSheet += cs;

            if (i == 1 && PrevTrackType == 0)
            {
                msf = lba;
                cs.Format(_T("    INDEX 00 %02d:%02d:%02d\n"), msf.Minute, msf.Second, msf.Frame);
                CueSheet += cs;
                lba += 150;
            }

            msf = lba;
            cs.Format(_T("    INDEX 01 %02d:%02d:%02d\n"), msf.Minute, msf.Second, msf.Frame);
            CueSheet += cs;

            FileName = m_List->GetItemText(i, 2);
            hFile = CreateFile(FileName, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

            if (SkipAudioHeader(hFile))
            {
                DWORD np, ep, size;
                np = SetFilePointer(hFile, 0, nullptr, FILE_CURRENT);
                ep = SetFilePointer(hFile, 0, nullptr, FILE_END);
                size = ep - np;
                lba += size / 2352;
                if (size % 2352) lba++;
            }
            else
            {
                cs.Format(MSG(54), i + 1);
                UiLog(this, LOG_ERROR, cs);
                CloseHandle(hFile);
                return false;
            }

            CloseHandle(hFile);
            PrevTrackType = TrackType;
        }
    }

    m_TotalFrames = lba;
    return true;
}

struct t_chunk
{
    DWORD chunk_id;
    DWORD chunk_size;
};

bool CWriteThread::SkipAudioHeader(HANDLE hFile)
{
    if (hFile == INVALID_HANDLE_VALUE)
        return false;

    struct t_chunk ch;
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
