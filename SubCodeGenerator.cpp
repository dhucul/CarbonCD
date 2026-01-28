#include "StdAfx.h"
#include "SubCodeGenerator.h"

#include <cstring>  // memset, memcpy

CSubCodeGenerator::CSubCodeGenerator(void)
{
    // Generate CRC table (CCITT 0x1021)
    for (int i = 0; i < 256; ++i)
    {
        int crc = (i << 8);

        for (int c = 0; c < 8; ++c)
        {
            if (crc & 0x8000)
                crc = (crc << 1) ^ 0x1021;
            else
                crc = (crc << 1);
        }

        m_SubcodeCRCTable[i] = (crc & 0xFFFF);
    }

    // Defensive defaults
    m_CueSheet = nullptr;
    m_CueEntryCount = 0;
    m_CurrentEntry = 0;
    m_CurrentLBA = 0;
    m_RelativeLBA = 0;
    m_TocCounter = 0;
    m_3Counter = 0;
    m_P_Count = 0;
    m_TocCount = 0;

    std::memset(m_RawToc, 0, sizeof(m_RawToc));
}

CSubCodeGenerator::~CSubCodeGenerator(void)
{
}

void CSubCodeGenerator::SetCueSheet(const BYTE* MMCCueSheet, int EntryCount)
{
    m_CueSheet = MMCCueSheet;
    m_CueEntryCount = EntryCount;

    // Defensive guard
    if (!m_CueSheet || m_CueEntryCount <= 0)
    {
        m_CurrentEntry = 0;
        m_CurrentLBA = 0;
        m_RelativeLBA = 0;
        m_TocCounter = 0;
        m_3Counter = 0;
        m_P_Count = 0;
        m_TocCount = 0;
        std::memset(m_RawToc, 0, sizeof(m_RawToc));
        return;
    }

    m_CurrentLBA = ToBin(m_CueSheet[7]) + 75 * (ToBin(m_CueSheet[6]) + 60 * ToBin(m_CueSheet[5])) - 450000;
    m_RelativeLBA = 0;

    CreateToc();

    m_CurrentEntry = 0;
    m_TocCounter = 0;
    m_3Counter = 0;
    m_P_Count = 0;
}

DWORD CSubCodeGenerator::GenerateSub16(BYTE* Buffer)
{
    if (!Buffer)
        return m_CurrentLBA;

    if (!m_CueSheet || m_CueEntryCount <= 0)
    {
        std::memset(Buffer, 0, 16);
        CalcCRC(Buffer);
        return m_CurrentLBA;
    }

    BYTE m = 0, s = 0, f = 0;
    const BYTE* Cue = m_CueSheet + m_CurrentEntry * 8;

    // Check lead-out
    if (Cue[1] != 0xAA)
    {
        // Check increment entry
        const BYTE* CueNext = m_CueSheet + m_CurrentEntry * 8 + 8;
        const BYTE* CueNext2 = CueNext; // alias to match your original flow

        // Avoid reading beyond end (CueNext points to next entry)
        if (m_CurrentEntry + 1 < m_CueEntryCount)
        {
            Cue = CueNext2; // same as your "Cue = ... + 8"
            DWORD lba = ToBin(Cue[7]) + 75 * (ToBin(Cue[6]) + 60 * ToBin(Cue[5]));

            if (lba <= m_CurrentLBA && m_CurrentLBA < 0x80000000)
            {
                // increment entry
                m_CurrentEntry++;

                // clamp
                if (m_CurrentEntry >= m_CueEntryCount)
                    m_CurrentEntry = m_CueEntryCount - 1;

                Cue = m_CueSheet + m_CurrentEntry * 8;

                // CueNext after increment
                if (m_CurrentEntry + 1 < m_CueEntryCount)
                    CueNext = m_CueSheet + m_CurrentEntry * 8 + 8;
                else
                    CueNext = Cue; // fallback

                if (Cue[1] != 0 && Cue[2] == 0)
                {
                    m_RelativeLBA =
                        (ToBin(CueNext[7]) + 75 * (ToBin(CueNext[6]) + 60 * ToBin(CueNext[5])))
                        - (ToBin(Cue[7]) + 75 * (ToBin(Cue[6]) + 60 * ToBin(Cue[5])));
                }
                else
                {
                    m_RelativeLBA = 0;
                }
            }
        }

        if (m_CurrentEntry >= m_CueEntryCount)
            m_CurrentEntry = m_CueEntryCount - 1;
    }
    else
    {
        // lead-out: keep behavior the same (no special handling here)
    }

    Cue = m_CueSheet + m_CurrentEntry * 8;
    std::memset(Buffer, 0, 16);

    if (Cue[1] == 0)
    {
        // Lead-In sub code (TOC)
        if (m_TocCount <= 0)
        {
            // No TOC created; still generate something valid
            LBAtoMSF(m, s, f, m_CurrentLBA);
            Buffer[3] = ToHex(m);
            Buffer[4] = ToHex(s);
            Buffer[5] = ToHex(f);
            m_CurrentLBA++;
            m_RelativeLBA++;
            CalcCRC(Buffer);
            return m_CurrentLBA - 1;
        }

        BYTE* Toc = m_RawToc + m_TocCounter * 16;

        Buffer[0] = (BYTE)((Toc[1] >> 4) | (Toc[1] << 4));
        Buffer[2] = Toc[3];

        LBAtoMSF(m, s, f, m_CurrentLBA);
        Buffer[3] = ToHex(m);
        Buffer[4] = ToHex(s);
        Buffer[5] = ToHex(f);

        Buffer[7] = Toc[8];
        Buffer[8] = Toc[9];
        Buffer[9] = Toc[10];

        m_CurrentLBA++;
        m_RelativeLBA++;
        m_3Counter++;

        if (m_3Counter >= 3)
        {
            m_3Counter = 0;
            m_TocCounter = (m_TocCounter + 1) % m_TocCount;
        }
    }
    else
    {
        // CTR/ADR & TRACK & INDEX
        Buffer[0] = Cue[0];
        Buffer[1] = Cue[1];
        Buffer[2] = Cue[2];

        // relative
        LBAtoMSF(m, s, f, m_RelativeLBA);
        Buffer[3] = ToHex(m);
        Buffer[4] = ToHex(s);
        Buffer[5] = ToHex(f);

        // absolute
        LBAtoMSF(m, s, f, m_CurrentLBA);
        Buffer[7] = ToHex(m);
        Buffer[8] = ToHex(s);
        Buffer[9] = ToHex(f);

        // P channel
        if (m_P_Count > 0)
        {
            Buffer[15] = 0x80;
            m_P_Count--;
        }

        if (Cue[2] == 0)
            m_P_Count = 1;

        // increment address
        if (Cue[1] != 0xAA && Cue[2] == 0)
        {
            m_CurrentLBA++;
            m_RelativeLBA--;
        }
        else
        {
            m_CurrentLBA++;
            m_RelativeLBA++;
        }
    }

    CalcCRC(Buffer);
    return m_CurrentLBA - 1;
}

void CSubCodeGenerator::LBAtoMSF(BYTE& m, BYTE& s, BYTE& f, DWORD lba)
{
    if (lba > 0x80000000)
        lba += 450000;

    m = (BYTE)(lba / (75 * 60));
    s = (BYTE)((lba / 75) % 60);
    f = (BYTE)(lba % 75);
}

void CSubCodeGenerator::CreateToc(void)
{
    if (!m_CueSheet || m_CueEntryCount <= 0)
    {
        m_TocCount = 0;
        std::memset(m_RawToc, 0, sizeof(m_RawToc));
        return;
    }

    int i;
    const BYTE* Cue = nullptr;
    BYTE* Toc = nullptr;

    BYTE LastTrack = 0;
    BYTE LastTNO = 0;
    BYTE FirstADR = 0;
    BYTE LastADR = 0;
    BYTE M = 0, S = 0, F = 0;

    std::memset(m_RawToc, 0, sizeof(m_RawToc));

    for (i = 0; i < m_CueEntryCount; i++)
    {
        Cue = m_CueSheet + i * 8;

        if (Cue[1] == 1 && Cue[2] == 1)
            FirstADR = Cue[0];

        if (Cue[2] == 1 && Cue[1] != 0xAA)
        {
            LastTrack = ToBin(Cue[1]);
            LastTNO = Cue[1];
            LastADR = Cue[0];
        }

        if (Cue[1] == 0xAA)
        {
            M = Cue[5];
            S = Cue[6];
            F = Cue[7];
        }
    }

    // set first track info
    Toc = m_RawToc + 16 * LastTrack;
    std::memset(Toc, 0, 16);
    Toc[0] = 1;                        // session no
    Toc[1] = (BYTE)(0x10 | (FirstADR >> 4)); // ADR/CTL
    Toc[3] = 0xA0;                     // First Track
    Toc[8] = 1;

    // set last track info
    Toc = m_RawToc + 16 * (LastTrack + 1);
    std::memset(Toc, 0, 16);
    Toc[0] = 1;
    Toc[1] = (BYTE)(0x10 | (FirstADR >> 4)); // keep your original choice
    Toc[3] = 0xA1;                     // Last Track
    Toc[8] = LastTNO;

    // set Lead-Out position
    Toc = m_RawToc + 16 * (LastTrack + 2);
    std::memset(Toc, 0, 16);
    Toc[0] = 1;
    Toc[1] = (BYTE)(0x10 | (FirstADR >> 4));
    Toc[3] = 0xA2;                     // Lead-Out
    Toc[8] = M;
    Toc[9] = S;
    Toc[10] = F;

    for (i = 0; i < m_CueEntryCount; i++)
    {
        Cue = m_CueSheet + i * 8;

        if (Cue[2] == 1 && Cue[1] != 0xAA)
        {
            BYTE tno = ToBin(Cue[1]);

            // ensure we don't overrun the TOC buffer if something is malformed
            // (m_RawToc size is assumed to be enough for your cue sheets)
            Toc = m_RawToc + 16 * (tno - 1);
            std::memset(Toc, 0, 16);

            Toc[0] = 0x01;
            Toc[1] = (BYTE)(0x10 | (Cue[0] >> 4));
            Toc[2] = 0x00;
            Toc[3] = Cue[1];
            Toc[4] = 0x00;
            Toc[5] = 0x00;
            Toc[6] = 0x00;
            Toc[7] = 0x00;
            Toc[8] = Cue[5];
            Toc[9] = Cue[6];
            Toc[10] = Cue[7];
        }
    }

    m_TocCount = LastTrack + 3;
}

BYTE CSubCodeGenerator::ToHex(BYTE Bin)
{
    return (BYTE)((Bin / 10) * 0x10 + (Bin % 10));
}

BYTE CSubCodeGenerator::ToBin(BYTE Hex)
{
    return (BYTE)((Hex / 0x10) * 10 + (Hex % 0x10));
}

void CSubCodeGenerator::CalcCRC(BYTE* Buffer)
{
    if (!Buffer) return;

    WORD crc = 0;

    for (int i = 0; i < 10; i++)
    {
        BYTE index = (BYTE)(Buffer[i] ^ (crc >> 8));
        crc = (WORD)(m_SubcodeCRCTable[index] ^ (crc << 8));
    }

    Buffer[10] = (BYTE)~(crc >> 8);
    Buffer[11] = (BYTE)~(crc);
}
