#pragma once

#include <windows.h>
#include <afxstr.h>   // CString (MFC)

#pragma pack(push, 1)

// ASPI global definitions
#ifndef SENSE_LEN
#define SENSE_LEN 14
#endif

#ifndef SRB_POSTING
#define SRB_POSTING 0x01
#endif
#ifndef SRB_DIR_IN
#define SRB_DIR_IN  0x08
#endif
#ifndef SRB_DIR_OUT
#define SRB_DIR_OUT 0x10
#endif
#ifndef SRB_EVENT_NOTIFY
#define SRB_EVENT_NOTIFY 0x40
#endif

// SRB_Status
#ifndef SS_PENDING
#define SS_PENDING 0x00
#endif
#ifndef SS_COMP
#define SS_COMP    0x01
#endif
#ifndef SS_ABORTED
#define SS_ABORTED 0x02
#endif
#ifndef SS_ERR
#define SS_ERR     0x04
#endif

// ASPI command
#ifndef SC_HA_INQUIRY
#define SC_HA_INQUIRY   0x00
#endif
#ifndef SC_GET_DEV_TYPE
#define SC_GET_DEV_TYPE 0x01
#endif
#ifndef SC_EXEC_SCSI_CMD
#define SC_EXEC_SCSI_CMD 0x02
#endif

// host adapter inquiry
struct SRB_HAInquiry
{
    BYTE  SRB_Cmd;
    BYTE  SRB_Status;
    BYTE  SRB_Ha;
    BYTE  SRB_Flags;
    DWORD Reserved1;
    BYTE  HA_Count;
    BYTE  HA_SCSI_ID;
    BYTE  HA_ManagerId[16];
    BYTE  HA_Identifier[16];
    BYTE  HA_Unique[16];
    WORD  Reserved2;
};

// get device type
struct SRB_GDEVBlock
{
    BYTE  SRB_Cmd;
    BYTE  SRB_Status;
    BYTE  SRB_Ha;
    BYTE  SRB_Flags;
    DWORD Reserved1;
    BYTE  SRB_Tgt;
    BYTE  SRB_Lun;
    BYTE  SRB_DeviceType;
    BYTE  Reserved2;
};

// execute scsi command
struct SRB_ExecSCSICmd
{
    BYTE  SRB_Cmd;       // SC_EXEC_SCSI_CMD
    BYTE  SRB_Status;
    BYTE  SRB_Ha;
    BYTE  SRB_Flags;
    DWORD Reserved1;
    BYTE  SRB_Tgt;
    BYTE  SRB_Lun;
    WORD  Reserved2;
    DWORD SRB_BufLen;
    BYTE* SRB_BufPointer;
    BYTE  SRB_SenseLen;
    BYTE  SRB_CDBLen;
    BYTE  SRB_HaStat;
    BYTE  SRB_TgtStat;
    VOID* SRB_PostProc;
    BYTE  Reserved3[20];
    BYTE  CDBByte[16];
    BYTE  SenseArea[32 + 2];
};

// aspibuff
struct ASPI32BUFF
{
    PBYTE BufPointer;
    DWORD BufLen;
    DWORD ZeroFill;
    DWORD Reserved;
};

using LPSRB = void*;

// function typedefs
using fGetASPI32SupportInfo = DWORD(*)(void);
using fSendASPI32Command = DWORD(*)(LPSRB);
using fGetASPI32Buffer = BOOL(*)(ASPI32BUFF* ab);
using fFreeASPI32Buffer = BOOL(*)(ASPI32BUFF* pab);
using fTranslateASPI32Address = BOOL(*)(DWORD* path, DWORD* node);
using fGetASPI32DLLVersion = DWORD(*)(void);

#pragma pack(pop)

class CAspi
{
public:
    CAspi();
    virtual ~CAspi();

    // Helper commands that call ExecuteCommand()
    virtual bool  ModeSelect(BYTE* Buffer, DWORD BufLen);
    virtual DWORD ModeSense(BYTE* Buffer, DWORD BufLen, BYTE PCFlag, BYTE PageCode);

    // Driver interface (preserved signatures)
    virtual void  ExecuteCommand(SRB_ExecSCSICmd& cmd);
    virtual DWORD GetVersion(void);
    virtual BOOL  IsActive(void);
    virtual void  Initialize(void);

    // Async API exists historically; in option (2) we keep it but
    // SPTI backend remains synchronous unless your codebase truly uses async.
    virtual void  InitialAsync(void);
    virtual void  FinalizeAsync(void);
    virtual bool  ExecuteCommandAsync(SRB_ExecSCSICmd& cmd);

    // Device selection / enumeration
    virtual int   GetDeviceCount(void);
    virtual void  GetDeviceString(CString& Vendor, CString& Product, CString& Revision, CString& BusAddress);
    virtual void  SetDevice(int DeviceNo);
    virtual int   GetCurrentDevice(void);
};
