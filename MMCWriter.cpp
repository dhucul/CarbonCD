// mmcwriter_fixed.cpp
#include "StdAfx.h"
#include "mmcwriter.h"
#include "CDWriter.h"

#include <cstdint>   // uintptr_t
#include <cstring>   // memset, memcpy

// 16-byte align helper (safe on 32/64-bit)
#define PGB(a) ((BYTE*)((((uintptr_t)(a)) + 0x0F) & ~(uintptr_t)0x0F))

// This class allocates two ping-pong buffers sized for the largest frame mode (RAW+96)
// and max frames (27). Keep this consistent with ctor allocation.
static constexpr DWORD kMaxBufferedFramesHardLimit = 27;
static constexpr DWORD kMaxFrameSizeBytes = (2352 + 96);
static constexpr DWORD kBufferCapacityBytes = kMaxFrameSizeBytes * kMaxBufferedFramesHardLimit;

namespace
{
    struct SenseTriplet
    {
        BYTE sk{ 0 }, asc{ 0 }, ascq{ 0 };
    };

    static inline SenseTriplet ExtractSense(const SRB_ExecSCSICmd& cmd)
    {
        SenseTriplet s;
        s.sk = cmd.SenseArea[2] & 0x0f;
        s.asc = cmd.SenseArea[12];
        s.ascq = cmd.SenseArea[13];
        return s;
    }

    static inline bool ScsiExec(CAspi* aspi, SRB_ExecSCSICmd& cmd, SenseTriplet& outSense)
    {
        if (!aspi) return false;
        aspi->ExecuteCommand(cmd);
        if (cmd.SRB_Status != SS_COMP)
        {
            outSense = ExtractSense(cmd);
            return false;
        }
        return true;
    }

    static inline bool ScsiExecAsync(CAspi* aspi, SRB_ExecSCSICmd& cmd, SenseTriplet& outSense)
    {
        if (!aspi) return false;
        aspi->ExecuteCommandAsync(cmd);
        if (cmd.SRB_Status != SS_COMP)
        {
            outSense = ExtractSense(cmd);
            return false;
        }
        return true;
    }

    static inline BYTE bcd_to_bin(BYTE v)
    {
        return static_cast<BYTE>(((v >> 4) * 10) + (v & 0x0F));
    }

    struct WriteModeSpec
    {
        int  mode;
        BYTE writeTypeNibble; // low nibble of mp[2]
        BYTE blockTypeNibble; // low nibble of mp[4]
        DWORD frameSize;
    };

    static constexpr WriteModeSpec kWriteSpecs[] = {
        { WRITEMODE_RAW_96,  0x03, 0x03, 2352 + 96 }, // RAW + raw PW
        { WRITEMODE_RAW_P96, 0x03, 0x02, 2352 + 96 }, // RAW + packed PW
        { WRITEMODE_RAW_16,  0x03, 0x01, 2352 + 16 }, // RAW + PQ
        { WRITEMODE_2048,    0x02, 0x08, 2352       }, // legacy behavior preserved
    };

    static inline const WriteModeSpec* FindWriteSpec(int writeMode)
    {
        for (const auto& s : kWriteSpecs)
        {
            if (s.mode == writeMode) return &s;
        }
        return nullptr;
    }

    static inline DWORD ClampMaxFrames(int bufferingFrames)
    {
        if (bufferingFrames < 0) return 0;
        DWORD mf = static_cast<DWORD>(bufferingFrames);
        if (mf > kMaxBufferedFramesHardLimit) mf = kMaxBufferedFramesHardLimit;
        return mf;
    }
} // namespace

CMMCWriter::CMMCWriter(void)
{
    m_Aspi = nullptr;
    m_LeadInLBA = 0;
    m_LeadInSize = 0;

    m_FirstWriteFlag = true;
    m_WritingMode = 0;
    m_MaxFrames = 0;

    // Allocate buffers FIRST
    m_Buffer1 = m_PBuf1.CreateBuffer(kBufferCapacityBytes);
    m_Buffer2 = m_PBuf2.CreateBuffer(kBufferCapacityBytes);

    // Then select active buffer
    m_Buffer = m_Buffer1;

    // Initialize buffering state
    m_BufferingFrames = 0;
    m_BufferPoint = 0;
    m_BufferLBA = 0;
    m_FrameSize = kMaxFrameSizeBytes; // default (will be set by SetWriteParam)

    // Error defaults
    m_SK = 0;
    m_ASC = 0;
    m_ASCQ = 0;
}

CMMCWriter::~CMMCWriter(void)
{
}

void CMMCWriter::Initialize(CAspi* aspi)
{
    m_Aspi = aspi;
}

bool CMMCWriter::CheckDisc(void)
{
    if (m_Aspi == nullptr)
    {
        return false;
    }

    CPBBuffer PBuffer;
    BYTE* Buffer;
    SRB_ExecSCSICmd cmd;

    Buffer = PBuffer.CreateBuffer(256);
    memset(&cmd, 0, sizeof(cmd));
    cmd.SRB_BufLen = 256;
    cmd.SRB_BufPointer = Buffer;
    cmd.SRB_CDBLen = 10;
    cmd.SRB_Flags = SRB_DIR_IN;
    cmd.CDBByte[0] = 0x51; // READ DISC INFORMATION
    cmd.CDBByte[7] = static_cast<BYTE>(255 >> 8);
    cmd.CDBByte[8] = static_cast<BYTE>(255 & 0xff);

    SenseTriplet s{};
    if (!ScsiExec(m_Aspi, cmd, s))
    {
        m_SK = s.sk; m_ASC = s.asc; m_ASCQ = s.ascq;
        m_LeadInLBA = 0;
        m_LeadInSize = 0;
        return false;
    }

    m_LeadInLBA = 0;
    m_LeadInSize = 0;

    if ((Buffer[2] & 0x03) != 0)
    {
        m_SK = 0;
        m_ASC = 0;
        m_ASCQ = 0;
        return false;
    }

    m_LeadInLBA = Buffer[19] + 75 * (Buffer[18] + 60 * (Buffer[17]));
    m_LeadInSize = 450000 - (Buffer[19] + 75 * (Buffer[18] + 60 * (Buffer[17])));
    return true;
}

DWORD CMMCWriter::GetLeadInLBA(void)
{
    return m_LeadInLBA;
}

DWORD CMMCWriter::GetLeadInSize(void)
{
    return m_LeadInSize;
}

bool CMMCWriter::SetWriteParam(int WriteMode, bool BurnProof, bool TestMode, int BufferingFrames)
{
    if (m_Aspi == nullptr)
    {
        return false;
    }

    const WriteModeSpec* spec = FindWriteSpec(WriteMode);
    if (!spec)
    {
        return false;
    }

    BYTE* Buffer;
    BYTE B[256 + 15];
    DWORD DataPoint;
    BYTE* mp;
    BYTE PageLen;

    Buffer = PGB(B);

    DataPoint = m_Aspi->ModeSense(Buffer, 256, 1, 5); // get default setting
    if (DataPoint == 0)
    {
        return false;
    }

    mp = Buffer + DataPoint;
    if ((mp[2] & 0x40) == 0)
    {
        BurnProof = false;
    }

    DataPoint = m_Aspi->ModeSense(Buffer, 256, 2, 5); // get default setting
    if (DataPoint == 0)
    {
        return false;
    }

    mp = Buffer + DataPoint;
    PageLen = mp[1] + 2;

    memset(mp + 2, 0, 0x36);

    if (TestMode) mp[2] |= 0x10;
    else          mp[2] &= static_cast<BYTE>(~0x10);

    if (BurnProof) mp[2] |= 0x40;
    else           mp[2] &= static_cast<BYTE>(~0x40);

    // Apply spec
    mp[2] = static_cast<BYTE>((mp[2] & 0xF0) | (spec->writeTypeNibble & 0x0F));
    mp[4] = static_cast<BYTE>((mp[4] & 0xF0) | (spec->blockTypeNibble & 0x0F));
    m_FrameSize = spec->frameSize;

    if (!m_Aspi->ModeSelect(Buffer, PageLen + DataPoint))
    {
        return false;
    }

    // reset buffering state (must be done inside member function)
    m_BufferingFrames = 0;
    m_BufferPoint = 0;
    m_BufferLBA = 0;
    m_FirstWriteFlag = true;
    m_WritingMode = WriteMode;

    m_MaxFrames = ClampMaxFrames(BufferingFrames);
    m_Buffer = m_Buffer1;

    return true;
}

void CMMCWriter::ResetParams(void)
{
    if (m_Aspi == nullptr)
    {
        return;
    }

    BYTE* Buffer;
    BYTE B[256 + 15];
    DWORD DataPoint;
    BYTE PageLen;

    Buffer = PGB(B);
    DataPoint = m_Aspi->ModeSense(Buffer, 256, 2, 5); // get default setting
    if (DataPoint == 0)
    {
        return;
    }

    PageLen = Buffer[DataPoint + 1] + 2;
    m_Aspi->ModeSelect(Buffer, PageLen + DataPoint);
}

bool CMMCWriter::WriteBuffering(BYTE* Buffer, DWORD lba)
{
    if (m_Aspi == nullptr)
    {
        return false;
    }

    if (m_MaxFrames == 0)
    {
        return Write(Buffer, lba, 1);
    }

    if (m_BufferingFrames == 0)
    {
        m_BufferPoint = 0;
        m_BufferLBA = lba;
    }

    if (m_BufferPoint + m_FrameSize > kBufferCapacityBytes)
    {
        return false;
    }

    memcpy(m_Buffer + m_BufferPoint, Buffer, m_FrameSize);
    m_BufferingFrames++;
    m_BufferPoint += m_FrameSize;

    if (m_BufferingFrames >= m_MaxFrames)
    {
        bool ok = Write(m_Buffer, m_BufferLBA, m_BufferingFrames);
        m_BufferingFrames = 0;
        m_BufferPoint = 0;
        m_Buffer = (m_Buffer == m_Buffer1) ? m_Buffer2 : m_Buffer1;
        return ok;
    }

    return true;
}

bool CMMCWriter::WriteBufferingEx(BYTE* Buffer, DWORD lba, DWORD Size)
{
    if (m_Aspi == nullptr)
    {
        return false;
    }

    if (m_FrameSize == 0 || (Size % m_FrameSize) != 0)
    {
        return false;
    }

    DWORD framesToAdd = Size / m_FrameSize;

    if (m_MaxFrames == 0)
    {
        return Write(Buffer, lba, framesToAdd);
    }

    if (m_BufferingFrames == 0)
    {
        m_BufferPoint = 0;
        m_BufferLBA = lba;
    }

    if (m_BufferPoint + Size > kBufferCapacityBytes)
    {
        return false;
    }

    memcpy(m_Buffer + m_BufferPoint, Buffer, Size);
    m_BufferingFrames += framesToAdd;
    m_BufferPoint += Size;

    if (m_BufferingFrames >= m_MaxFrames)
    {
        bool ok = Write(m_Buffer, m_BufferLBA, m_BufferingFrames);
        m_BufferingFrames = 0;
        m_BufferPoint = 0;
        m_Buffer = (m_Buffer == m_Buffer1) ? m_Buffer2 : m_Buffer1;
        return ok;
    }

    return true;
}

bool CMMCWriter::Write(BYTE* Buffer, DWORD lba, DWORD WriteLen)
{
    if (m_Aspi == nullptr)
    {
        return false;
    }

    if (WriteLen == 0)
    {
        return true;
    }

    const DWORD BufferLength = WriteLen * m_FrameSize;

    if (m_FirstWriteFlag)
    {
        m_FirstWriteFlag = false;
        m_Aspi->InitialAsync();
    }

    SRB_ExecSCSICmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.SRB_BufLen = BufferLength;
    cmd.SRB_BufPointer = Buffer;
    cmd.SRB_CDBLen = 10;
    cmd.SRB_Flags = SRB_DIR_OUT;
    cmd.CDBByte[0] = 0x2A; // WRITE(10)
    cmd.CDBByte[1] = 0;
    cmd.CDBByte[2] = static_cast<BYTE>(lba >> 24);
    cmd.CDBByte[3] = static_cast<BYTE>(lba >> 16);
    cmd.CDBByte[4] = static_cast<BYTE>(lba >> 8);
    cmd.CDBByte[5] = static_cast<BYTE>(lba >> 0);
    cmd.CDBByte[6] = 0;
    cmd.CDBByte[7] = static_cast<BYTE>(WriteLen >> 8);
    cmd.CDBByte[8] = static_cast<BYTE>(WriteLen >> 0);

    SenseTriplet s{};
    if (!ScsiExecAsync(m_Aspi, cmd, s))
    {
        m_SK = s.sk; m_ASC = s.asc; m_ASCQ = s.ascq;
        return false;
    }
    return true;
}

int CMMCWriter::TestUnitReady(void)
{
    if (m_Aspi == nullptr)
    {
        return 1; // treat as error, NOT "ready"
    }

    SRB_ExecSCSICmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.SRB_BufLen = 0;
    cmd.SRB_BufPointer = nullptr;
    cmd.SRB_CDBLen = 6;
    cmd.CDBByte[0] = 0x00; // TEST UNIT READY

    m_Aspi->ExecuteCommand(cmd);

    if (cmd.SRB_Status != SS_COMP)
    {
        SenseTriplet s = ExtractSense(cmd);
        m_SK = s.sk; m_ASC = s.asc; m_ASCQ = s.ascq;
        return 1;
    }

    if ((cmd.SenseArea[2] & 0x0f) == 2) return 2;
    if ((cmd.SenseArea[2] & 0x0f) == 6) return 2;

    return 0;
}

bool CMMCWriter::FlushBuffer(void)
{
    if (m_Aspi == nullptr)
    {
        return false;
    }

    if (m_BufferingFrames != 0)
    {
        if (!Write(m_Buffer, m_BufferLBA, m_BufferingFrames))
        {
            return false;
        }
        m_BufferingFrames = 0;
        m_BufferPoint = 0;
    }

    m_Aspi->FinalizeAsync();

    SRB_ExecSCSICmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.SRB_BufLen = 0;
    cmd.SRB_BufPointer = nullptr;
    cmd.SRB_CDBLen = 10;
    cmd.SRB_Flags = SRB_DIR_OUT;
    cmd.CDBByte[0] = 0x35; // SYNCHRONIZE CACHE

    SenseTriplet s{};
    if (!ScsiExec(m_Aspi, cmd, s))
    {
        m_SK = s.sk; m_ASC = s.asc; m_ASCQ = s.ascq;
        return false;
    }
    return true;
}

bool CMMCWriter::SetCueSheet(BYTE* Buffer, int EntryNumber)
{
    if (m_Aspi == nullptr)
    {
        return false;
    }

    if (EntryNumber <= 0)
    {
        return false;
    }

    const DWORD BufferLength = static_cast<DWORD>(EntryNumber) * 8;

    CPBBuffer PBuffer;
    BYTE* CueBuffer = PBuffer.CreateBuffer(2000);

    // Patch -> modify cue-sheet
    {
        memcpy(CueBuffer, Buffer, BufferLength);
        memset(CueBuffer + 4, 0, 4);

        for (int i = 1; i < EntryNumber; i++)
        {
            BYTE* p = CueBuffer + i * 8;

            if (p[1] != 0xaa)
            {
                p[1] = bcd_to_bin(p[1]);
            }

            p[5] = bcd_to_bin(p[5]);
            p[6] = bcd_to_bin(p[6]);
            p[7] = bcd_to_bin(p[7]);
        }
    }

    SRB_ExecSCSICmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.SRB_BufLen = BufferLength;
    cmd.SRB_BufPointer = CueBuffer;
    cmd.SRB_CDBLen = 10;
    cmd.SRB_Flags = SRB_DIR_OUT;
    cmd.CDBByte[0] = 0x5D; // SEND CUE SHEET (MMC)
    cmd.CDBByte[6] = static_cast<BYTE>(BufferLength >> 16);
    cmd.CDBByte[7] = static_cast<BYTE>(BufferLength >> 8);
    cmd.CDBByte[8] = static_cast<BYTE>(BufferLength >> 0);

    SenseTriplet s{};
    if (!ScsiExec(m_Aspi, cmd, s))
    {
        m_SK = s.sk; m_ASC = s.asc; m_ASCQ = s.ascq;
        return false;
    }
    return true;
}

bool CMMCWriter::PerformPowerCalibration(void)
{
    if (m_Aspi == nullptr)
    {
        return false;
    }

    SRB_ExecSCSICmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.SRB_BufLen = 0;
    cmd.SRB_BufPointer = nullptr;
    cmd.SRB_CDBLen = 10;
    cmd.CDBByte[0] = 0x54; // SEND OPC INFORMATION
    cmd.CDBByte[1] = 0x01;

    SenseTriplet s{};
    if (!ScsiExec(m_Aspi, cmd, s))
    {
        m_SK = s.sk; m_ASC = s.asc; m_ASCQ = s.ascq;
        return false;
    }
    return true;
}

bool CMMCWriter::PreventMediaRemoval(bool BlockFlag)
{
    if (m_Aspi == nullptr)
    {
        return false;
    }

    SRB_ExecSCSICmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.SRB_BufLen = 0;
    cmd.SRB_BufPointer = nullptr;
    cmd.SRB_CDBLen = 6;
    cmd.CDBByte[0] = 0x1E; // PREVENT/ALLOW MEDIA REMOVAL
    cmd.CDBByte[4] = BlockFlag ? 0x01 : 0x00;

    SenseTriplet s{};
    if (!ScsiExec(m_Aspi, cmd, s))
    {
        m_SK = s.sk; m_ASC = s.asc; m_ASCQ = s.ascq;
        return false;
    }
    return true;
}

bool CMMCWriter::ReWind(void)
{
    if (m_Aspi == nullptr)
    {
        return false;
    }

    SRB_ExecSCSICmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.SRB_BufLen = 0;
    cmd.SRB_BufPointer = nullptr;
    cmd.SRB_CDBLen = 6;
    cmd.CDBByte[0] = 0x01; // REWIND

    SenseTriplet s{};
    if (!ScsiExec(m_Aspi, cmd, s))
    {
        m_SK = s.sk; m_ASC = s.asc; m_ASCQ = s.ascq;
        return false;
    }
    return true;
}

bool CMMCWriter::LoadTray(BYTE LoUnlo)
{
    if (m_Aspi == nullptr)
    {
        return false;
    }

    SRB_ExecSCSICmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.SRB_BufLen = 0;
    cmd.SRB_BufPointer = nullptr;
    cmd.SRB_CDBLen = 6;
    cmd.CDBByte[0] = 0x1b; // LOAD/UNLOAD
    cmd.CDBByte[4] = LoUnlo;

    SenseTriplet s{};
    if (!ScsiExec(m_Aspi, cmd, s))
    {
        m_SK = s.sk; m_ASC = s.asc; m_ASCQ = s.ascq;
        return false;
    }
    return true;
}

bool CMMCWriter::IsCDR(void)
{
    if (m_Aspi == nullptr)
    {
        return false;
    }

    BYTE* Buffer;
    BYTE B[256 + 15];
    DWORD DataPoint;
    BYTE* mp;

    Buffer = PGB(B);
    DataPoint = m_Aspi->ModeSense(Buffer, 256, 0, 0x2A); // get Drive feature
    if (DataPoint == 0)
    {
        return false;
    }

    mp = Buffer + DataPoint;
    return ((mp[3] & 0x03) != 0);
}

int CMMCWriter::GetBufferSize(void)
{
    if (m_Aspi == nullptr)
    {
        return 0;
    }

    BYTE* Buffer;
    BYTE B[256 + 15];
    DWORD DataPoint;
    BYTE* mp;

    Buffer = PGB(B);
    DataPoint = m_Aspi->ModeSense(Buffer, 256, 0, 0x2A); // get Drive feature
    if (DataPoint == 0)
    {
        return 0;
    }

    mp = Buffer + DataPoint;
    return mp[12] * 0x100 + mp[13];
}

bool CMMCWriter::SetWriteSpeed(BYTE Speed)
{
    if (m_Aspi == nullptr)
    {
        return false;
    }

    SRB_ExecSCSICmd cmd;
    DWORD PSpeed = (Speed * 2352 * 75) / 1000;

    if (Speed == 0xff)
    {
        PSpeed = 0xffff;
    }

    memset(&cmd, 0, sizeof(cmd));
    cmd.SRB_BufLen = 0;
    cmd.SRB_BufPointer = nullptr;
    cmd.SRB_CDBLen = 12;
    cmd.CDBByte[0] = 0xbb; // SET CD SPEED

    // read speed
    cmd.CDBByte[2] = 0xff;
    cmd.CDBByte[3] = 0xff;

    // write speed
    cmd.CDBByte[4] = static_cast<BYTE>(PSpeed >> 8);
    cmd.CDBByte[5] = static_cast<BYTE>(PSpeed);

    SenseTriplet s{};
    if (!ScsiExec(m_Aspi, cmd, s))
    {
        m_SK = s.sk; m_ASC = s.asc; m_ASCQ = s.ascq;
        return false;
    }
    return true;
}

bool CMMCWriter::EraseMedia(bool FastErase)
{
    if (m_Aspi == nullptr)
    {
        return false;
    }

    SRB_ExecSCSICmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.SRB_BufLen = 0;
    cmd.SRB_BufPointer = nullptr;
    cmd.SRB_CDBLen = 12;
    cmd.CDBByte[0] = 0xA1; // BLANK
    cmd.CDBByte[1] = FastErase ? 1 : 0;

    SenseTriplet s{};
    if (!ScsiExec(m_Aspi, cmd, s))
    {
        m_SK = s.sk; m_ASC = s.asc; m_ASCQ = s.ascq;
        return false;
    }
    return true;
}

void CMMCWriter::GetErrorParams(BYTE& SK, BYTE& ASC, BYTE& ASCQ)
{
    SK = m_SK;
    ASC = m_ASC;
    ASCQ = m_ASCQ;
}

bool CMMCWriter::ReadTrackInfo(BYTE* Buffer)
{
    if (m_Aspi == nullptr)
    {
        return false;
    }

    SRB_ExecSCSICmd cmd;
    CPBBuffer PBuffer;
    PBuffer.CreateBuffer(36);

    memset(&cmd, 0, sizeof(cmd));
    cmd.SRB_BufLen = 36;
    cmd.SRB_BufPointer = PBuffer.GetBuffer();
    cmd.SRB_CDBLen = 10;
    cmd.SRB_Flags = SRB_DIR_IN;
    cmd.CDBByte[0] = 0x52; // READ TRACK INFORMATION
    cmd.CDBByte[1] = 0x01;
    cmd.CDBByte[5] = 0xff;
    cmd.CDBByte[8] = 0x1c;

    SenseTriplet s{};
    if (ScsiExec(m_Aspi, cmd, s))
    {
        memcpy(Buffer, PBuffer.GetBuffer(), 36);
        return true;
    }

    m_SK = s.sk; m_ASC = s.asc; m_ASCQ = s.ascq;
    return false;
}
