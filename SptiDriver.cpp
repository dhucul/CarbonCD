// SptiDriver_onefile_choiceA.cpp
// Choice A: ONE FILE for CSptiDriver implementation, but uses your existing Aspi/PBBuffer
// (so you MUST keep compiling Aspi.cpp and PBBuffer.cpp; do NOT redefine CAspi/CPBBuffer here)
//
// IMPORTANT:
// - Keep PCH enabled: StdAfx.h must be FIRST.
// - This file should replace your old SptiDriver.cpp (or you can exclude the old one from build).

#include "StdAfx.h"          // must be first with /Yu

#include "SptiDriver.h"      // your header (contains CSptiDriver class + SPTI structs/macros)
#include "Setting.h"         // if you need it (you had it)
#include "PBBuffer.h"        // already included by SptiDriver.h, but harmless

#include <VersionHelpers.h>
#include <cstdint>
#include <cstring>

static bool IsWindowsNT()
{
    return IsWindowsVersionOrGreater(5, 0, 0); // Win2K+ NT-based
}

static DWORD WINAPI SptiThread(LPVOID Thread)
{
    auto spti = static_cast<CSptiDriver*>(Thread);
    DWORD BytesReturned = 0;
    DWORD CommandLength = sizeof(SCSI_PASS_THROUGH_DIRECT_WITH_BUFFER);

    while (true)
    {
        WaitForSingleObject(spti->m_hCommandEvent, INFINITE);
        ResetEvent(spti->m_hCommandEvent);

        if (spti->m_ExitFlag)
            break;

        spti->m_Status = DeviceIoControl(
            spti->m_hDevice,
            IOCTL_SCSI_PASS_THROUGH_DIRECT,
            &(spti->m_SptiCmd),
            CommandLength,
            &(spti->m_SptiCmd),
            CommandLength,
            &BytesReturned,
            nullptr);

        SetEvent(spti->m_hWaitEvent);
    }

    return 0;
}

CSptiDriver::CSptiDriver()
{
    m_DeviceCount = 0;
    m_hDevice = INVALID_HANDLE_VALUE;

    m_hWaitEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    m_hCommandEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);

    m_ExitFlag = false;
    m_hThread = CreateThread(nullptr, 0, SptiThread, this, 0, &m_ThreadID);

    // FIX: CreateThread returns NULL on failure
    if (m_hThread == nullptr)
        return;

    SetThreadPriority(m_hThread, THREAD_PRIORITY_NORMAL);
    Initialize();
}

CSptiDriver::~CSptiDriver()
{
    if (m_hThread != nullptr)
    {
        m_ExitFlag = true;
        SetEvent(m_hCommandEvent);
        WaitForSingleObject(m_hThread, INFINITE);
        CloseHandle(m_hThread);
        m_hThread = nullptr;
    }

    if (m_hDevice != INVALID_HANDLE_VALUE)
    {
        CloseHandle(m_hDevice);
        m_hDevice = INVALID_HANDLE_VALUE;
    }

    if (m_hWaitEvent != nullptr)
    {
        CloseHandle(m_hWaitEvent);
        m_hWaitEvent = nullptr;
    }

    if (m_hCommandEvent != nullptr)
    {
        CloseHandle(m_hCommandEvent);
        m_hCommandEvent = nullptr;
    }
}

void CSptiDriver::Initialize()
{
    m_DeviceCount = 0;

    for (int i = 0; i < 27; i++)
    {
        CString cs;
        cs.Format(_T("%c:\\"), i + 'A');

        if (GetDriveType(cs) == DRIVE_CDROM)
        {
            m_Address[m_DeviceCount] = static_cast<char>(i + 'A');
            m_DeviceCount++;
        }
    }

    // FIX: avoid underflow when there are no CDROM drives
    if (m_DeviceCount > 0)
        SetDevice(0);
    else
        m_CurrentDevice = -1;
}

BOOL CSptiDriver::IsActive()
{
    return IsWindowsNT() ? TRUE : FALSE;
}

DWORD CSptiDriver::GetVersion()
{
    return 1;
}

void CSptiDriver::ExecuteCommand(SRB_ExecSCSICmd& cmd)
{
    DWORD BytesReturned = 0;
    DWORD CommandLength = sizeof(SCSI_PASS_THROUGH_DIRECT_WITH_BUFFER);
    BOOL Status;

    std::memset(&m_SptiCmd, 0, sizeof(SCSI_PASS_THROUGH_DIRECT_WITH_BUFFER));

    m_SptiCmd.sptd.Length = sizeof(SCSI_PASS_THROUGH_DIRECT);
    m_SptiCmd.sptd.PathId = 0;
    m_SptiCmd.sptd.TargetId = 0;
    m_SptiCmd.sptd.Lun = 0;
    m_SptiCmd.sptd.SenseInfoOffset = offsetof(SCSI_PASS_THROUGH_DIRECT_WITH_BUFFER, ucSenseBuf);
    m_SptiCmd.sptd.SenseInfoLength = 32;

    if (cmd.SRB_Flags & SRB_DIR_IN)
        m_SptiCmd.sptd.DataIn = SCSI_IOCTL_DATA_IN;
    else if (cmd.SRB_Flags & SRB_DIR_OUT)
        m_SptiCmd.sptd.DataIn = SCSI_IOCTL_DATA_OUT;
    else
        m_SptiCmd.sptd.DataIn = SCSI_IOCTL_DATA_UNSPECIFIED;

    m_SptiCmd.sptd.DataTransferLength = cmd.SRB_BufLen;
    m_SptiCmd.sptd.TimeOutValue = 0xffff;
    m_SptiCmd.sptd.CdbLength = cmd.SRB_CDBLen;

    // keep original behavior: copy 16 bytes
    std::memcpy(&(m_SptiCmd.sptd.Cdb), &(cmd.CDBByte), 16);

    // FIX: 64-bit safe pointer alignment check
    const bool aligned16 = (((uintptr_t)cmd.SRB_BufPointer & 0x0F) == 0);

    if (aligned16)
    {
        m_SptiCmd.sptd.DataBuffer = cmd.SRB_BufPointer;
    }
    else
    {
        if (m_Buffer.GetBufferSize() < cmd.SRB_BufLen)
        {
            m_Buffer.CreateBuffer(cmd.SRB_BufLen);
        }

        m_SptiCmd.sptd.DataBuffer = m_Buffer.GetBuffer();

        if (cmd.SRB_Flags & SRB_DIR_OUT)
        {
            std::memcpy(m_Buffer.GetBuffer(), cmd.SRB_BufPointer, cmd.SRB_BufLen);
        }
    }

    Status = DeviceIoControl(
        m_hDevice,
        IOCTL_SCSI_PASS_THROUGH_DIRECT,
        &m_SptiCmd, CommandLength,
        &m_SptiCmd, CommandLength,
        &BytesReturned,
        nullptr);

    if (Status && m_SptiCmd.sptd.ScsiStatus == 0)
    {
        cmd.SRB_Status = SS_COMP;
    }

    if (!aligned16)
    {
        if (cmd.SRB_Flags & SRB_DIR_IN)
        {
            std::memcpy(cmd.SRB_BufPointer, m_Buffer.GetBuffer(), cmd.SRB_BufLen);
        }
    }

    std::memcpy(cmd.SenseArea, m_SptiCmd.ucSenseBuf, 32);
}

void CSptiDriver::InitialAsync(void)
{
    SetEvent(m_hWaitEvent);
    ResetEvent(m_hCommandEvent);

    std::memset(&m_SptiCmd, 0, sizeof(SCSI_PASS_THROUGH_DIRECT_WITH_BUFFER));
    m_Status = TRUE;
    m_SptiCmd.sptd.ScsiStatus = 0;
    m_SptiCmd.sptd.DataIn = SCSI_IOCTL_DATA_UNSPECIFIED;
    debug = 0;
}

void CSptiDriver::FinalizeAsync(void)
{
    WaitForSingleObject(m_hWaitEvent, INFINITE);
    m_Status = TRUE;
    m_SptiCmd.sptd.ScsiStatus = 0;
}

bool CSptiDriver::ExecuteCommandAsync(SRB_ExecSCSICmd& cmd)
{
    WaitForSingleObject(m_hWaitEvent, INFINITE);

    // report status of previous command
    if (m_Status && m_SptiCmd.sptd.ScsiStatus == 0)
    {
        cmd.SRB_Status = SS_COMP;
        std::memcpy(cmd.SenseArea, m_SptiCmd.ucSenseBuf, 32);
    }
    else
    {
        std::memcpy(cmd.SenseArea, m_SptiCmd.ucSenseBuf, 32);
        return false;
    }

    debug++;

    std::memset(&m_SptiCmd, 0, sizeof(SCSI_PASS_THROUGH_DIRECT_WITH_BUFFER));
    m_SptiCmd.sptd.Length = sizeof(SCSI_PASS_THROUGH_DIRECT);
    m_SptiCmd.sptd.PathId = 0;
    m_SptiCmd.sptd.TargetId = 0;
    m_SptiCmd.sptd.Lun = 0;
    m_SptiCmd.sptd.SenseInfoOffset = offsetof(SCSI_PASS_THROUGH_DIRECT_WITH_BUFFER, ucSenseBuf);
    m_SptiCmd.sptd.SenseInfoLength = 32;

    if (cmd.SRB_Flags & SRB_DIR_IN)
        m_SptiCmd.sptd.DataIn = SCSI_IOCTL_DATA_IN;
    else if (cmd.SRB_Flags & SRB_DIR_OUT)
        m_SptiCmd.sptd.DataIn = SCSI_IOCTL_DATA_OUT;
    else
        m_SptiCmd.sptd.DataIn = SCSI_IOCTL_DATA_UNSPECIFIED;

    m_SptiCmd.sptd.DataTransferLength = cmd.SRB_BufLen;
    m_SptiCmd.sptd.TimeOutValue = 0xffff;
    m_SptiCmd.sptd.CdbLength = cmd.SRB_CDBLen;

    std::memcpy(&(m_SptiCmd.sptd.Cdb), &(cmd.CDBByte), 16);

    // NOTE: Keeping your original async behavior (no bounce buffer).
    // If you want the async path to support unaligned buffers like sync does,
    // we need to add a few tracking fields to CSptiDriver (header change),
    // or switch to a different async strategy.

    m_SptiCmd.sptd.DataBuffer = cmd.SRB_BufPointer;

    ResetEvent(m_hWaitEvent);
    SetEvent(m_hCommandEvent);
    return true;
}

int CSptiDriver::GetDeviceCount(void)
{
    return m_DeviceCount;
}

void CSptiDriver::SetDevice(int DeviceNo)
{
    CString cs;

    // FIX: handle no devices
    if (m_DeviceCount <= 0)
    {
        m_CurrentDevice = -1;
        if (m_hDevice != INVALID_HANDLE_VALUE)
        {
            CloseHandle(m_hDevice);
            m_hDevice = INVALID_HANDLE_VALUE;
        }
        return;
    }

    if (DeviceNo >= m_DeviceCount)
        DeviceNo = m_DeviceCount - 1;
    if (DeviceNo < 0)
        DeviceNo = 0;

    m_CurrentDevice = DeviceNo;

    if (m_hDevice != INVALID_HANDLE_VALUE)
    {
        CloseHandle(m_hDevice);
        m_hDevice = INVALID_HANDLE_VALUE;
    }

    cs.Format(_T("\\\\.\\%c:"), m_Address[m_CurrentDevice]);
    m_hDevice = CreateFile(cs, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
}

void CSptiDriver::GetDeviceString(CString& Vendor, CString& Product, CString& Revision, CString& BusAddress)
{
    SRB_ExecSCSICmd cmd;
    BYTE Buffer[100];

    Vendor = _T("");
    Product = _T("");
    Revision = _T("");

    std::memset(&cmd, 0, sizeof(cmd));
    std::memset(Buffer, 0, 100);

    cmd.SRB_BufLen = 100;
    cmd.SRB_BufPointer = Buffer;
    cmd.SRB_Flags = SRB_DIR_IN;
    cmd.SRB_CDBLen = 6;
    cmd.CDBByte[0] = 0x12; // INQUIRY
    cmd.CDBByte[4] = 100;

    ExecuteCommand(cmd);

    if (cmd.SRB_Status == SS_COMP)
    {
        char product[17], revision[5], vendor[9];

        std::memcpy(product, Buffer + 16, 16);
        std::memcpy(revision, Buffer + 32, 4);
        std::memcpy(vendor, Buffer + 8, 8);

        product[16] = '\0';
        revision[4] = '\0';
        vendor[8] = '\0';

        Product = product;
        Revision = revision;
        Vendor = vendor;

        Product.TrimRight();
        Revision.TrimRight();
        Vendor.TrimRight();
    }
    else
    {
        Product = _T("Unknown");
        Revision = _T("-");
        Vendor = _T("-");
    }

    BusAddress.Format(_T("%c:"), m_Address[m_CurrentDevice]);
}

int CSptiDriver::GetCurrentDevice(void)
{
    return m_CurrentDevice;
}
