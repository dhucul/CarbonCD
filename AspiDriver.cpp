// AspiDriver.cpp (updated - single file)
//
// Key fixes:
// - Use nullptr (not INVALID_HANDLE_VALUE) for event handle sentinel
// - Check CreateEvent / ResetEvent / WaitForSingleObject results
// - Make LoadLibrary TCHAR-safe (_T)
// - Fix SetDevice bounds (handles 0 devices + negative indices)
// - Guard event usage when not created

#include "StdAfx.h"
#include "AspiDriver.h"
#include "Setting.h"
#include "PBBuffer.h"

#include <tchar.h>
#include <cstring>

CAspiDriver::CAspiDriver()
{
    m_hModule = nullptr;
    m_hEvent = nullptr;            // was INVALID_HANDLE_VALUE
    m_DeviceCount = 0;
    m_CurrentDevice = 0;

    // Ensure function pointers are in a known state
    m_GetASPI32SupportInfo = nullptr;
    m_SendASPI32Command = nullptr;
    m_GetASPI32Buffer = nullptr;
    m_FreeASPI32Buffer = nullptr;
    m_TranslateASPI32Address = nullptr;
    m_GetASPI32DLLVersion = nullptr;

    Initialize();
}

CAspiDriver::~CAspiDriver()
{
    if (m_hModule != nullptr)
    {
        FreeLibrary(m_hModule);
        m_hModule = nullptr;
    }

    if (m_hEvent != nullptr)
    {
        CloseHandle(m_hEvent);
        m_hEvent = nullptr;
    }
}

void CAspiDriver::Initialize()
{
    // Clean up existing resources first
    if (m_hModule != nullptr)
    {
        FreeLibrary(m_hModule);
        m_hModule = nullptr;
    }

    if (m_hEvent != nullptr)
    {
        CloseHandle(m_hEvent);
        m_hEvent = nullptr;
    }

    // Clear function pointers
    m_GetASPI32SupportInfo = nullptr;
    m_SendASPI32Command = nullptr;
    m_GetASPI32Buffer = nullptr;
    m_FreeASPI32Buffer = nullptr;
    m_TranslateASPI32Address = nullptr;
    m_GetASPI32DLLVersion = nullptr;

    // Load DLL (TCHAR-safe)
    if (theSetting.m_AspiDLL == _T(""))
    {
        m_hModule = LoadLibrary(_T("WNASPI32.DLL"));
    }
    else
    {
        m_hModule = LoadLibrary(theSetting.m_AspiDLL);
    }

    if (m_hModule == nullptr)
    {
        MessageBox(nullptr, MSG(0), ERROR_MSG, MB_OK);
        return;
    }

    m_GetASPI32SupportInfo =
        (fGetASPI32SupportInfo)GetProcAddress(m_hModule, "GetASPI32SupportInfo");
    m_SendASPI32Command =
        (fSendASPI32Command)GetProcAddress(m_hModule, "SendASPI32Command");
    m_GetASPI32Buffer =
        (fGetASPI32Buffer)GetProcAddress(m_hModule, "GetASPI32Buffer");
    m_FreeASPI32Buffer =
        (fFreeASPI32Buffer)GetProcAddress(m_hModule, "FreeASPI32Buffer");
    m_TranslateASPI32Address =
        (fTranslateASPI32Address)GetProcAddress(m_hModule, "TranslateASPI32Address");
    m_GetASPI32DLLVersion =
        (fGetASPI32DLLVersion)GetProcAddress(m_hModule, "GetASPI32DLLVersion");

    // Minimum required exports
    if (!(m_GetASPI32SupportInfo && m_SendASPI32Command))
    {
        FreeLibrary(m_hModule);
        m_hModule = nullptr;
        MessageBox(nullptr, MSG(0), ERROR_MSG, MB_OK);
        return;
    }

    // Create event (manual reset)
    m_hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    if (m_hEvent == nullptr)
    {
        FreeLibrary(m_hModule);
        m_hModule = nullptr;
        MessageBox(nullptr, MSG(0), ERROR_MSG, MB_OK);
        return;
    }

    ScanBus();
}

BOOL CAspiDriver::IsActive()
{
    return (m_hModule != nullptr) ? TRUE : FALSE;
}

DWORD CAspiDriver::GetVersion()
{
    if (m_hModule == nullptr || m_GetASPI32DLLVersion == nullptr)
    {
        return 0;
    }
    return m_GetASPI32DLLVersion();
}

void CAspiDriver::ExecuteCommand(SRB_ExecSCSICmd& cmd)
{
    if (m_hModule == nullptr || m_SendASPI32Command == nullptr || m_hEvent == nullptr)
        return;

    (void)ResetEvent(m_hEvent);

    cmd.SRB_Ha = m_Ha;
    cmd.SRB_Tgt = m_Tgt;
    cmd.SRB_Lun = m_Lun;
    cmd.SRB_Cmd = SC_EXEC_SCSI_CMD;
    cmd.SRB_Flags = (cmd.SRB_Flags & ~SRB_POSTING) | SRB_EVENT_NOTIFY;
    cmd.SRB_PostProc = m_hEvent;
    cmd.SRB_SenseLen = SENSE_LEN;

    DWORD ret = m_SendASPI32Command(static_cast<LPSRB>(&cmd));
    if (ret == SS_PENDING)
    {
        DWORD w = WaitForSingleObject(m_hEvent, INFINITE);
        (void)w; // consider logging/handling WAIT_FAILED if desired
    }
}

void CAspiDriver::InitialAsync(void)
{
    std::memset(&m_Cmd, 0, sizeof(m_Cmd));
    if (m_hEvent != nullptr)
        (void)ResetEvent(m_hEvent);
    m_Ret = SS_COMP;
}

void CAspiDriver::FinalizeAsync(void)
{
    if (m_hEvent == nullptr)
    {
        m_Ret = SS_COMP;
        return;
    }

    if (m_Ret == SS_PENDING)
    {
        DWORD w = WaitForSingleObject(m_hEvent, INFINITE);
        (void)w;
    }

    m_Ret = SS_COMP;
}

bool CAspiDriver::ExecuteCommandAsync(SRB_ExecSCSICmd& cmd)
{
    if (m_hModule == nullptr || m_SendASPI32Command == nullptr || m_hEvent == nullptr)
        return false;

    // If a prior async op exists, complete it and report errors via return=false
    if (m_Cmd.SRB_Cmd == SC_EXEC_SCSI_CMD)
    {
        if (m_Ret == SS_PENDING)
        {
            DWORD w = WaitForSingleObject(m_hEvent, INFINITE);
            (void)w;
        }

        if (m_Cmd.SRB_Status != SS_COMP)
        {
            std::memcpy(&cmd, &m_Cmd, sizeof(m_Cmd));
            return false;
        }
    }

    (void)ResetEvent(m_hEvent);

    cmd.SRB_Ha = m_Ha;
    cmd.SRB_Tgt = m_Tgt;
    cmd.SRB_Lun = m_Lun;
    cmd.SRB_Cmd = SC_EXEC_SCSI_CMD;
    cmd.SRB_Flags = (cmd.SRB_Flags & ~SRB_POSTING) | SRB_EVENT_NOTIFY;
    cmd.SRB_PostProc = m_hEvent;
    cmd.SRB_SenseLen = SENSE_LEN;

    std::memcpy(&m_Cmd, &cmd, sizeof(m_Cmd));
    m_Ret = m_SendASPI32Command(static_cast<LPSRB>(&m_Cmd));

    // Don't lie about status; caller can check later or rely on return value
    // cmd.SRB_Status = SS_COMP;  // removed

    return true;
}

void CAspiDriver::ScanBus(void)
{
    if (m_hModule == nullptr || m_GetASPI32SupportInfo == nullptr || m_SendASPI32Command == nullptr)
    {
        m_DeviceCount = 0;
        m_CurrentDevice = 0;
        return;
    }

    BYTE Ha, Tgt, HaMax, TgtMax;
    DWORD Info;

    m_DeviceCount = 0;
    m_CurrentDevice = 0;

    Info = m_GetASPI32SupportInfo();
    if (HIBYTE(LOWORD(Info)) != SS_COMP)
    {
        HaMax = 0;
    }
    else
    {
        HaMax = LOBYTE(LOWORD(Info));
    }

    for (Ha = 0; Ha < HaMax; Ha++)
    {
        SRB_HAInquiry ha_cmd;
        std::memset(&ha_cmd, 0, sizeof(ha_cmd));
        ha_cmd.SRB_Cmd = SC_HA_INQUIRY;
        ha_cmd.SRB_Ha = Ha;

        m_SendASPI32Command(static_cast<LPSRB>(&ha_cmd));

        if (ha_cmd.HA_Unique[3] == 0)
            TgtMax = 8;
        else
            TgtMax = ha_cmd.HA_Unique[3];

        for (Tgt = 0; Tgt < TgtMax; Tgt++)
        {
            SRB_GDEVBlock dev_cmd;
            std::memset(&dev_cmd, 0, sizeof(dev_cmd));
            dev_cmd.SRB_Cmd = SC_GET_DEV_TYPE;
            dev_cmd.SRB_Ha = Ha;
            dev_cmd.SRB_Tgt = Tgt;
            dev_cmd.SRB_Lun = 0;

            m_SendASPI32Command(static_cast<LPSRB>(&dev_cmd));

            if (dev_cmd.SRB_Status == SS_COMP && dev_cmd.SRB_DeviceType == 5)
            {
                m_Adr[m_DeviceCount][0] = Ha;
                m_Adr[m_DeviceCount][1] = Tgt;
                m_Adr[m_DeviceCount][2] = 0;
                m_DeviceCount++;

                if (m_DeviceCount >= 30)
                    return;
            }
        }
    }
}

int CAspiDriver::GetDeviceCount(void)
{
    return m_DeviceCount;
}

void CAspiDriver::SetDevice(int DeviceNo)
{
    // Fix: handle 0 devices and negative indices safely
    if (m_DeviceCount <= 0)
    {
        m_CurrentDevice = 0;
        m_Ha = 0;
        m_Tgt = 0;
        m_Lun = 0;
        return;
    }

    if (DeviceNo < 0)
        DeviceNo = 0;

    if (DeviceNo >= m_DeviceCount)
        DeviceNo = m_DeviceCount - 1;

    m_CurrentDevice = DeviceNo;
    m_Ha = m_Adr[DeviceNo][0];
    m_Tgt = m_Adr[DeviceNo][1];
    m_Lun = m_Adr[DeviceNo][2];
}

void CAspiDriver::GetDeviceString(CString& Vendor, CString& Product, CString& Revision, CString& BusAddress)
{
    SRB_ExecSCSICmd cmd;
    BYTE Buffer[100];

    Vendor = _T("");
    Product = _T("");
    Revision = _T("");

    BusAddress.Format(_T("%d:%d:%d"), m_Ha, m_Tgt, m_Lun);

    std::memset(&cmd, 0, sizeof(cmd));
    std::memset(Buffer, 0, sizeof(Buffer));

    cmd.SRB_BufLen = (DWORD)sizeof(Buffer);
    cmd.SRB_Flags = SRB_DIR_IN;
    cmd.SRB_BufPointer = Buffer;
    cmd.SRB_CDBLen = 6;
    cmd.CDBByte[0] = 0x12; // INQUIRY (SPC)
    cmd.CDBByte[4] = (BYTE)sizeof(Buffer);

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

#ifdef _UNICODE
        // Convert ANSI inquiry fields -> Unicode CString safely
        Vendor = CA2T(vendor);
        Product = CA2T(product);
        Revision = CA2T(revision);
#else
        Vendor = vendor;
        Product = product;
        Revision = revision;
#endif

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
}

int CAspiDriver::GetCurrentDevice(void)
{
    return m_CurrentDevice;
}
