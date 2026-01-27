#include "StdAfx.h"
#include "cdwriter.h"
#include "Setting.h"

// Fixes applied:
//  - delete[] vs delete for file buffer
//  - safer path split (no overflow) + handles no-backslash case
//  - validates GetFileSize / ReadFile
//  - fixes WriteRawGap() reading m_WriteBuffer[2352] after zeroing only 2352 bytes
//  - removes unreachable return
//  - fixes suspicious static_cast<int> -> static_cast<DWORD> for lba

CCDWriter::CCDWriter(void)
{
    // create scramble table
    {
        int i, j;
        WORD Counter;
        memset(m_ScrambleTable, 0, 2340);
        Counter = 1;

        for (i = 0; i < 2340; i++)
        {
            for (j = 0; j < 8; j++)
            {
                if (Counter & 1)
                {
                    m_ScrambleTable[i] = m_ScrambleTable[i] | (1 << j);
                }

                if ((Counter & 1) != ((Counter & 2) >> 1))
                {
                    Counter = Counter | 0x8000;
                }

                Counter = Counter >> 1;
            }
        }
    }
    m_WritingMode = 0;
}

CCDWriter::~CCDWriter(void)
{
}

void CCDWriter::Initialize(CAspi* aspi)
{
    m_MMCWriter.Initialize(aspi);
}

static void SplitDirFromPathSafe(LPCSTR fileName, char* outDir, size_t outDirSize)
{
    if (!outDir || outDirSize == 0)
        return;

    outDir[0] = '\0';
    if (!fileName || !*fileName)
        return;

#if defined(_MSC_VER)
    // Copy full path into outDir (truncated safely)
    strncpy_s(outDir, outDirSize, fileName, _TRUNCATE);
#else
    // Fallback: ensure null-termination
    strncpy(outDir, fileName, outDirSize - 1);
    outDir[outDirSize - 1] = '\0';
#endif

    // Find last backslash
    char* lastSlash = nullptr;
    for (char* it = outDir; *it != '\0'; ++it)
    {
        if (*it == '\\')
            lastSlash = it;
    }

    if (lastSlash)
        *lastSlash = '\0';     // keep directory portion
    else
        outDir[0] = '\0';      // no directory component
}

bool CCDWriter::ParseCueSheetFile(LPCSTR FileName)
{
    HANDLE hFile;
    char Path[512];
    DWORD FileSize;
    DWORD read = 0;

    // split dir name safely
    SplitDirFromPathSafe(FileName, Path, sizeof(Path));

    hFile = CreateFile(FileName, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        m_ErrorMessage.Format(MSG(1));
        return false;
    }

    FileSize = GetFileSize(hFile, nullptr);
    if (FileSize == INVALID_FILE_SIZE)
    {
        CloseHandle(hFile);
        m_ErrorMessage = "Failed to get file size.";
        return false;
    }

    char* buf = new char[FileSize + 1];

    BOOL ok = ReadFile(hFile, buf, FileSize, &read, nullptr);
    CloseHandle(hFile);

    if (!ok || read != FileSize)
    {
        delete[] buf;
        m_ErrorMessage = "Failed to read cue sheet file.";
        return false;
    }

    buf[FileSize] = '\0';

    if (!m_CueSheetParser.Parse(buf, Path, 0))
    {
        m_ErrorMessage.Format(MSG(2), m_CueSheetParser.GetErrorMessage());
        delete[] buf;
        return false;
    }

    delete[] buf;
    return true;
}

bool CCDWriter::ParseCueSheet(LPCSTR CueSheet, DWORD ImageSize)
{
    if (!m_CueSheetParser.Parse(CueSheet, "", ImageSize))
    {
        m_ErrorMessage.Format(MSG(2), m_CueSheetParser.GetErrorMessage());
        return false;
    }
    return true;
}

LPCSTR CCDWriter::GetErrorMessage(void)
{
    return m_ErrorMessage;
}

bool CCDWriter::StartWriting(bool ModeMS)
{
    // clear counter
    m_LeadInCounter = 450000;

    if (ModeMS)
    {
        m_MMCWriter.PreventMediaRemoval(true);
    }
    else
    {
        // create cue-sheet as MMC format
        m_CueSheetParser.CreateMMCCueSheet(m_MMCWriter.GetLeadInLBA());
        m_SubCode.SetCueSheet(m_CueSheetParser.GetMMCCueSheet(), m_CueSheetParser.GetEntryCount());
        m_MMCWriter.PreventMediaRemoval(true);

        if ((*(m_CueSheetParser.GetMMCCueSheet()) & 0xf0) == 0)
            m_LeadInMode = false;
        else
            m_LeadInMode = true;

        m_DataMode = m_CueSheetParser.GetDataMode();

        if (m_WritingMode == WRITEMODE_2048)
        {
            BYTE Buffer[36];
            if (!m_MMCWriter.ReadTrackInfo(Buffer))
            {
                m_ErrorMessage = "Failed to read track info.";
                return false;
            }

            m_MMCWriter.SetCueSheet(m_CueSheetParser.GetMMCCueSheet(), m_CueSheetParser.GetEntryCount());
        }
    }

    return true;
}

void CCDWriter::FlushBuffer(void)
{
    m_MMCWriter.FlushBuffer();
}

void CCDWriter::FinishWriting(void)
{
    m_MMCWriter.FlushBuffer();

    while (m_MMCWriter.TestUnitReady() == 2)
        Sleep(1000);

    m_MMCWriter.PreventMediaRemoval(false);
    m_MMCWriter.ReWind();
}

void CCDWriter::AbortWriting(void)
{
    m_MMCWriter.FlushBuffer();

    while (m_MMCWriter.TestUnitReady() == 2)
        Sleep(1000);

    m_MMCWriter.PreventMediaRemoval(false);
    m_MMCWriter.ReWind();
}

DWORD CCDWriter::GetLeadInSize(void)
{
    return m_MMCWriter.GetLeadInSize();
}

void CCDWriter::Sub16To96(BYTE* Buffer)
{
    BYTE Sub16[16];
    BYTE Mask, * p;
    int i;
    memcpy(Sub16, Buffer, 16);
    Mask = Sub16[15];
    memset(Buffer, 0, 96);
    p = Buffer;

    for (i = 0; i < 12; i++)
    {
        p[0] = Mask | ((Sub16[i] >> 1) & 0x40);
        p[1] = Mask | ((Sub16[i] >> 0) & 0x40);
        p[2] = Mask | ((Sub16[i] << 1) & 0x40);
        p[3] = Mask | ((Sub16[i] << 2) & 0x40);
        p[4] = Mask | ((Sub16[i] << 3) & 0x40);
        p[5] = Mask | ((Sub16[i] << 4) & 0x40);
        p[6] = Mask | ((Sub16[i] << 5) & 0x40);
        p[7] = Mask | ((Sub16[i] << 6) & 0x40);
        p += 8;
    }
}

bool CCDWriter::WriteRaw(BYTE* Buffer)
{
    DWORD lba;
    bool ret;

    lba = static_cast<DWORD>(m_SubCode.GenerateSub16(m_WriteBuffer + 2352));
    memcpy(m_WriteBuffer, Buffer, 2352);

    if (m_WritingMode == WRITEMODE_RAW_96 || m_WritingMode == WRITEMODE_RAW_P96)
    {
        Scramble(m_WriteBuffer);
        Sub16To96(m_WriteBuffer + 2352);
        ret = m_MMCWriter.WriteBuffering(m_WriteBuffer, lba - 150);
    }
    else if (m_WritingMode == WRITEMODE_RAW_16)
    {
        Scramble(m_WriteBuffer);
        ret = m_MMCWriter.WriteBuffering(m_WriteBuffer, lba - 150);
    }
    else
    {
        if ((m_WriteBuffer[2352] & 0xf0) == 0x00)
        {
            ret = m_MMCWriter.WriteBuffering(m_WriteBuffer, lba - 150);
        }
        else
        {
            if (m_DataMode == 1)
                ret = m_MMCWriter.WriteBufferingEx(m_WriteBuffer + 16, lba - 150, 2048);
            else
                ret = m_MMCWriter.WriteBufferingEx(m_WriteBuffer + 16 + 8, lba - 150, 2048);
        }
    }

    return ret;
}

bool CCDWriter::WriteRaw96(BYTE* Buffer, DWORD lba)
{
    return m_MMCWriter.WriteBuffering(Buffer, lba - 150);
}

bool CCDWriter::WriteRawLeadIn(void)
{
    DWORD lba;
    lba = static_cast<DWORD>(m_SubCode.GenerateSub16(m_WriteBuffer + 2352));

    if (m_WritingMode == WRITEMODE_2048)
        return true;

    GenerateZeroAreaLeadIn(m_WriteBuffer, lba);
    m_LeadInCounter++;

    if (m_WritingMode == WRITEMODE_RAW_96 || m_WritingMode == WRITEMODE_RAW_P96)
    {
        Sub16To96(m_WriteBuffer + 2352);
        return m_MMCWriter.WriteBuffering(m_WriteBuffer, lba - 150);
    }
    if (m_WritingMode == WRITEMODE_RAW_16)
    {
        return m_MMCWriter.WriteBuffering(m_WriteBuffer, lba - 150);
    }

    return false;
}

bool CCDWriter::WriteRawGap(void)
{
    DWORD lba;
    lba = static_cast<DWORD>(m_SubCode.GenerateSub16(m_WriteBuffer + 2352));

    // Build a proper gap sector (data + subcode) for raw modes
    GenerateZeroArea(m_WriteBuffer, lba);

    if (m_WritingMode == WRITEMODE_RAW_96 || m_WritingMode == WRITEMODE_RAW_P96)
    {
        Sub16To96(m_WriteBuffer + 2352);
        return m_MMCWriter.WriteBuffering(m_WriteBuffer, lba - 150);
    }
    if (m_WritingMode == WRITEMODE_RAW_16)
    {
        return m_MMCWriter.WriteBuffering(m_WriteBuffer, lba - 150);
    }

    // Non-raw modes: decide based on the subcode header BEFORE clearing data area.
    const bool isAudio = ((m_WriteBuffer[2352] & 0xf0) == 0x00);

    // Clear user data area
    memset(m_WriteBuffer, 0, 2352);

    if (isAudio)
        return m_MMCWriter.WriteBuffering(m_WriteBuffer, lba - 150);

    return m_MMCWriter.WriteBufferingEx(m_WriteBuffer + 16, lba - 150, 2048);
}

bool CCDWriter::OPC(void)
{
    return m_MMCWriter.PerformPowerCalibration();
}

bool CCDWriter::SetWritingParams(int WritingMode, bool BurnProof, bool TestMode, int BufferingFrames)
{
    m_WritingMode = WritingMode;
    m_ErrorMessage.Format(MSG(3));
    return m_MMCWriter.SetWriteParam(WritingMode, BurnProof, TestMode, BufferingFrames);
}

bool CCDWriter::LoadTray(bool LoadingMode)
{
    m_ErrorMessage.Format(MSG(4));

    if (LoadingMode)
        return m_MMCWriter.LoadTray(0x03);

    return m_MMCWriter.LoadTray(0x02);
}

LPCSTR CCDWriter::GetImageFileName(void)
{
    return m_CueSheetParser.GetImageFileName();
}

DWORD CCDWriter::GetImageFrames(void)
{
    return m_CueSheetParser.GetTotalFrames();
}

void CCDWriter::GenerateZeroArea(BYTE* Buffer, DWORD lba)
{
    BYTE* SubQ = Buffer + 2352;

    // scramble only data
    if ((SubQ[0] & 0xf0) == 0)
    {
        memset(Buffer, 0, 2352);
    }
    else
    {
        memset(Buffer, 0, 2352);

        if (lba > 0x80000000)
            lba += 450000;

        if (m_DataMode == 1)
        {
            m_EDC.Mode1Raw(Buffer,
                static_cast<BYTE>(lba / (75 * 60)),
                static_cast<BYTE>((lba / 75) % 60),
                static_cast<BYTE>(lba % 75));
        }
        else
        {
            memset(Buffer + 1, 0xff, 10);
            Buffer[12] = m_CueSheetParser.ToHex(static_cast<BYTE>(lba / (75 * 60)));
            Buffer[13] = m_CueSheetParser.ToHex(static_cast<BYTE>((lba / 75) % 60));
            Buffer[14] = m_CueSheetParser.ToHex(static_cast<BYTE>(lba % 75));
            Buffer[15] = 2;
            Buffer[0x12] = 0x20;
            Buffer[0x16] = 0x20;
        }

        Scramble(Buffer);
    }
}

void CCDWriter::GenerateZeroAreaLeadIn(BYTE* Buffer, DWORD lba)
{
    BYTE* SubQ = Buffer + 2352;

    if (!m_LeadInMode)
    {
        memset(Buffer, 0, 2352);
    }
    else
    {
        memset(Buffer, 0, 2352);

        if (lba > 0x80000000)
            lba += 450000;

        if (m_DataMode == 1)
        {
            m_EDC.Mode1Raw(Buffer,
                static_cast<BYTE>(lba / (75 * 60)),
                static_cast<BYTE>((lba / 75) % 60),
                static_cast<BYTE>(lba % 75));
        }
        else
        {
            memset(Buffer + 1, 0xff, 10);
            Buffer[12] = m_CueSheetParser.ToHex(static_cast<BYTE>(lba / (75 * 60)));
            Buffer[13] = m_CueSheetParser.ToHex(static_cast<BYTE>((lba / 75) % 60));
            Buffer[14] = m_CueSheetParser.ToHex(static_cast<BYTE>(lba % 75));
            Buffer[15] = 2;
            Buffer[0x12] = 0x20;
            Buffer[0x16] = 0x20;
        }

        ForceScramble(Buffer);
    }
}

void CCDWriter::Scramble(BYTE* Buffer)
{
    DWORD i;
    BYTE* SubQ = Buffer + 2352;

    if ((SubQ[0] & 0xf0) == 0)
        return;

    for (i = 0; i < 2340; i++)
        Buffer[i + 12] = Buffer[i + 12] ^ m_ScrambleTable[i];
}

void CCDWriter::ForceScramble(BYTE* Buffer)
{
    DWORD i;
    for (i = 0; i < 2340; i++)
        Buffer[i + 12] = Buffer[i + 12] ^ m_ScrambleTable[i];
}

bool CCDWriter::IsCDR(void)
{
    return m_MMCWriter.IsCDR();
}

bool CCDWriter::SetWriteSpeed(BYTE Speed)
{
    return m_MMCWriter.SetWriteSpeed(Speed);
}

bool CCDWriter::EraseMedia(bool FastErase)
{
    return m_MMCWriter.EraseMedia(FastErase);
}

int CCDWriter::GetWritingMode(void)
{
    return m_WritingMode;
}

int CCDWriter::GetBufferSize(void)
{
    return m_MMCWriter.GetBufferSize();
}

void CCDWriter::GetErrorParams(BYTE& SK, BYTE& ASC, BYTE& ASCQ)
{
    m_MMCWriter.GetErrorParams(SK, ASC, ASCQ);
}

bool CCDWriter::CheckDisc(void)
{
    if (m_MMCWriter.TestUnitReady() != 0)
    {
        m_ErrorMessage.Format(MSG(5));
        return false;
    }

    if (!m_MMCWriter.CheckDisc())
    {
        m_ErrorMessage.Format(MSG(6));
        return false;
    }

    return true;
}
