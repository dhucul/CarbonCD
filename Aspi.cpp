// CAspi.cpp - single file, C++03/MSVC friendly

#include "StdAfx.h"
#include "Aspi.h"
#include "Setting.h"
#include "PBBuffer.h"

#include <cstring> // memset, memcpy

// Local helper to avoid std::min / min macro issues
static DWORD MinDWORD(DWORD a, DWORD b) { return (a < b) ? a : b; }

CAspi::CAspi()
{
}

CAspi::~CAspi()
{
}

void CAspi::Initialize()
{
    // TODO
}

BOOL CAspi::IsActive()
{
    // TODO
    return FALSE;
}

DWORD CAspi::GetVersion()
{
    // TODO
    return 0;
}

void CAspi::ExecuteCommand(SRB_ExecSCSICmd& /*cmd*/)
{
    // TODO: issue SRB to ASPI layer and fill cmd.SRB_Status, etc.
}

void CAspi::InitialAsync(void)
{
    // TODO
}

void CAspi::FinalizeAsync(void)
{
    // TODO
}

bool CAspi::ExecuteCommandAsync(SRB_ExecSCSICmd& /*cmd*/)
{
    // TODO
    return false;
}

// Returns offset (ModeDataPoint) on success, 0 on failure.
// Copies up to BufLen bytes into Buffer; zero-fills remainder on success/failure.
DWORD CAspi::ModeSense(BYTE* Buffer, DWORD BufLen, BYTE PCFlag, BYTE PageCode)
{
    if (Buffer == 0 || BufLen == 0)
        return 0;

    const DWORD kLocalCap = 256;

    CPBBuffer PBuffer;
    BYTE* LocalBuffer = PBuffer.CreateBuffer(kLocalCap);
    if (LocalBuffer == 0)
    {
        std::memset(Buffer, 0, BufLen);
        return 0;
    }

    SRB_ExecSCSICmd cmd;
    std::memset(&cmd, 0, sizeof(cmd));
    std::memset(LocalBuffer, 0, kLocalCap);

    cmd.SRB_Flags = SRB_DIR_IN;
    cmd.SRB_BufLen = kLocalCap;
    cmd.SRB_BufPointer = LocalBuffer;
    cmd.SRB_CDBLen = 0x0A;

    // MODE SENSE(10)
    cmd.CDBByte[0] = 0x5A;
    cmd.CDBByte[2] = (BYTE)((PageCode & 0x3F) | ((PCFlag & 0x03) << 6));
    cmd.CDBByte[7] = (BYTE)((kLocalCap >> 8) & 0xFF);
    cmd.CDBByte[8] = (BYTE)(kLocalCap & 0xFF);

    ExecuteCommand(cmd);

    if (cmd.SRB_Status != SS_COMP)
    {
        std::memset(Buffer, 0, BufLen);
        return 0;
    }

    // Need at least Mode Parameter Header(10) = 8 bytes
    // Your original code: DataLen = ((buf[0]<<8)|buf[1]) + 2
    DWORD dataLen = (((DWORD)LocalBuffer[0] << 8) | (DWORD)LocalBuffer[1]) + 2;
    if (dataLen > kLocalCap) dataLen = kLocalCap;

    if (dataLen < 8)
    {
        std::memset(Buffer, 0, BufLen);
        return 0;
    }

    DWORD blockDescLen = ((DWORD)LocalBuffer[6] << 8) | (DWORD)LocalBuffer[7];
    DWORD modeDataPoint = blockDescLen + 8;

    if (modeDataPoint >= dataLen)
    {
        std::memset(Buffer, 0, BufLen);
        return 0;
    }

    // Copy safely into caller buffer
    DWORD toCopy = MinDWORD(dataLen, BufLen);
    std::memcpy(Buffer, LocalBuffer, toCopy);
    if (toCopy < BufLen)
        std::memset(Buffer + toCopy, 0, BufLen - toCopy);

    return modeDataPoint;
}

bool CAspi::ModeSelect(BYTE* Buffer, DWORD BufLen)
{
    if (Buffer == 0 || BufLen == 0)
        return false;

    CPBBuffer PBuffer;
    BYTE* out = PBuffer.CreateBuffer(BufLen);
    if (out == 0)
        return false;

    // Copy caller's buffer into our outgoing buffer
    std::memcpy(out, Buffer, BufLen);

    // If you need these header bytes cleared, do it on outgoing data (NOT the caller's Buffer)
    if (BufLen > 5)
    {
        out[0] = 0;
        out[1] = 0;
        out[4] = 0;
        out[5] = 0;
    }

    SRB_ExecSCSICmd cmd;
    std::memset(&cmd, 0, sizeof(cmd));

    cmd.SRB_Flags = SRB_DIR_OUT;
    cmd.SRB_BufLen = BufLen;
    cmd.SRB_BufPointer = out;
    cmd.SRB_CDBLen = 0x0A;

    // MODE SELECT(10)
    cmd.CDBByte[0] = 0x55;
    cmd.CDBByte[1] = (BYTE)(1 << 4); // PF bit
    cmd.CDBByte[7] = (BYTE)((BufLen >> 8) & 0xFF);
    cmd.CDBByte[8] = (BYTE)(BufLen & 0xFF);

    ExecuteCommand(cmd);

    return (cmd.SRB_Status == SS_COMP);
}

int CAspi::GetDeviceCount(void)
{
    // TODO
    return 0;
}

void CAspi::SetDevice(int /*DeviceNo*/)
{
    // TODO
}

void CAspi::GetDeviceString(CString& /*Vendor*/, CString& /*Product*/, CString& /*Revision*/, CString& /*BusAddress*/)
{
    // TODO
}

int CAspi::GetCurrentDevice(void)
{
    // TODO
    return 0;
}
