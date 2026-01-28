// SubCodeGenerator.h
#pragma once

// Prevent windows.h from defining min/max macros that break std::min/std::max
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cstdint>     // int32_t
#include <windows.h>   // BYTE, WORD, DWORD

class CSubCodeGenerator
{
public:
    CSubCodeGenerator(void);
    ~CSubCodeGenerator(void);

    void SetCueSheet(const BYTE* MMCCueSheet, int EntryCount);
    DWORD GenerateSub16(BYTE* Buffer);

    void CreateToc(void);
    void LBAtoMSF(BYTE& m, BYTE& s, BYTE& f, int32_t lba);

    BYTE ToHex(BYTE Bin);
    BYTE ToBin(BYTE Hex);
    void CalcCRC(BYTE* Buffer);

protected:
    const BYTE* m_CueSheet;
    int         m_CueEntryCount;
    int         m_CurrentEntry;

    int         m_TocCounter;
    int         m_3Counter;

    DWORD       m_CurrentLBA;      // absolute LBA
    int32_t     m_RelativeLBA;     // signed relative LBA (pregap safe)

    BYTE        m_RawToc[100 * 16];
    int         m_TocCount;

    int         m_P_Count;

    WORD        m_SubcodeCRCTable[256];
};
