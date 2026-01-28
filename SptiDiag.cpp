// SptiDiag.cpp - drop-in diagnostics for SPTI
#include "StdAfx.h"

#include <windows.h>
#include <cstddef>   // size_t
#include <cstdio>

// ------------------------------
// Sense helpers
// ------------------------------
static bool IsSenseHeaderValid(const BYTE* s, size_t len)
{
    if (!s || len < 4) return false;
    const BYTE rc = (BYTE)(s[0] & 0x7F);
    return (rc == 0x70 || rc == 0x71 || rc == 0x72 || rc == 0x73);
}

static bool DecodeSenseAny(const BYTE* s, size_t len, int& key, int& asc, int& ascq)
{
    key = asc = ascq = -1;
    if (!IsSenseHeaderValid(s, len)) return false;

    const BYTE rc = (BYTE)(s[0] & 0x7F);

    // Fixed format (0x70/0x71): key[2], asc[12], ascq[13]
    if ((rc == 0x70 || rc == 0x71) && len >= 14)
    {
        key = (int)(s[2] & 0x0F);
        asc = (int)s[12];
        ascq = (int)s[13];
        return true;
    }

    // Descriptor format (0x72/0x73): key[1], asc[2], ascq[3]
    if ((rc == 0x72 || rc == 0x73) && len >= 4)
    {
        key = (int)(s[1] & 0x0F);
        asc = (int)s[2];
        ascq = (int)s[3];
        return true;
    }

    return false;
}

static void DumpHex(const BYTE* p, size_t n, char* out, size_t outCap)
{
    if (!out || outCap == 0) return;
    out[0] = '\0';

    if (!p || n == 0) return;

    size_t used = 0;
    for (size_t i = 0; i < n; ++i)
    {
        // Need up to 3 chars per byte after first: " XX"
        if (used + 3 >= outCap) break;

        int written = 0;
        if (i == 0)
            written = _snprintf_s(out + used, outCap - used, _TRUNCATE, "%02X", (unsigned)p[i]);
        else
            written = _snprintf_s(out + used, outCap - used, _TRUNCATE, " %02X", (unsigned)p[i]);

        if (written <= 0) break;
        used += (size_t)written;
    }
}

// ------------------------------
// Public diagnostic function
// ------------------------------
// Call this at the exact point you decide "lead-in failed".
void SptiReportResultA(
    const char* tag,
    BOOL ok,
    DWORD lastError,
    BYTE opcode,
    BYTE scsiStatus,
    const BYTE* sense,
    size_t senseLen)
{
    // show up to 32 sense bytes
    size_t n = senseLen;
    if (n > 32) n = 32;

    char senseHex[256] = {};
    DumpHex(sense, n, senseHex, sizeof(senseHex));

    if (!ok)
    {
        // Transport/OS failure: sense is meaningless.
        char msg[256] = {};
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            NULL, lastError, 0, msg, (DWORD)sizeof(msg), NULL);

        std::printf("[%s] IOCTL FAIL op=%02X GLE=%lu %s\n",
            tag ? tag : "SPTI",
            (unsigned)opcode,
            (unsigned long)lastError,
            msg);

        return;
    }

    // IOCTL succeeded: report SCSI status.
    std::printf("[%s] ok op=%02X ScsiStatus=%02X\n",
        tag ? tag : "SPTI",
        (unsigned)opcode,
        (unsigned)scsiStatus);

    if (scsiStatus == 0x02) // CHECK CONDITION
    {
        int k = -1, a = -1, q = -1;
        if (DecodeSenseAny(sense, senseLen, k, a, q))
        {
            std::printf("[%s] sense key=%02X asc=%02X ascq=%02X raw=[%s]\n",
                tag ? tag : "SPTI",
                (unsigned)k, (unsigned)a, (unsigned)q,
                senseHex);
        }
        else
        {
            std::printf("[%s] sense undecodable raw=[%s]\n",
                tag ? tag : "SPTI",
                senseHex);
        }
    }
    else
    {
        // Not CHECK CONDITION; sense being 00/00/00 is normal.
        std::printf("[%s] (no CHECK CONDITION) sense raw=[%s]\n",
            tag ? tag : "SPTI",
            senseHex);
    }
}
