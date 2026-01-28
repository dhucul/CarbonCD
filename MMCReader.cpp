#include "StdAfx.h"
#include "MMCReader.h"

#include <cstring>

// ----------------------------------------------
// Local helpers
// ----------------------------------------------
static bool IsIllegalRequestInvalidField(const BYTE* sense)
{
    if (!sense) return false;
    // fixed format expected in your codebase
    const BYTE rc = sense[0] & 0x7F;
    if (rc != 0x70 && rc != 0x71) return false;
    const BYTE key = sense[2] & 0x0F;
    const BYTE asc = sense[12];
    const BYTE ascq = sense[13];
    return (key == 0x05 && asc == 0x24 && ascq == 0x00);
}

CMMCReader::CMMCReader(void)
    : m_SRB_Status(0)
{
    m_Aspi = nullptr;
    m_ReadSubQMethod = 0;
}

CMMCReader::~CMMCReader(void)
{
}

void CMMCReader::Initialize(CAspi* Aspi)
{
    m_Aspi = Aspi;
}

// ------------------------------------------------------------
// READ TOC/PMA/ATIP (0x43)
// This function originally assumed "Full TOC" (format 0x02)
// which some drives reject (05/24/00). We fall back to format 0x00.
// ------------------------------------------------------------
bool CMMCReader::ReadTOCFromSession(TableOfContents& Toc)
{
    if (m_Aspi == nullptr) return false;

    std::memset(Toc.m_RawTOC, 0, sizeof(Toc.m_RawTOC));

    auto exec_read_toc = [&](BYTE format) -> bool
        {
            SRB_ExecSCSICmd cmd;
            std::memset(&cmd, 0, sizeof(cmd));
            cmd.SRB_Flags = SRB_DIR_IN;
            cmd.SRB_BufLen = sizeof(Toc.m_RawTOC);
            cmd.SRB_BufPointer = static_cast<BYTE*>(Toc.m_RawTOC);
            cmd.SRB_CDBLen = 10;

            cmd.CDBByte[0] = 0x43;          // READ TOC/PMA/ATIP
            cmd.CDBByte[1] = 0x02;          // MSF = 1 (your original behavior)
            cmd.CDBByte[2] = format;        // 0x02 = Full TOC, 0x00 = Formatted TOC
            cmd.CDBByte[6] = 0x00;          // Track/session number (0 for first / drive default)

            cmd.CDBByte[7] = static_cast<BYTE>((sizeof(Toc.m_RawTOC) >> 8) & 0xFF);
            cmd.CDBByte[8] = static_cast<BYTE>((sizeof(Toc.m_RawTOC)) & 0xFF);

            m_Aspi->ExecuteCommand(cmd);
            if (cmd.SRB_Status == SS_COMP) return true;

            m_SRB_Status = cmd.SRB_Status;
            // stash last sense decode for callers who look at it
            m_SK = cmd.SenseArea[2] & 0x0F;
            m_ASC = cmd.SenseArea[12];
            m_ASCQ = cmd.SenseArea[13];

            // If it's the classic "invalid field in CDB", caller may retry different format
            return false;
        };

    // 1) Try Full TOC (format 0x02) first (matches your original parsing expectations)
    if (!exec_read_toc(0x02))
    {
        // If drive rejects Full TOC with 05/24/00, retry with formatted TOC (0x00).
        // NOTE: We don't have direct access to cmd.SenseArea here, but m_SK/m_ASC/m_ASCQ were set.
        if (m_SK == 0x05 && m_ASC == 0x24 && m_ASCQ == 0x00)
        {
            std::memset(Toc.m_RawTOC, 0, sizeof(Toc.m_RawTOC));
            if (!exec_read_toc(0x00))
                return false;
        }
        else
        {
            return false;
        }
    }

    // ------------------------------------------------------------
    // Parse returned TOC.
    // Your original code assumes 11-byte descriptors (Full TOC).
    // For formatted TOC (0x00), descriptors are 8 bytes each.
    // We detect by payload length and do the appropriate parse.
    // ------------------------------------------------------------
    const int payloadLen = (Toc.m_RawTOC[0] * 0x100 + Toc.m_RawTOC[1]) - 2;
    if (payloadLen <= 0) return false;

    BYTE* p = Toc.m_RawTOC + 4;

    // Heuristic: if payloadLen is divisible by 11, treat as Full TOC.
    const bool looksLikeFullToc = (payloadLen % 11) == 0;

    int MaxTrack = 0;
    MSFAddress EndOfDisk;
    EndOfDisk.Minute = EndOfDisk.Second = EndOfDisk.Frame = 0;

    // reset
    Toc.m_LastTrack = 0;

    if (looksLikeFullToc)
    {
        const int infoNum = payloadLen / 11;
        BYTE PrevType = 0xFF;
        int SetEndFlag = 0;

        for (int i = 0; i < infoNum; i++)
        {
            BYTE* TrackData = p + 11 * i;

            // Your original checks:
            // TrackData[1] control/adr, TrackData[3] point
            if ((TrackData[1] & 0xF0) == 0x10 && (TrackData[3] < 100))
            {
                const BYTE track = TrackData[3] - 1;

                if (MaxTrack < track) MaxTrack = track;

                Toc.m_Track[track].m_TrackNo = track + 1;
                Toc.m_Track[track].m_MSF.Minute = TrackData[8];
                Toc.m_Track[track].m_MSF.Second = TrackData[9];
                Toc.m_Track[track].m_MSF.Frame = TrackData[10];
                Toc.m_Track[track].m_Session = TrackData[0];

                if (SetEndFlag == 1)
                    Toc.m_Track[track - 1].m_EndMSF = Toc.m_Track[track].m_MSF;

                SetEndFlag = 1;

                if ((TrackData[1] & 4) == 4)
                {
                    Toc.m_Track[track].m_TrackType = TRACKTYPE_DATA;
                    Toc.m_Track[track].m_DigitalCopy = TRACKFLAG_UNKNOWN;
                    Toc.m_Track[track].m_Emphasis = TRACKFLAG_UNKNOWN;
                    PrevType = TRACKTYPE_DATA;
                }
                else
                {
                    Toc.m_Track[track].m_TrackType = TRACKTYPE_AUDIO;
                    Toc.m_Track[track].m_DigitalCopy = ((TrackData[1] & 2) ? TRACKFLAG_YES : TRACKFLAG_NO);
                    Toc.m_Track[track].m_Emphasis = ((TrackData[1] & 1) ? TRACKFLAG_YES : TRACKFLAG_NO);

                    if (track > 0 && PrevType == TRACKTYPE_DATA)
                        Toc.m_Track[track - 1].m_EndMSF = Toc.m_Track[track].m_MSF.GetByLBA() - 150;

                    PrevType = TRACKTYPE_AUDIO;
                }
            }
            else if ((TrackData[1] & 0xF0) == 0x10 && TrackData[3] == 0xA2)
            {
                EndOfDisk.Minute = TrackData[8];
                EndOfDisk.Second = TrackData[9];
                EndOfDisk.Frame = TrackData[10];
            }
        }

        Toc.m_Track[MaxTrack].m_EndMSF = EndOfDisk;
        Toc.m_LastTrack = MaxTrack + 1;
        Toc.m_Track[MaxTrack + 1].m_TrackNo = MaxTrack + 1;
        Toc.m_Track[MaxTrack + 1].m_MSF = EndOfDisk;
        Toc.m_Track[MaxTrack + 1].m_EndMSF = EndOfDisk;
        return true;
    }
    else
    {
        // Formatted TOC (8-byte descriptors):
        // Each descriptor: [0]=Reserved, [1]=ADR/Control, [2]=Track No, [3]=Reserved,
        // [4]=Min, [5]=Sec, [6]=Frame, [7]=Reserved (MSF when MSF=1)
        const int infoNum = payloadLen / 8;
        if (infoNum <= 0) return false;

        int firstTrack = Toc.m_RawTOC[2];
        int lastTrack = Toc.m_RawTOC[3];

        // Initialize basic info
        for (int t = 0; t < 100; ++t)
        {
            Toc.m_Track[t].m_TrackNo = 0;
        }

        for (int i = 0; i < infoNum; ++i)
        {
            BYTE* d = p + 8 * i;
            BYTE trackNo = d[2];

            if (trackNo >= 1 && trackNo <= 99)
            {
                int idx = trackNo - 1;
                Toc.m_Track[idx].m_TrackNo = trackNo;
                Toc.m_Track[idx].m_MSF.Minute = d[4];
                Toc.m_Track[idx].m_MSF.Second = d[5];
                Toc.m_Track[idx].m_MSF.Frame = d[6];

                // Track type hint from control bits
                const BYTE control = (d[1] >> 4) & 0x0F;
                Toc.m_Track[idx].m_TrackType = (control & 0x04) ? TRACKTYPE_DATA : TRACKTYPE_AUDIO;
            }
            else if (trackNo == 0xAA)
            {
                EndOfDisk.Minute = d[4];
                EndOfDisk.Second = d[5];
                EndOfDisk.Frame = d[6];
            }
        }

        Toc.m_LastTrack = lastTrack; // best effort
        if (lastTrack > 0)
        {
            int idx = lastTrack - 1;
            Toc.m_Track[idx].m_EndMSF = EndOfDisk;
            Toc.m_Track[lastTrack].m_TrackNo = lastTrack + 1;
            Toc.m_Track[lastTrack].m_MSF = EndOfDisk;
            Toc.m_Track[lastTrack].m_EndMSF = EndOfDisk;
        }
        return true;
    }
}

bool CMMCReader::ReadCD(MSFAddress Address, LPSTR Buffer)
{
    if (m_Aspi == nullptr) return false;

    SRB_ExecSCSICmd cmd;
    std::memset(&cmd, 0, sizeof(cmd));
    cmd.SRB_BufLen = 2352;
    cmd.SRB_BufPointer = (BYTE*)Buffer;
    cmd.SRB_CDBLen = 12;
    cmd.SRB_Flags = SRB_DIR_IN;

    cmd.CDBByte[0] = 0xB9; // READ CD MSF
    cmd.CDBByte[1] = 0x00;
    cmd.CDBByte[3] = Address.Minute;
    cmd.CDBByte[4] = Address.Second;
    cmd.CDBByte[5] = Address.Frame;

    Address = Address.GetByLBA() + 1;
    cmd.CDBByte[6] = Address.Minute;
    cmd.CDBByte[7] = Address.Second;
    cmd.CDBByte[8] = Address.Frame;

    cmd.CDBByte[9] = 0x10; // UserData
    m_Aspi->ExecuteCommand(cmd);

    if (cmd.SRB_Status == SS_COMP) return true;

    m_SRB_Status = cmd.SRB_Status;
    m_SK = cmd.SenseArea[2] & 0x0f;
    m_ASC = cmd.SenseArea[12];
    m_ASCQ = cmd.SenseArea[13];
    return false;
}

bool CMMCReader::ReadCDDA(MSFAddress Address, LPSTR Buffer)
{
    if (m_Aspi == nullptr) return false;

    SRB_ExecSCSICmd cmd;
    std::memset(&cmd, 0, sizeof(cmd));
    cmd.SRB_BufLen = 2352;
    cmd.SRB_BufPointer = (BYTE*)Buffer;
    cmd.SRB_CDBLen = 12;
    cmd.SRB_Flags = SRB_DIR_IN;

    cmd.CDBByte[0] = 0xB9; // READ CD MSF
    cmd.CDBByte[1] = 0x04; // CDDA sector
    cmd.CDBByte[3] = Address.Minute;
    cmd.CDBByte[4] = Address.Second;
    cmd.CDBByte[5] = Address.Frame;

    Address = Address.GetByLBA() + 1;
    cmd.CDBByte[6] = Address.Minute;
    cmd.CDBByte[7] = Address.Second;
    cmd.CDBByte[8] = Address.Frame;

    cmd.CDBByte[9] = 0x10; // UserData (legacy behavior)
    m_Aspi->ExecuteCommand(cmd);

    if (cmd.SRB_Status == SS_COMP) return true;

    m_SRB_Status = cmd.SRB_Status;
    m_SK = cmd.SenseArea[2] & 0x0f;
    m_ASC = cmd.SenseArea[12];
    m_ASCQ = cmd.SenseArea[13];
    return false;
}

bool CMMCReader::ReadCD_LBA(MSFAddress Address, LPSTR Buffer)
{
    if (m_Aspi == nullptr) return false;

    DWORD lba = Address.Frame + 75 * (Address.Second + 60 * (Address.Minute)) - 150;

    SRB_ExecSCSICmd cmd;
    std::memset(&cmd, 0, sizeof(cmd));
    cmd.SRB_BufLen = 2352;
    cmd.SRB_BufPointer = (BYTE*)Buffer;
    cmd.SRB_CDBLen = 12;
    cmd.SRB_Flags = SRB_DIR_IN;

    cmd.CDBByte[0] = 0xBE; // READ CD LBA
    cmd.CDBByte[2] = (BYTE)(lba >> 24);
    cmd.CDBByte[3] = (BYTE)(lba >> 16);
    cmd.CDBByte[4] = (BYTE)(lba >> 8);
    cmd.CDBByte[5] = (BYTE)(lba);
    cmd.CDBByte[8] = 1;
    cmd.CDBByte[9] = 0x10; // UserData

    m_Aspi->ExecuteCommand(cmd);

    if (cmd.SRB_Status == SS_COMP) return true;

    m_SRB_Status = cmd.SRB_Status;
    m_SK = cmd.SenseArea[2] & 0x0f;
    m_ASC = cmd.SenseArea[12];
    m_ASCQ = cmd.SenseArea[13];
    return false;
}

bool CMMCReader::ReadCDDA_LBA(MSFAddress Address, LPSTR Buffer)
{
    if (m_Aspi == nullptr) return false;

    DWORD lba = Address.Frame + 75 * (Address.Second + 60 * (Address.Minute)) - 150;

    SRB_ExecSCSICmd cmd;
    std::memset(&cmd, 0, sizeof(cmd));
    cmd.SRB_BufLen = 2352;
    cmd.SRB_BufPointer = (BYTE*)Buffer;
    cmd.SRB_CDBLen = 12;
    cmd.SRB_Flags = SRB_DIR_IN;

    cmd.CDBByte[0] = 0xBE; // READ CD LBA
    cmd.CDBByte[1] = 0x04; // CDDA sector
    cmd.CDBByte[2] = (BYTE)(lba >> 24);
    cmd.CDBByte[3] = (BYTE)(lba >> 16);
    cmd.CDBByte[4] = (BYTE)(lba >> 8);
    cmd.CDBByte[5] = (BYTE)(lba);
    cmd.CDBByte[8] = 1;
    cmd.CDBByte[9] = 0x10; // UserData (legacy behavior)

    m_Aspi->ExecuteCommand(cmd);

    if (cmd.SRB_Status == SS_COMP) return true;

    m_SRB_Status = cmd.SRB_Status;
    m_SK = cmd.SenseArea[2] & 0x0f;
    m_ASC = cmd.SenseArea[12];
    m_ASCQ = cmd.SenseArea[13];
    return false;
}

bool CMMCReader::ReadCDRaw(MSFAddress Address, LPSTR Buffer)
{
    if (m_Aspi == nullptr) return false;

    SRB_ExecSCSICmd cmd;
    std::memset(&cmd, 0, sizeof(cmd));
    cmd.SRB_BufLen = 2352;
    cmd.SRB_BufPointer = (BYTE*)Buffer;
    cmd.SRB_CDBLen = 12;
    cmd.SRB_Flags = SRB_DIR_IN;

    cmd.CDBByte[0] = 0xB9; // READ CD MSF
    cmd.CDBByte[3] = Address.Minute;
    cmd.CDBByte[4] = Address.Second;
    cmd.CDBByte[5] = Address.Frame;

    Address = Address.GetByLBA() + 1;
    cmd.CDBByte[6] = Address.Minute;
    cmd.CDBByte[7] = Address.Second;
    cmd.CDBByte[8] = Address.Frame;

    cmd.CDBByte[9] = 0xF8; // SYNC/AllHeaders/UserData/EDC&ECC
    m_Aspi->ExecuteCommand(cmd);

    return (cmd.SRB_Status == SS_COMP);
}

bool CMMCReader::ReadCDRaw_LBA(MSFAddress Address, LPSTR Buffer)
{
    if (m_Aspi == nullptr) return false;

    DWORD lba = Address.Frame + 75 * (Address.Second + 60 * (Address.Minute)) - 150;

    SRB_ExecSCSICmd cmd;
    std::memset(&cmd, 0, sizeof(cmd));
    cmd.SRB_BufLen = 2352;
    cmd.SRB_BufPointer = (BYTE*)Buffer;
    cmd.SRB_CDBLen = 12;
    cmd.SRB_Flags = SRB_DIR_IN;

    cmd.CDBByte[0] = 0xBE; // READ CD LBA
    cmd.CDBByte[2] = (BYTE)(lba >> 24);
    cmd.CDBByte[3] = (BYTE)(lba >> 16);
    cmd.CDBByte[4] = (BYTE)(lba >> 8);
    cmd.CDBByte[5] = (BYTE)(lba);
    cmd.CDBByte[8] = 1;
    cmd.CDBByte[9] = 0xF8; // SYNC/AllHeaders/UserData/EDC&ECC

    m_Aspi->ExecuteCommand(cmd);

    if (cmd.SRB_Status == SS_COMP) return true;

    m_SRB_Status = cmd.SRB_Status;
    m_SK = cmd.SenseArea[2] & 0x0f;
    m_ASC = cmd.SenseArea[12];
    m_ASCQ = cmd.SenseArea[13];
    return false;
}

bool CMMCReader::ReadCD_Read10(MSFAddress Address, LPSTR Buffer)
{
    if (m_Aspi == nullptr) return false;

    DWORD lba = Address.Frame + 75 * (Address.Second + 60 * (Address.Minute)) - 150;

    SRB_ExecSCSICmd cmd;
    std::memset(&cmd, 0, sizeof(cmd));
    cmd.SRB_BufLen = 2352;
    cmd.SRB_BufPointer = (BYTE*)Buffer;
    cmd.SRB_CDBLen = 12;
    cmd.SRB_Flags = SRB_DIR_IN;

    cmd.CDBByte[0] = 0x28; // READ(10)
    cmd.CDBByte[2] = (BYTE)(lba >> 24);
    cmd.CDBByte[3] = (BYTE)(lba >> 16);
    cmd.CDBByte[4] = (BYTE)(lba >> 8);
    cmd.CDBByte[5] = (BYTE)(lba);
    cmd.CDBByte[8] = 1;

    m_Aspi->ExecuteCommand(cmd);

    if (cmd.SRB_Status == SS_COMP) return true;

    m_SRB_Status = cmd.SRB_Status;
    m_SK = cmd.SenseArea[2] & 0x0f;
    m_ASC = cmd.SenseArea[12];
    m_ASCQ = cmd.SenseArea[13];
    return false;
}

bool CMMCReader::ReadCD_D8(MSFAddress Address, LPSTR Buffer)
{
    if (m_Aspi == nullptr) return false;

    DWORD lba = Address.Frame + 75 * (Address.Second + 60 * (Address.Minute)) - 150;

    SRB_ExecSCSICmd cmd;
    std::memset(&cmd, 0, sizeof(cmd));
    cmd.SRB_BufLen = 2352;
    cmd.SRB_BufPointer = (BYTE*)Buffer;
    cmd.SRB_CDBLen = 12;
    cmd.SRB_Flags = SRB_DIR_IN;

    cmd.CDBByte[0] = 0xD8; // vendor READ
    cmd.CDBByte[3] = (BYTE)(lba >> 16);
    cmd.CDBByte[4] = (BYTE)(lba >> 8);
    cmd.CDBByte[5] = (BYTE)(lba);
    cmd.CDBByte[9] = 1;

    m_Aspi->ExecuteCommand(cmd);

    if (cmd.SRB_Status == SS_COMP) return true;

    m_SRB_Status = cmd.SRB_Status;
    m_SK = cmd.SenseArea[2] & 0x0f;
    m_ASC = cmd.SenseArea[12];
    m_ASCQ = cmd.SenseArea[13];
    return false;
}

bool CMMCReader::ReadCD_D4(MSFAddress Address, LPSTR Buffer)
{
    if (m_Aspi == nullptr) return false;

    DWORD lba = Address.Frame + 75 * (Address.Second + 60 * (Address.Minute)) - 150;

    SRB_ExecSCSICmd cmd;
    std::memset(&cmd, 0, sizeof(cmd));
    cmd.SRB_BufLen = 2352;
    cmd.SRB_BufPointer = (BYTE*)Buffer;
    cmd.SRB_CDBLen = 10;
    cmd.SRB_Flags = SRB_DIR_IN;

    cmd.CDBByte[0] = 0xD4;
    cmd.CDBByte[3] = (BYTE)(lba >> 16);
    cmd.CDBByte[4] = (BYTE)(lba >> 8);
    cmd.CDBByte[5] = (BYTE)(lba);
    cmd.CDBByte[8] = 1;

    m_Aspi->ExecuteCommand(cmd);

    if (cmd.SRB_Status == SS_COMP) return true;

    m_SRB_Status = cmd.SRB_Status;
    m_SK = cmd.SenseArea[2] & 0x0f;
    m_ASC = cmd.SenseArea[12];
    m_ASCQ = cmd.SenseArea[13];
    return false;
}

bool CMMCReader::ReadCD_D4_2(MSFAddress Address, LPSTR Buffer)
{
    if (m_Aspi == nullptr) return false;

    DWORD lba = Address.Frame + 75 * (Address.Second + 60 * (Address.Minute)) - 150;

    SRB_ExecSCSICmd cmd;
    std::memset(&cmd, 0, sizeof(cmd));
    cmd.SRB_BufLen = 2352;
    cmd.SRB_BufPointer = (BYTE*)Buffer;
    cmd.SRB_CDBLen = 10;
    cmd.SRB_Flags = SRB_DIR_IN;

    cmd.CDBByte[0] = 0xD4;
    cmd.CDBByte[3] = (BYTE)(lba >> 16);
    cmd.CDBByte[4] = (BYTE)(lba >> 8);
    cmd.CDBByte[5] = (BYTE)(lba);
    cmd.CDBByte[9] = 1;

    m_Aspi->ExecuteCommand(cmd);

    if (cmd.SRB_Status == SS_COMP) return true;

    m_SRB_Status = cmd.SRB_Status;
    m_SK = cmd.SenseArea[2] & 0x0f;
    m_ASC = cmd.SenseArea[12];
    m_ASCQ = cmd.SenseArea[13];
    return false;
}

bool CMMCReader::ReadCD_D5(MSFAddress Address, LPSTR Buffer)
{
    if (m_Aspi == nullptr) return false;

    DWORD lba = Address.Frame + 75 * (Address.Second + 60 * (Address.Minute)) - 150;

    SRB_ExecSCSICmd cmd;
    std::memset(&cmd, 0, sizeof(cmd));
    cmd.SRB_BufLen = 2352;
    cmd.SRB_BufPointer = (BYTE*)Buffer;
    cmd.SRB_CDBLen = 10;
    cmd.SRB_Flags = SRB_DIR_IN;

    cmd.CDBByte[0] = 0xD5;
    cmd.CDBByte[3] = (BYTE)(lba >> 16);
    cmd.CDBByte[4] = (BYTE)(lba >> 8);
    cmd.CDBByte[5] = (BYTE)(lba);
    cmd.CDBByte[9] = 1;

    m_Aspi->ExecuteCommand(cmd);

    if (cmd.SRB_Status == SS_COMP) return true;

    m_SRB_Status = cmd.SRB_Status;
    m_SK = cmd.SenseArea[2] & 0x0f;
    m_ASC = cmd.SenseArea[12];
    m_ASCQ = cmd.SenseArea[13];
    return false;
}

void CMMCReader::SetCDSpeed(BYTE ReadSpeed, BYTE WriteSpeed)
{
    if (m_Aspi == nullptr) return;

    DWORD RSpeed = (ReadSpeed * 2352 * 75) / 1000;
    if (ReadSpeed == 0xFF) RSpeed = 0xFFFF;

    DWORD WSpeed = (WriteSpeed * 2352 * 75) / 1000;
    if (WriteSpeed == 0xFF) WSpeed = 0xFFFF;

    SRB_ExecSCSICmd cmd;
    std::memset(&cmd, 0, sizeof(cmd));
    cmd.SRB_BufLen = 0;
    cmd.SRB_BufPointer = nullptr;
    cmd.SRB_CDBLen = 12;
    cmd.SRB_Flags = SRB_DIR_IN;

    cmd.CDBByte[0] = 0xBB; // SET CD SPEED
    cmd.CDBByte[2] = (BYTE)(RSpeed >> 8);
    cmd.CDBByte[3] = (BYTE)(RSpeed);
    cmd.CDBByte[4] = (BYTE)(WSpeed >> 8);
    cmd.CDBByte[5] = (BYTE)(WSpeed);

    m_Aspi->ExecuteCommand(cmd);
}

bool CMMCReader::SetErrorCorrectMode(bool CorrectFlag)
{
    if (m_Aspi == nullptr) return false;

    if (CorrectFlag)
    {
        BYTE Buffer[256];
        DWORD DataPoint = m_Aspi->ModeSense(Buffer, 256, 0x02, 0x01);
        DWORD BufLen = ((Buffer[0] << 8) + Buffer[1]) + 2;

        if (DataPoint)
        {
            bool b = false;
            if (DataPoint == 8)
            {
                BYTE WriteData[256];
                BYTE PageLen = Buffer[DataPoint + 1] + 2;
                std::memset(WriteData, 0, 16);
                std::memcpy(WriteData + 16, Buffer + DataPoint, PageLen);
                WriteData[0x07] = 0x08;
                WriteData[0x0E] = 0x08;
                WriteData[0x0F] = 0x00;
                WriteData[0x12] = 0x00;
                b = m_Aspi->ModeSelect(WriteData, PageLen + 16);
            }
            else
            {
                Buffer[0x0E] = 0x08;
                Buffer[0x0F] = 0x00;
                Buffer[DataPoint + 2] = 0x00;
                b = m_Aspi->ModeSelect(Buffer, BufLen);
            }

            if (!b)
            {
                Buffer[DataPoint + 2] = 0x00;
                b = m_Aspi->ModeSelect(Buffer, BufLen);
            }
            return b;
        }
    }
    else
    {
        BYTE Buffer[256];
        DWORD DataPoint = m_Aspi->ModeSense(Buffer, 256, 0x00, 0x01);
        DWORD BufLen = ((Buffer[0] << 8) + Buffer[1]) + 2;

        if (DataPoint)
        {
            bool b = false;
            if (DataPoint == 8)
            {
                BYTE WriteData[256];
                BYTE PageLen = Buffer[DataPoint + 1] + 2;
                std::memset(WriteData, 0, 16);
                std::memcpy(WriteData + 16, Buffer + DataPoint, PageLen);
                WriteData[0x07] = 0x08;
                WriteData[0x0E] = 0x09;
                WriteData[0x0F] = 0x30;
                WriteData[0x12] = 0x01;
                WriteData[0x13] = 0x00;
                b = m_Aspi->ModeSelect(WriteData, PageLen + 16);
            }
            else
            {
                Buffer[0x0E] = 0x09;
                Buffer[0x0F] = 0x30;
                Buffer[DataPoint + 2] = 0x01;
                Buffer[DataPoint + 3] = 0x00;
                b = m_Aspi->ModeSelect(Buffer, BufLen);
            }

            if (!b)
            {
                Buffer[DataPoint + 2] = 0x01;
                Buffer[DataPoint + 3] = 0x00;
                b = m_Aspi->ModeSelect(Buffer, BufLen);
            }

            if (!b)
            {
                if (DataPoint == 8)
                {
                    BYTE WriteData[256];
                    BYTE PageLen = Buffer[DataPoint + 1] + 2;
                    std::memset(WriteData, 0, 16);
                    std::memcpy(WriteData + 16, Buffer + DataPoint, PageLen);
                    WriteData[0x07] = 0x08;
                    WriteData[0x0E] = 0x09;
                    WriteData[0x0F] = 0x30;
                    WriteData[0x12] = 0x00;
                    WriteData[0x13] = 0x00;
                    b = m_Aspi->ModeSelect(WriteData, PageLen + 16);
                }
                else
                {
                    Buffer[0x0E] = 0x09;
                    Buffer[0x0F] = 0x30;
                    Buffer[DataPoint + 2] = 0x00;
                    Buffer[DataPoint + 3] = 0x00;
                    b = m_Aspi->ModeSelect(Buffer, BufLen);
                }
            }

            if (!b)
            {
                Buffer[DataPoint + 2] = 0x00;
                Buffer[DataPoint + 3] = 0x00;
                b = m_Aspi->ModeSelect(Buffer, BufLen);
            }
            return b;
        }
    }

    return false;
}

bool CMMCReader::ReadSubQ(MSFAddress msf, BYTE* Buffer)
{
    if (m_Aspi == nullptr) return false;

    BYTE ReadBuffer[2352 + 96];
    std::memset(ReadBuffer, 0, sizeof(ReadBuffer));

    DWORD lba = msf.Frame + 75 * (msf.Second + 60 * (msf.Minute)) - 150;

    SRB_ExecSCSICmd cmd;
    std::memset(&cmd, 0, sizeof(cmd));
    cmd.SRB_BufLen = sizeof(ReadBuffer);
    cmd.SRB_BufPointer = ReadBuffer;
    cmd.SRB_CDBLen = 12;
    cmd.SRB_Flags = SRB_DIR_IN;

    cmd.CDBByte[0] = 0xBE; // READ CD LBA
    cmd.CDBByte[2] = (BYTE)(lba >> 24);
    cmd.CDBByte[3] = (BYTE)(lba >> 16);
    cmd.CDBByte[4] = (BYTE)(lba >> 8);
    cmd.CDBByte[5] = (BYTE)(lba);
    cmd.CDBByte[8] = 1;
    cmd.CDBByte[10] = 0x02; // SubQ

    if (m_ReadSubQMethod == 0)
    {
        cmd.CDBByte[9] = 0x10; // UserData
        m_Aspi->ExecuteCommand(cmd);

        if (cmd.SRB_Status != SS_COMP || ReadBuffer[2352] == 0x00)
        {
            cmd.CDBByte[9] = 0xF8;
            m_Aspi->ExecuteCommand(cmd);
            if (cmd.SRB_Status == SS_COMP) m_ReadSubQMethod = 1;
        }
    }
    else
    {
        cmd.CDBByte[9] = 0xF8;
        m_Aspi->ExecuteCommand(cmd);

        if (cmd.SRB_Status != SS_COMP)
        {
            cmd.CDBByte[9] = 0x10;
            m_Aspi->ExecuteCommand(cmd);
            if (cmd.SRB_Status == SS_COMP) m_ReadSubQMethod = 0;
        }
    }

    if (cmd.SRB_Status == SS_COMP)
    {
        std::memcpy(Buffer, ReadBuffer + 2352, 16);
        return true;
    }

    m_SRB_Status = cmd.SRB_Status;
    m_SK = cmd.SenseArea[2] & 0x0f;
    m_ASC = cmd.SenseArea[12];
    m_ASCQ = cmd.SenseArea[13];
    return false;
}

bool CMMCReader::ReadATIP(BYTE* Buffer)
{
    if (m_Aspi == nullptr) return false;

    const DWORD BufferSize = 400;

    SRB_ExecSCSICmd cmd;
    std::memset(&cmd, 0, sizeof(cmd));
    cmd.SRB_BufLen = BufferSize;
    cmd.SRB_BufPointer = Buffer;
    cmd.SRB_CDBLen = 10;
    cmd.SRB_Flags = SRB_DIR_IN;

    cmd.CDBByte[0] = 0x43;  // READ TOC/PMA/ATIP
    cmd.CDBByte[1] = 0x02;  // MSF=1
    cmd.CDBByte[2] = 0x04;  // ATIP format
    cmd.CDBByte[7] = (BYTE)((BufferSize >> 8) & 0xFF);
    cmd.CDBByte[8] = (BYTE)(BufferSize & 0xFF);

    m_Aspi->ExecuteCommand(cmd);

    if (cmd.SRB_Status == SS_COMP) return true;

    m_SRB_Status = cmd.SRB_Status;
    m_SK = cmd.SenseArea[2] & 0x0f;
    m_ASC = cmd.SenseArea[12];
    m_ASCQ = cmd.SenseArea[13];
    return false;
}

int CMMCReader::ReadCDText(BYTE* Buffer)
{
    if (m_Aspi == nullptr) return 0;

    BYTE ReadBuffer[5000];
    std::memset(ReadBuffer, 0, sizeof(ReadBuffer));

    SRB_ExecSCSICmd cmd;
    std::memset(&cmd, 0, sizeof(cmd));
    cmd.SRB_Flags = SRB_DIR_IN;
    cmd.SRB_BufLen = sizeof(ReadBuffer);
    cmd.SRB_BufPointer = ReadBuffer;
    cmd.SRB_CDBLen = 10;

    cmd.CDBByte[0] = 0x43; // READ TOC/PMA/ATIP
    cmd.CDBByte[2] = 0x05; // CD-TEXT
    cmd.CDBByte[7] = (BYTE)((sizeof(ReadBuffer) >> 8) & 0xFF);
    cmd.CDBByte[8] = (BYTE)(sizeof(ReadBuffer) & 0xFF);

    m_Aspi->ExecuteCommand(cmd);

    if (cmd.SRB_Status != SS_COMP)
    {
        m_SRB_Status = cmd.SRB_Status;
        m_SK = cmd.SenseArea[2] & 0x0f;
        m_ASC = cmd.SenseArea[12];
        m_ASCQ = cmd.SenseArea[13];
        return 0;
    }

    const int len = (ReadBuffer[0] * 0x100) + ReadBuffer[1] - 2;
    if (len <= 0) return 0;

    std::memcpy(Buffer, ReadBuffer + 4, len);
    return len;
}
