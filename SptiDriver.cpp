#include "StdAfx.h"
#include "SptiDriver.h"
#include "Setting.h"

#include <VersionHelpers.h>
#include <chrono>
#include <cstring>

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

static void DecodeSenseFixed(const BYTE* s, int& key, int& asc, int& ascq)
{
    key = s[2] & 0x0F;
    asc = s[12];
    ascq = s[13];
}

// SK=02 ASC=04 ASCQ=08 (LOGICAL UNIT NOT READY / LONG WRITE IN PROGRESS)
static bool IsLongWriteInProgress(const BYTE* s)
{
    if (!IsSenseValid(s)) return false;
    int k, a, q;
    DecodeSenseFixed(s, k, a, q);
    return (k == 0x02 && a == 0x04 && q == 0x08);
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
        return true;
    default:
        return false;
    }
}

// ------------------------------
// thread
// ------------------------------
DWORD WINAPI CSptiDriver::thread_proc(LPVOID p)
{
    CSptiDriver* d = static_cast<CSptiDriver*>(p);
    DWORD bytesReturned = 0;

    while (true)
    {
        ::WaitForSingleObject(d->m_CommandEvent.get(), INFINITE);

        if (d->m_ExitFlag.load(std::memory_order_relaxed))
            break;

        d->m_Status = ::DeviceIoControl(
            d->m_Device.get(),
            IOCTL_SCSI_PASS_THROUGH_DIRECT,
            &d->m_Spti, sizeof(d->m_Spti),
            &d->m_Spti, sizeof(d->m_Spti),
            &bytesReturned,
            NULL);

        // Retry "long write in progress" for write-like ops
        if (d->m_Status &&
            d->m_Spti.sptd.ScsiStatus == 0x02 &&
            IsLongWriteInProgress(d->m_Spti.Sense) &&
            IsWriteLikeOpcode(d->m_Spti.sptd.Cdb[0]))
        {
            using namespace std::chrono;

            const auto kMaxWait = 5min;
            const auto kStep = 200ms;

            auto waited = 0ms;
            while (waited < kMaxWait)
            {
                ::Sleep((DWORD)kStep.count());
                waited += kStep;

                d->m_Status = ::DeviceIoControl(
                    d->m_Device.get(),
                    IOCTL_SCSI_PASS_THROUGH_DIRECT,
                    &d->m_Spti, sizeof(d->m_Spti),
                    &d->m_Spti, sizeof(d->m_Spti),
                    &bytesReturned,
                    NULL);

                if (d->m_Status && d->m_Spti.sptd.ScsiStatus == 0)
                    break;
            }
        }

        ::SetEvent(d->m_WaitEvent.get());
    }

    return 0;
}

// ------------------------------
// CSptiDriver
// ------------------------------
CSptiDriver::CSptiDriver()
{
    m_WaitEvent.reset(::CreateEvent(NULL, FALSE, TRUE, NULL));
    m_CommandEvent.reset(::CreateEvent(NULL, FALSE, FALSE, NULL));

    m_ExitFlag.store(false, std::memory_order_relaxed);
    m_Status = FALSE;

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
BOOL CSptiDriver::IsActive() { return IsWindowsNT() ? TRUE : FALSE; }

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
    // \\.\D:
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

    cmd.CDBByte[0] = 0x12;           // INQUIRY
    cmd.CDBByte[4] = (BYTE)sizeof(buf);

    ExecuteCommand(cmd);

    if (cmd.SRB_Status == SS_COMP)
    {
        CStringA vA((LPCSTR)(buf + 8), 8);
        CStringA pA((LPCSTR)(buf + 16), 16);
        CStringA rA((LPCSTR)(buf + 32), 4);

        vA.TrimRight();
        pA.TrimRight();
        rA.TrimRight();

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

    std::memset(&m_Spti, 0, sizeof(m_Spti));

    m_Spti.sptd.Length = sizeof(SCSI_PASS_THROUGH_DIRECT);
    m_Spti.sptd.CdbLength = cmd.SRB_CDBLen;
    m_Spti.sptd.SenseInfoLength = (UCHAR)sizeof(m_Spti.Sense);
    m_Spti.sptd.SenseInfoOffset = (ULONG)offsetof(SPTD_WITH_SENSE, Sense);
    m_Spti.sptd.DataTransferLength = cmd.SRB_BufLen;

    // Keep legacy timeout behavior (very long); if you want this configurable, say so.
    m_Spti.sptd.TimeOutValue = 0xFFFF;

    std::memcpy(m_Spti.sptd.Cdb, cmd.CDBByte, sizeof(m_Spti.sptd.Cdb));
    m_Spti.sptd.DataBuffer = cmd.SRB_BufPointer;

    if (cmd.SRB_Flags & SRB_DIR_IN)
        m_Spti.sptd.DataIn = SCSI_IOCTL_DATA_IN;
    else if (cmd.SRB_Flags & SRB_DIR_OUT)
        m_Spti.sptd.DataIn = SCSI_IOCTL_DATA_OUT;
    else
        m_Spti.sptd.DataIn = SCSI_IOCTL_DATA_UNSPECIFIED;

    DWORD bytesReturned = 0;
    const BOOL ok = ::DeviceIoControl(
        m_Device.get(),
        IOCTL_SCSI_PASS_THROUGH_DIRECT,
        &m_Spti, sizeof(m_Spti),
        &m_Spti, sizeof(m_Spti),
        &bytesReturned,
        NULL);

    if (!ok)
        return;

    // status 0 == GOOD
    if (m_Spti.sptd.ScsiStatus == 0)
        cmd.SRB_Status = SS_COMP;

    // Copy sense into caller buffer (best-effort, avoids breaking old callers)
    std::memcpy(cmd.SenseArea, m_Spti.Sense, (sizeof(cmd.SenseArea) < sizeof(m_Spti.Sense)) ? sizeof(cmd.SenseArea) : sizeof(m_Spti.Sense));
}

void CSptiDriver::InitialAsync()
{
    // Option 2: keep API but do not change execution semantics.
    if (m_WaitEvent) ::SetEvent(m_WaitEvent.get());
}

void CSptiDriver::FinalizeAsync()
{
    if (m_WaitEvent) ::WaitForSingleObject(m_WaitEvent.get(), INFINITE);
}

bool CSptiDriver::ExecuteCommandAsync(SRB_ExecSCSICmd& /*cmd*/)
{
    // Option 2: keep API but explicitly not supported here (call ExecuteCommand instead).
    return false;
}
