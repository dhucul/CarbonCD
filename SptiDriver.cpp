// SptiDriver.cpp
// SPTI (IOCTL_SCSI_PASS_THROUGH_DIRECT) implementation matching SptiDriver.h

#include "stdafx.h"      // MUST be first if project uses /Yu"stdafx.h"
#include "SptiDriver.h"
#include "Setting.h"

#include <VersionHelpers.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <chrono>

// ------------------------------------------------------------
// Debug logging: define SPTI_DEBUG_LOG in project settings
// ------------------------------------------------------------
// #define SPTI_DEBUG_LOG 1

#ifdef SPTI_DEBUG_LOG
static void dbgprintf(const char* fmt, ...)
{
    char buf[2048] = {};
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    ::OutputDebugStringA(buf);
}

static void dump_hex32(const char* label, const BYTE* p, size_t n)
{
    if (!p || n == 0) { dbgprintf("%s: <none>\n", label); return; }
    dbgprintf("%s:", label);
    for (size_t i = 0; i < n; ++i) dbgprintf(" %02X", (unsigned)p[i]);
    dbgprintf("\n");
}
#else
static void dbgprintf(const char*, ...) {}
static void dump_hex32(const char*, const BYTE*, size_t) {}
#endif

// ------------------------------
// helpers
// ------------------------------
static bool IsWindowsNT()
{
    return IsWindowsVersionOrGreater(5, 0, 0);
}

static bool IsSenseValid(const BYTE* s)
{
    if (!s) return false;
    const BYTE rc = s[0] & 0x7F;
    return (rc == 0x70 || rc == 0x71 || rc == 0x72 || rc == 0x73);
}

// Robust: fixed (0x70/0x71) and descriptor (0x72/0x73)
static bool DecodeSense(const BYTE* s, int& key, int& asc, int& ascq)
{
    key = asc = ascq = -1;
    if (!IsSenseValid(s)) return false;

    const BYTE rc = s[0] & 0x7F;
    if (rc == 0x70 || rc == 0x71)
    {
        key = s[2] & 0x0F;
        asc = s[12];
        ascq = s[13];
        return true;
    }
    if (rc == 0x72 || rc == 0x73)
    {
        key = s[1] & 0x0F;
        asc = s[2];
        ascq = s[3];
        return true;
    }
    return false;
}

static bool SenseIs(const BYTE* s, int k, int a, int q)
{
    int key, asc, ascq;
    if (!DecodeSense(s, key, asc, ascq)) return false;
    return (key == k && asc == a && ascq == q);
}

// 02/04/08 LONG WRITE IN PROGRESS
static bool IsLongWriteInProgress(const BYTE* s) { return SenseIs(s, 0x02, 0x04, 0x08); }
// 02/04/01 BECOMING READY
static bool IsBecomingReady(const BYTE* s) { return SenseIs(s, 0x02, 0x04, 0x01); }
// 02/3A/02 MEDIUM NOT PRESENT - TRAY OPEN
static bool IsNoMediumTrayOpen(const BYTE* s) { return SenseIs(s, 0x02, 0x3A, 0x02); }

// 05/24/00 INVALID FIELD IN CDB (seen on some drives for READ TOC when disc blank)
static bool IsReadTocInvalidFieldCompat(BYTE op, const BYTE* s)
{
    return (op == 0x43) && SenseIs(s, 0x05, 0x24, 0x00);
}

static bool IsWriteLikeOpcode(BYTE op)
{
    switch (op)
    {
    case 0x2A: // WRITE(10)
    case 0xAA: // WRITE(12)
    case 0x8A: // WRITE(16)
    case 0x5B: // CLOSE TRACK/SESSION
    case 0x35: // SYNCHRONIZE CACHE
    case 0x53: // RESERVE TRACK
    case 0x15: // MODE SELECT(6)
    case 0x55: // MODE SELECT(10)
    case 0xA1: // BLANK
    case 0x54: // SEND OPC INFORMATION
        return true;
    default:
        return false;
    }
}

static bool ShouldWaitAndRetryOnSense(const BYTE* sense)
{
    // transient "not ready" / "long write" / "tray open"
    return IsBecomingReady(sense) || IsLongWriteInProgress(sense) || IsNoMediumTrayOpen(sense);
}

static const char* ScsiStatusName(BYTE st)
{
    switch (st)
    {
    case 0x00: return "GOOD";
    case 0x02: return "CHECK_CONDITION";
    case 0x08: return "BUSY";
    default:   return "OTHER";
    }
}

static void LogSptiResult(const char* tag, const SPTD_WITH_SENSE& spti, BOOL ok, DWORD gle)
{
#ifdef SPTI_DEBUG_LOG
    const BYTE op = spti.sptd.Cdb[0];
    dbgprintf("[%s] ok=%d GLE=%lu op=%02X ScsiStatus=%02X(%s) sense0=%02X\n",
        tag,
        (int)ok,
        (unsigned long)gle,
        (unsigned)op,
        (unsigned)spti.sptd.ScsiStatus,
        ScsiStatusName((BYTE)spti.sptd.ScsiStatus),
        (unsigned)(spti.Sense[0]));

    dump_hex32("[cdb]", spti.sptd.Cdb, spti.sptd.CdbLength);

    if (ok && spti.sptd.ScsiStatus == 0x02)
    {
        int k = -1, a = -1, q = -1;
        DecodeSense(spti.Sense, k, a, q);
        dbgprintf("[sense] key=%02X asc=%02X ascq=%02X\n", (unsigned)k, (unsigned)a, (unsigned)q);
        dump_hex32("[sense32]", spti.Sense, sizeof(spti.Sense));
    }
#else
    (void)tag; (void)spti; (void)ok; (void)gle;
#endif
}

static BOOL DoSptiIoctl(HANDLE h, SPTD_WITH_SENSE& spti, DWORD& bytesReturned)
{
    return ::DeviceIoControl(
        h,
        IOCTL_SCSI_PASS_THROUGH_DIRECT,
        &spti, sizeof(spti),
        &spti, sizeof(spti),
        &bytesReturned,
        NULL);
}

// ------------------------------
// CSptiDriver
// ------------------------------
CSptiDriver::CSptiDriver()
{
    m_WaitEvent.reset(::CreateEvent(NULL, FALSE, TRUE, NULL));   // auto-reset, initially signaled
    m_CommandEvent.reset(::CreateEvent(NULL, FALSE, FALSE, NULL));

    m_ExitFlag.store(false, std::memory_order_relaxed);
    m_Status = FALSE;

    // keep thread for compatibility; it can be used if you later restore true async pipelining
    m_Thread.reset(::CreateThread(NULL, 0, &CSptiDriver::thread_proc, this, 0, &m_ThreadID));

    Initialize();
}

CSptiDriver::~CSptiDriver()
{
    m_ExitFlag.store(true, std::memory_order_relaxed);
    if (m_CommandEvent) ::SetEvent(m_CommandEvent.get());
    if (m_Thread) ::WaitForSingleObject(m_Thread.get(), INFINITE);
}

DWORD CSptiDriver::GetVersion() { return 1; }
BOOL  CSptiDriver::IsActive() { return IsWindowsNT() ? TRUE : FALSE; }

void CSptiDriver::Initialize()
{
    m_DeviceCount = 0;
    m_CurrentDevice = -1;

    for (int i = 0; i < 26; ++i)
    {
        const char letter = (char)('A' + i);
        char root[4] = { letter, ':', '\\', '\0' };

        if (::GetDriveTypeA(root) == DRIVE_CDROM)
        {
            if (m_DeviceCount < (int)m_DeviceLetters.size())
                m_DeviceLetters[m_DeviceCount++] = letter;
        }
    }

    if (m_DeviceCount > 0)
        SetDevice(0);
}

int CSptiDriver::GetDeviceCount() { return m_DeviceCount; }
int CSptiDriver::GetCurrentDevice() { return m_CurrentDevice; }

void CSptiDriver::open_device_by_letter(char driveLetter)
{
    char path[8] = {};
    std::snprintf(path, sizeof(path), "\\\\.\\%c:", driveLetter);

    m_Device.reset(::CreateFileA(
        path,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        0,
        NULL));
}

void CSptiDriver::SetDevice(int n)
{
    if (n < 0 || n >= m_DeviceCount)
        return;

    m_CurrentDevice = n;
    open_device_by_letter(m_DeviceLetters[n]);
}

void CSptiDriver::GetDeviceString(CString& Vendor, CString& Product, CString& Revision, CString& BusAddress)
{
    Vendor.Empty();
    Product.Empty();
    Revision.Empty();
    BusAddress.Empty();

    if (m_CurrentDevice < 0 || !m_Device)
    {
        Vendor = Product = Revision = _T("Unknown");
        return;
    }

    SRB_ExecSCSICmd cmd;
    std::memset(&cmd, 0, sizeof(cmd));

    BYTE buf[100] = {};
    cmd.SRB_BufLen = sizeof(buf);
    cmd.SRB_BufPointer = buf;
    cmd.SRB_Flags = SRB_DIR_IN;
    cmd.SRB_CDBLen = 6;
    cmd.CDBByte[0] = 0x12; // INQUIRY
    cmd.CDBByte[4] = (BYTE)sizeof(buf);

    ExecuteCommand(cmd);

    if (cmd.SRB_Status == SS_COMP)
    {
        CStringA vA((LPCSTR)(buf + 8), 8);
        CStringA pA((LPCSTR)(buf + 16), 16);
        CStringA rA((LPCSTR)(buf + 32), 4);

        vA.TrimRight(); pA.TrimRight(); rA.TrimRight();

        Vendor = CString(vA);
        Product = CString(pA);
        Revision = CString(rA);
    }
    else
    {
        Vendor = Product = Revision = _T("Unknown");
    }

    BusAddress.Format(_T("%c:"), m_DeviceLetters[m_CurrentDevice]);
}

void CSptiDriver::ExecuteCommand(SRB_ExecSCSICmd& cmd)
{
    cmd.SRB_Status = SS_ERR;
    std::memset(cmd.SenseArea, 0, sizeof(cmd.SenseArea));

    if (!m_Device)
        return;

    // Build a local spti to avoid races/overwrites.
    SPTD_WITH_SENSE spti;
    std::memset(&spti, 0, sizeof(spti));

    spti.sptd.Length = sizeof(SCSI_PASS_THROUGH_DIRECT);
    spti.sptd.CdbLength = cmd.SRB_CDBLen;
    spti.sptd.SenseInfoLength = (UCHAR)sizeof(spti.Sense);
    spti.sptd.SenseInfoOffset = (ULONG)offsetof(SPTD_WITH_SENSE, Sense);
    spti.sptd.DataTransferLength = cmd.SRB_BufLen;
    spti.sptd.TimeOutValue = 0xFFFF; // keep legacy huge timeout unless you want to tune per-op

    std::memcpy(spti.sptd.Cdb, cmd.CDBByte, sizeof(spti.sptd.Cdb));
    spti.sptd.DataBuffer = cmd.SRB_BufPointer;

    if (cmd.SRB_Flags & SRB_DIR_IN)
        spti.sptd.DataIn = SCSI_IOCTL_DATA_IN;
    else if (cmd.SRB_Flags & SRB_DIR_OUT)
        spti.sptd.DataIn = SCSI_IOCTL_DATA_OUT;
    else
        spti.sptd.DataIn = SCSI_IOCTL_DATA_UNSPECIFIED;

    const BYTE op = spti.sptd.Cdb[0];
    const bool writeLike = IsWriteLikeOpcode(op);

    DWORD bytesReturned = 0;
    BOOL ok = DoSptiIoctl(m_Device.get(), spti, bytesReturned);
    DWORD gle = ok ? 0 : ::GetLastError();

    LogSptiResult("sync", spti, ok, gle);

    if (!ok)
        return;

    // If device says CHECK CONDITION, decode sense and optionally wait/retry.
    if (spti.sptd.ScsiStatus == 0x02)
    {
        // Compatibility: some drives return 05/24/00 on READ TOC when blank.
        if (IsReadTocInvalidFieldCompat(op, spti.Sense))
        {
#ifdef SPTI_DEBUG_LOG
            dbgprintf("[compat] op=43 got 05/24/00 (READ TOC invalid field) treating as SS_COMP.\n");
#endif
            cmd.SRB_Status = SS_COMP;
        }
        else if (writeLike && ShouldWaitAndRetryOnSense(spti.Sense))
        {
            using namespace std::chrono;
            const auto kStep = 200ms;
            const auto kMaxWait = 5min;

            auto waited = 0ms;
            while (waited < kMaxWait)
            {
                ::Sleep((DWORD)kStep.count());
                waited += kStep;

                std::memset(spti.Sense, 0, sizeof(spti.Sense));
                ok = DoSptiIoctl(m_Device.get(), spti, bytesReturned);
                gle = ok ? 0 : ::GetLastError();

                LogSptiResult("retry", spti, ok, gle);

                if (!ok)
                    return;

                if (spti.sptd.ScsiStatus == 0x00)
                    break;

                if (!(spti.sptd.ScsiStatus == 0x02 && ShouldWaitAndRetryOnSense(spti.Sense)))
                    break;
            }
        }
    }

    // success only if GOOD (or compat override set above)
    if (cmd.SRB_Status != SS_COMP)
    {
        if (spti.sptd.ScsiStatus == 0x00)
            cmd.SRB_Status = SS_COMP;
        else
            cmd.SRB_Status = SS_ERR;
    }

    // Copy sense to caller (best-effort, bounded)
    const size_t toCopy = (sizeof(cmd.SenseArea) < sizeof(spti.Sense)) ? sizeof(cmd.SenseArea) : sizeof(spti.Sense);
    std::memcpy(cmd.SenseArea, spti.Sense, toCopy);

    // Keep last status snapshot
    std::memcpy(&m_Spti, &spti, sizeof(spti));
    m_Status = ok;
}

void CSptiDriver::InitialAsync()
{
    // For compatibility with older pipeline code:
    // - signal "ready" so writer can start issuing writes
    if (m_WaitEvent) ::SetEvent(m_WaitEvent.get());
}

void CSptiDriver::FinalizeAsync()
{
    // If you later implement real async pipelining using the thread,
    // this is where you'd wait for completion.
    if (m_WaitEvent) ::WaitForSingleObject(m_WaitEvent.get(), INFINITE);
}

bool CSptiDriver::ExecuteCommandAsync(SRB_ExecSCSICmd& cmd)
{
    // Safe “pseudo-async”: execute synchronously but keep the async API.
    // This avoids broken SRB_Status checks in callers while keeping behavior stable.
    ExecuteCommand(cmd);
    return (cmd.SRB_Status == SS_COMP);
}

// ------------------------------
// thread (kept for compatibility; not required by pseudo-async path)
// ------------------------------
DWORD WINAPI CSptiDriver::thread_proc(LPVOID p)
{
    CSptiDriver* d = static_cast<CSptiDriver*>(p);
    DWORD bytesReturned = 0;
    SPTD_WITH_SENSE localSpti;

    while (true)
    {
        ::WaitForSingleObject(d->m_CommandEvent.get(), INFINITE);

        if (d->m_ExitFlag.load(std::memory_order_relaxed))
            break;

        std::memcpy(&localSpti, &d->m_Spti, sizeof(localSpti));

        BOOL ok = DoSptiIoctl(d->m_Device.get(), localSpti, bytesReturned);
        DWORD gle = ok ? 0 : ::GetLastError();
        d->m_Status = ok;

        LogSptiResult("thread", localSpti, ok, gle);

        // same wait/retry policy for write-like ops
        const BYTE op = localSpti.sptd.Cdb[0];
        if (ok &&
            localSpti.sptd.ScsiStatus == 0x02 &&
            IsWriteLikeOpcode(op) &&
            ShouldWaitAndRetryOnSense(localSpti.Sense))
        {
            using namespace std::chrono;
            const auto kStep = 200ms;
            const auto kMaxWait = 5min;

            auto waited = 0ms;
            while (waited < kMaxWait)
            {
                ::Sleep((DWORD)kStep.count());
                waited += kStep;

                std::memset(localSpti.Sense, 0, sizeof(localSpti.Sense));
                ok = DoSptiIoctl(d->m_Device.get(), localSpti, bytesReturned);
                gle = ok ? 0 : ::GetLastError();
                d->m_Status = ok;

                LogSptiResult("thread-retry", localSpti, ok, gle);

                if (!ok)
                    break;

                if (localSpti.sptd.ScsiStatus == 0x00)
                    break;

                if (!(localSpti.sptd.ScsiStatus == 0x02 && ShouldWaitAndRetryOnSense(localSpti.Sense)))
                    break;
            }
        }

        std::memcpy(&d->m_Spti, &localSpti, sizeof(localSpti));
        ::SetEvent(d->m_WaitEvent.get());
    }

    return 0;
}

bool CSptiDriver::IsDriveReady()
{
    if (!m_Device)
        return false;

    BYTE cdb[6] = { 0x00, 0, 0, 0, 0, 0 }; // TEST UNIT READY
    SCSI_PASS_THROUGH_DIRECT sptd = {};
    sptd.Length = sizeof(SCSI_PASS_THROUGH_DIRECT);
    sptd.CdbLength = sizeof(cdb);
    std::memcpy(sptd.Cdb, cdb, sizeof(cdb));
    sptd.DataIn = SCSI_IOCTL_DATA_UNSPECIFIED;
    sptd.TimeOutValue = 10; // seconds
    sptd.DataTransferLength = 0;
    sptd.DataBuffer = nullptr;

    DWORD returned = 0;
    BOOL ok = ::DeviceIoControl(
        m_Device.get(),
        IOCTL_SCSI_PASS_THROUGH_DIRECT,
        &sptd,
        sizeof(sptd),
        &sptd,
        sizeof(sptd),
        &returned,
        NULL);

    return ok && sptd.ScsiStatus == 0x00;
}

void CSptiDriver::GetDiscInformation(CString& discInfo)
{
    discInfo.Empty();

    if (m_CurrentDevice < 0 || !m_Device)
    {
        discInfo = _T("Unknown");
        return;
    }

    SRB_ExecSCSICmd cmd;
    std::memset(&cmd, 0, sizeof(cmd));

    BYTE buf[36] = {};
    cmd.SRB_BufLen = sizeof(buf);
    cmd.SRB_BufPointer = buf;
    cmd.SRB_Flags = SRB_DIR_IN;
    cmd.SRB_CDBLen = 10;
    cmd.CDBByte[0] = 0x43; // READ TOC/PMA/ATIP
    cmd.CDBByte[1] = 0x00; // MSF=0, Format=0 (TOC)
    cmd.CDBByte[7] = 0x00;
    cmd.CDBByte[8] = 0x24; // 36 bytes

    ExecuteCommand(cmd);

    if (cmd.SRB_Status == SS_COMP)
    {
        CStringA infoA((LPCSTR)(buf + 10), 26);
        infoA.TrimRight();
        discInfo = CString(infoA);
    }
    else
    {
        discInfo = _T("Unknown");
    }
}
