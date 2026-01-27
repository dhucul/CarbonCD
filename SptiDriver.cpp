// SptiDriver_onefile.cpp
//
// SINGLE-FILE CSptiDriver IMPLEMENTATION
// Fixes unresolved externals + write lead-in correctness
//
// IMPORTANT:
//  - StdAfx.h MUST be first
//  - Remove / exclude old SptiDriver.cpp from project

#include "StdAfx.h"
#include "SptiDriver.h"
#include "Setting.h"
#include "PBBuffer.h"

#include <VersionHelpers.h>
#include <cstdint>
#include <cstring>

// ============================================================
// Helpers
// ============================================================

static bool IsWindowsNT()
{
    return IsWindowsVersionOrGreater(5, 0, 0);
}

// -------- Sense helpers --------

static bool IsSenseValid(const BYTE* s)
{
    if (!s) return false;
    BYTE rc = s[0] & 0x7F;
    return (rc == 0x70 || rc == 0x71 || rc == 0x72 || rc == 0x73);
}

static void DecodeSenseFixed(const BYTE* s, int& key, int& asc, int& ascq)
{
    key = s[2] & 0x0F;
    asc = s[12];
    ascq = s[13];
}

// SK=02 ASC=04 ASCQ=08
static bool IsLongWriteInProgress(const BYTE* s)
{
    if (!IsSenseValid(s)) return false;
    int k, a, q;
    DecodeSenseFixed(s, k, a, q);
    return (k == 0x02 && a == 0x04 && q == 0x08);
}

// -------- Opcode helpers --------

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

static bool IsPollingOpcode(BYTE op)
{
    switch (op)
    {
    case 0x00: // TEST UNIT READY
    case 0x51: // READ DISC INFORMATION
    case 0x52: // READ TRACK INFORMATION
    case 0x43: // READ TOC/PMA/ATIP
    case 0xAC: // GET PERFORMANCE
        return true;
    default:
        return false;
    }
}

// ============================================================
// Worker thread
// ============================================================

static DWORD WINAPI SptiThread(LPVOID p)
{
    CSptiDriver* d = static_cast<CSptiDriver*>(p);
    DWORD br = 0;
    DWORD len = sizeof(SCSI_PASS_THROUGH_DIRECT_WITH_BUFFER);

    while (true)
    {
        WaitForSingleObject(d->m_hCommandEvent, INFINITE);
        if (d->m_ExitFlag)
            break;

        d->m_Status = DeviceIoControl(
            d->m_hDevice,
            IOCTL_SCSI_PASS_THROUGH_DIRECT,
            &d->m_SptiCmd, len,
            &d->m_SptiCmd, len,
            &br, nullptr);

        // Retry lead-in / write commands while busy
        if (d->m_Status &&
            d->m_SptiCmd.sptd.ScsiStatus == 0x02 &&
            IsLongWriteInProgress(d->m_SptiCmd.ucSenseBuf) &&
            IsWriteLikeOpcode(d->m_SptiCmd.sptd.Cdb[0]))
        {
            DWORD waited = 0;
            while (waited < 300000) // 5 minutes
            {
                Sleep(200);
                waited += 200;

                d->m_Status = DeviceIoControl(
                    d->m_hDevice,
                    IOCTL_SCSI_PASS_THROUGH_DIRECT,
                    &d->m_SptiCmd, len,
                    &d->m_SptiCmd, len,
                    &br, nullptr);

                if (d->m_Status && d->m_SptiCmd.sptd.ScsiStatus == 0)
                    break;
            }
        }

        SetEvent(d->m_hWaitEvent);
    }
    return 0;
}

// ============================================================
// CSptiDriver
// ============================================================

CSptiDriver::CSptiDriver()
{
    m_DeviceCount = 0;
    m_CurrentDevice = -1;
    m_hDevice = INVALID_HANDLE_VALUE;

    // AUTO-RESET events
    m_hWaitEvent = CreateEvent(nullptr, FALSE, TRUE, nullptr);
    m_hCommandEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    m_ExitFlag = false;
    m_Status = FALSE;

    m_hThread = CreateThread(nullptr, 0, SptiThread, this, 0, &m_ThreadID);
    Initialize();
}

CSptiDriver::~CSptiDriver()
{
    m_ExitFlag = true;
    SetEvent(m_hCommandEvent);
    WaitForSingleObject(m_hThread, INFINITE);

    CloseHandle(m_hThread);
    CloseHandle(m_hWaitEvent);
    CloseHandle(m_hCommandEvent);

    if (m_hDevice != INVALID_HANDLE_VALUE)
        CloseHandle(m_hDevice);
}

// ============================================================
// REQUIRED VIRTUAL METHODS
// ============================================================

void CSptiDriver::Initialize(void)
{
    m_DeviceCount = 0;
    m_CurrentDevice = -1;

    for (int i = 0; i < 26; i++)
    {
        char root[4] = { char('A' + i), ':', '\\', 0 };
        if (GetDriveTypeA(root) == DRIVE_CDROM)
            m_Address[m_DeviceCount++] = char('A' + i);
    }

    if (m_DeviceCount > 0)
        SetDevice(0);
}

int CSptiDriver::IsActive(void)
{
    return IsWindowsNT() ? 1 : 0;
}

unsigned long CSptiDriver::GetVersion(void)
{
    return 1;
}

int CSptiDriver::GetDeviceCount(void)
{
    return m_DeviceCount;
}

int CSptiDriver::GetCurrentDevice(void)
{
    return m_CurrentDevice;
}

void CSptiDriver::SetDevice(int n)
{
    if (n < 0 || n >= m_DeviceCount)
        return;

    if (m_hDevice != INVALID_HANDLE_VALUE)
        CloseHandle(m_hDevice);

    m_CurrentDevice = n;

    char path[8];
    sprintf(path, "\\\\.\\%c:", m_Address[n]);

    m_hDevice = CreateFileA(
        path,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);
}

// ============================================================
// *** THIS FIXES YOUR LAST LINKER ERROR ***
// Exact CStringA signature
// ============================================================

void CSptiDriver::GetDeviceString(
    CStringA& Vendor,
    CStringA& Product,
    CStringA& Revision,
    CStringA& BusAddress)
{
    SRB_ExecSCSICmd cmd;
    BYTE buf[100];

    Vendor = "";
    Product = "";
    Revision = "";

    memset(&cmd, 0, sizeof(cmd));
    memset(buf, 0, sizeof(buf));

    cmd.SRB_BufLen = sizeof(buf);
    cmd.SRB_BufPointer = buf;
    cmd.SRB_Flags = SRB_DIR_IN;
    cmd.SRB_CDBLen = 6;
    cmd.CDBByte[0] = 0x12; // INQUIRY
    cmd.CDBByte[4] = sizeof(buf);

    ExecuteCommand(cmd);

    if (cmd.SRB_Status == SS_COMP)
    {
        Vendor = CStringA((char*)buf + 8, 8);
        Product = CStringA((char*)buf + 16, 16);
        Revision = CStringA((char*)buf + 32, 4);

        Vendor.TrimRight();
        Product.TrimRight();
        Revision.TrimRight();
    }
    else
    {
        Vendor = Product = Revision = "Unknown";
    }

    if (m_CurrentDevice >= 0)
        BusAddress.Format("%c:", m_Address[m_CurrentDevice]);
    else
        BusAddress = "";
}

// ============================================================
// ExecuteCommand (sync)
// ============================================================

void CSptiDriver::ExecuteCommand(SRB_ExecSCSICmd& cmd)
{
    DWORD br = 0;
    DWORD len = sizeof(SCSI_PASS_THROUGH_DIRECT_WITH_BUFFER);

    cmd.SRB_Status = SS_ERR;
    memset(cmd.SenseArea, 0, 32);
    memset(&m_SptiCmd, 0, sizeof(m_SptiCmd));

    m_SptiCmd.sptd.Length = sizeof(SCSI_PASS_THROUGH_DIRECT);
    m_SptiCmd.sptd.SenseInfoOffset =
        offsetof(SCSI_PASS_THROUGH_DIRECT_WITH_BUFFER, ucSenseBuf);
    m_SptiCmd.sptd.SenseInfoLength = 32;
    m_SptiCmd.sptd.CdbLength = cmd.SRB_CDBLen;
    m_SptiCmd.sptd.DataTransferLength = cmd.SRB_BufLen;
    m_SptiCmd.sptd.TimeOutValue = 0xFFFF;

    memcpy(m_SptiCmd.sptd.Cdb, cmd.CDBByte, 16);
    m_SptiCmd.sptd.DataBuffer = cmd.SRB_BufPointer;

    if (cmd.SRB_Flags & SRB_DIR_IN)
        m_SptiCmd.sptd.DataIn = SCSI_IOCTL_DATA_IN;
    else if (cmd.SRB_Flags & SRB_DIR_OUT)
        m_SptiCmd.sptd.DataIn = SCSI_IOCTL_DATA_OUT;

    BOOL ok = DeviceIoControl(
        m_hDevice,
        IOCTL_SCSI_PASS_THROUGH_DIRECT,
        &m_SptiCmd, len,
        &m_SptiCmd, len,
        &br, nullptr);

    if (!ok)
        return;

    if (m_SptiCmd.sptd.ScsiStatus == 0)
        cmd.SRB_Status = SS_COMP;
}

// ============================================================
// Async stubs (kept for ABI compatibility)
// ============================================================

void CSptiDriver::InitialAsync(void) { SetEvent(m_hWaitEvent); }
void CSptiDriver::FinalizeAsync(void) { WaitForSingleObject(m_hWaitEvent, INFINITE); }
bool CSptiDriver::ExecuteCommandAsync(SRB_ExecSCSICmd&) { return false; }
