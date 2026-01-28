// SubCodeGenerator.cpp
#include "StdAfx.h"
#include "SubCodeGenerator.h"

#include <cstdint>
#include <cstring>  // memset

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

        m_SubcodeCRCTable[i] = (WORD)(crc & 0xFFFF);
    }

    // Defensive defaults
    m_CueSheet = nullptr;
    m_CueEntryCount = 0;
    m_CurrentEntry = 0;
    m_TocCounter = 0;
    m_3Counter = 0;
    m_CurrentLBA = 0;
    m_RelativeLBA = 0;
    m_TocCount = 0;
    m_P_Count = 0;

    std::memset(m_RawToc, 0, sizeof(m_RawToc));
}

CSubCodeGenerator::~CSubCodeGenerator(void) {}

void CSubCodeGenerator::SetCueSheet(const BYTE* MMCCueSheet, int EntryCount)
{
    m_CueSheet = MMCCueSheet;
    m_CueEntryCount = EntryCount;

    // Defensive guard
    if (!m_CueSheet || m_CueEntryCount <= 0)
    {
        m_CurrentEntry = 0;
        m_TocCounter = 0;
        m_3Counter = 0;
        m_CurrentLBA = 0;
        m_RelativeLBA = 0;
        m_TocCount = 0;
        m_P_Count = 0;
        std::memset(m_RawToc, 0, sizeof(m_RawToc));
        return;
    }

    // Keep your original behavior (MSF in BCD -> LBA), with -450000 offset
    m_CurrentLBA =
        (DWORD)(
            ToBin(m_CueSheet[7]) +
            75 * (ToBin(m_CueSheet[6]) + 60 * ToBin(m_CueSheet[5])) -
            450000
            );

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

    // Clamp entry
    if (m_CurrentEntry < 0) m_CurrentEntry = 0;
    if (m_CurrentEntry >= m_CueEntryCount) m_CurrentEntry = m_CueEntryCount - 1;

    const BYTE* Cue = m_CueSheet + m_CurrentEntry * 8;

    // If not lead-out, consider advancing to next entry when we reach its time
    if (Cue[1] != 0xAA)
    {
        if (m_CurrentEntry + 1 < m_CueEntryCount)
        {
            const BYTE* CueNext = m_CueSheet + (m_CurrentEntry + 1) * 8;

            DWORD nextLba =
                (DWORD)(ToBin(CueNext[7]) +
                    75 * (ToBin(CueNext[6]) + 60 * ToBin(CueNext[5])));

            if (nextLba <= m_CurrentLBA && m_CurrentLBA < 0x80000000)
            {
                m_CurrentEntry++;
                if (m_CurrentEntry >= m_CueEntryCount)
                    m_CurrentEntry = m_CueEntryCount - 1;

                Cue = m_CueSheet + m_CurrentEntry * 8;

                // Recompute relative window similarly to your original logic
                const BYTE* CueNext2 = Cue;
                if (m_CurrentEntry + 1 < m_CueEntryCount)
                    CueNext2 = m_CueSheet + (m_CurrentEntry + 1) * 8;

                if (Cue[1] != 0 && Cue[2] == 0)
                {
                    DWORD lbaNext =
                        (DWORD)(ToBin(CueNext2[7]) +
                            75 * (ToBin(CueNext2[6]) + 60 * ToBin(CueNext2[5])));
                    DWORD lbaCur =
                        (DWORD)(ToBin(Cue[7]) +
                            75 * (ToBin(Cue[6]) + 60 * ToBin(Cue[5])));

                    m_RelativeLBA = (int32_t)(lbaNext - lbaCur);
                }
                else
                {
                    m_RelativeLBA = 0;
                }
            }
        }
    }

    Cue = m_CueSheet + m_CurrentEntry * 8;

    std::memset(Buffer, 0, 16);

    BYTE m = 0, s = 0, f = 0;

    if (Cue[1] == 0)
    {
        // Lead-In sub code (TOC)
        if (m_TocCount <= 0)
        {
            // Fallback: just stamp absolute time
            LBAtoMSF(m, s, f, (int32_t)m_CurrentLBA);
            Buffer[3] = ToHex(m);
            Buffer[4] = ToHex(s);
            Buffer[5] = ToHex(f);

            m_CurrentLBA++;
            m_RelativeLBA++;

            CalcCRC(Buffer);
            return m_CurrentLBA - 1;
        }

        if (m_TocCounter < 0) m_TocCounter = 0;
        if (m_TocCounter >= m_TocCount) m_TocCounter = 0;

        BYTE* Toc = m_RawToc + m_TocCounter * 16;

        // Preserve your original nibble-swap behavior
        Buffer[0] = (BYTE)((Toc[1] >> 4) | (Toc[1] << 4));
        Buffer[2] = Toc[3];

        // absolute time
        LBAtoMSF(m, s, f, (int32_t)m_CurrentLBA);
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
            m_TocCounter++;
            if (m_TocCounter >= m_TocCount)
                m_TocCounter = 0;
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
        LBAtoMSF(m, s, f, (int32_t)m_CurrentLBA);
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
        // Preserve your original "INDEX 00 counts backwards" behavior, safely (signed)
        if (Cue[1] != 0xAA && Cue[2] == 0)
        {
            m_CurrentLBA++;
            m_RelativeLBA -= 1;
        }
        else
        {
            m_CurrentLBA++;
            m_RelativeLBA += 1;
        }
    }

    CalcCRC(Buffer);
    return m_CurrentLBA - 1;
}

void CSubCodeGenerator::LBAtoMSF(BYTE& m, BYTE& s, BYTE& f, int32_t lba)
{
    // If relative LBA goes negative (pregap), keep it representable.
    // This implementation uses magnitude only. If your target format requires a
    // specific encoding for negative relative time, implement it here.
    int32_t x = lba;
    if (x < 0)
        x = -x;

    m = (BYTE)(x / (75 * 60));
    s = (BYTE)((x / 75) % 60);
    f = (BYTE)(x % 75);
}

void CSubCodeGenerator::CreateToc(void)
{
    std::memset(m_RawToc, 0, sizeof(m_RawToc));
    m_TocCount = 0;

    if (!m_CueSheet || m_CueEntryCount <= 0)
        return;

    const int tocSlots = (int)(sizeof(m_RawToc) / 16);
    if (tocSlots <= 0)
        return;

    BYTE LastTrack = 0;
    BYTE LastTNO = 0;
    BYTE FirstADR = 0;
    BYTE M = 0, S = 0, F = 0;

    for (int i = 0; i < m_CueEntryCount; i++)
    {
        const BYTE* Cue = m_CueSheet + i * 8;

        if (Cue[1] == 1 && Cue[2] == 1)
            FirstADR = Cue[0];

        if (Cue[2] == 1 && Cue[1] != 0xAA)
        {
            LastTrack = ToBin(Cue[1]);
            LastTNO = Cue[1];
        }

        if (Cue[1] == 0xAA)
        {
            M = Cue[5];
            S = Cue[6];
            F = Cue[7];
        }
    }

    // Ensure room for A0/A1/A2 at indices LastTrack, LastTrack+1, LastTrack+2
    if ((int)LastTrack + 2 >= tocSlots)
    {
        int tmp = tocSlots - 3;
        if (tmp < 0) tmp = 0;
        LastTrack = (BYTE)tmp;
    }

    // A0: First Track
    {
        int idx = (int)LastTrack;
        BYTE* Toc = m_RawToc + 16 * idx;
        std::memset(Toc, 0, 16);
        Toc[0] = 1;
        Toc[1] = (BYTE)(0x10 | (FirstADR >> 4));
        Toc[3] = 0xA0;
        Toc[8] = 1;
    }

    // A1: Last Track
    {
        int idx = (int)LastTrack + 1;
        BYTE* Toc = m_RawToc + 16 * idx;
        std::memset(Toc, 0, 16);
        Toc[0] = 1;
        Toc[1] = (BYTE)(0x10 | (FirstADR >> 4));
        Toc[3] = 0xA1;
        Toc[8] = LastTNO;
    }

    // A2: Lead-Out position
    {
        int idx = (int)LastTrack + 2;
        BYTE* Toc = m_RawToc + 16 * idx;
        std::memset(Toc, 0, 16);
        Toc[0] = 1;
        Toc[1] = (BYTE)(0x10 | (FirstADR >> 4));
        Toc[3] = 0xA2;
        Toc[8] = M;
        Toc[9] = S;
        Toc[10] = F;
    }

    // Per-track entries at index (tno - 1)
    for (int i = 0; i < m_CueEntryCount; i++)
    {
        const BYTE* Cue = m_CueSheet + i * 8;

        if (Cue[2] == 1 && Cue[1] != 0xAA)
        {
            BYTE tno = ToBin(Cue[1]);
            if (tno == 0)
                continue;

            int idx = (int)tno - 1;
            if (idx < 0 || idx >= tocSlots)
                continue;

            BYTE* Toc = m_RawToc + 16 * idx;
            std::memset(Toc, 0, 16);

            Toc[0] = 0x01;
            Toc[1] = (BYTE)(0x10 | (Cue[0] >> 4));
            Toc[2] = 0x00;
            Toc[3] = Cue[1];
            Toc[8] = Cue[5];
            Toc[9] = Cue[6];
            Toc[10] = Cue[7];
        }
    }

    m_TocCount = (int)LastTrack + 3;
    if (m_TocCount > tocSlots)
        m_TocCount = tocSlots;
    if (m_TocCount < 0)
        m_TocCount = 0;
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
