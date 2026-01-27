#include "StdAfx.h"
#include "Aspi.h"
#include "PBBuffer.h"

#include <cstring>

static DWORD MinDWORD(DWORD a, DWORD b) { return (a < b) ? a : b; }

CAspi::CAspi() {}
CAspi::~CAspi() {}

void CAspi::Initialize() {}
BOOL CAspi::IsActive() { return FALSE; }
DWORD CAspi::GetVersion() { return 0; }
void CAspi::ExecuteCommand(SRB_ExecSCSICmd& /*cmd*/) {}

void CAspi::InitialAsync() {}
void CAspi::FinalizeAsync() {}
bool CAspi::ExecuteCommandAsync(SRB_ExecSCSICmd& /*cmd*/) { return false; }

// Returns offset (ModeDataPoint) on success, 0 on failure.
// Copies up to BufLen bytes into Buffer; zero-fills remainder.
DWORD CAspi::ModeSense(BYTE* Buffer, DWORD BufLen, BYTE PCFlag, BYTE PageCode)
{
    if (!Buffer || BufLen == 0)
        return 0;

    const DWORD kLocalCap = 256;

    CPBBuffer temp;
    BYTE* local = temp.CreateBuffer(kLocalCap);
    if (!local)
    {
        std::memset(Buffer, 0, BufLen);
        return 0;
    }

    SRB_ExecSCSICmd cmd;
    std::memset(&cmd, 0, sizeof(cmd));
    std::memset(local, 0, kLocalCap);

    cmd.SRB_Flags = SRB_DIR_IN;
    cmd.SRB_BufLen = kLocalCap;
    cmd.SRB_BufPointer = local;
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

    DWORD dataLen = (((DWORD)local[0] << 8) | (DWORD)local[1]) + 2;
    if (dataLen > kLocalCap) dataLen = kLocalCap;

    if (dataLen < 8)
    {
        std::memset(Buffer, 0, BufLen);
        return 0;
    }

    DWORD blockDescLen = ((DWORD)local[6] << 8) | (DWORD)local[7];
    DWORD modeDataPoint = blockDescLen + 8;

    if (modeDataPoint >= dataLen)
    {
        std::memset(Buffer, 0, BufLen);
        return 0;
    }

    const DWORD toCopy = MinDWORD(dataLen, BufLen);
    std::memcpy(Buffer, local, toCopy);
    if (toCopy < BufLen)
        std::memset(Buffer + toCopy, 0, BufLen - toCopy);

    return modeDataPoint;
}

bool CAspi::ModeSelect(BYTE* Buffer, DWORD BufLen)
{
    if (!Buffer || BufLen == 0)
        return false;

    CPBBuffer temp;
    BYTE* out = temp.CreateBuffer(BufLen);
    if (!out)
        return false;

    std::memcpy(out, Buffer, BufLen);

    // If the header bytes must be cleared, do it on outgoing data (not caller's buffer)
    if (BufLen > 5)
    {
        out[0] = 0; out[1] = 0;
        out[4] = 0; out[5] = 0;
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

int CAspi::GetDeviceCount() { return 0; }
void CAspi::SetDevice(int /*DeviceNo*/) {}
void CAspi::GetDeviceString(CString& /*Vendor*/, CString& /*Product*/, CString& /*Revision*/, CString& /*BusAddress*/) {}
int CAspi::GetCurrentDevice() { return 0; }
