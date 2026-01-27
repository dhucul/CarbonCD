#pragma once

#include "Aspi.h"
#include "PBBuffer.h"

#include <windows.h>
#include <winioctl.h>
#include <ntddscsi.h>

#include <array>
#include <atomic>
#include <cstddef>

// Keep SS_ERR available
#ifndef SS_ERR
#define SS_ERR 0x04
#endif

// Wrapper that includes a sense buffer.
// (Windows provides SCSI_PASS_THROUGH_DIRECT, but not this exact convenience wrapper.)
#pragma pack(push, 1)
struct SPTD_WITH_SENSE
{
    SCSI_PASS_THROUGH_DIRECT sptd;
    ULONG Filler;            // alignment
    UCHAR Sense[32];
};
#pragma pack(pop)

class CSptiDriver : public CAspi
{
public:
    CSptiDriver();
    virtual ~CSptiDriver();

    CSptiDriver(const CSptiDriver&) = delete;
    CSptiDriver& operator=(const CSptiDriver&) = delete;

    // CAspi overrides (same signatures)
    virtual void  ExecuteCommand(SRB_ExecSCSICmd& cmd);
    virtual DWORD GetVersion();
    virtual BOOL  IsActive();

    virtual void  Initialize();
    virtual void  InitialAsync();
    virtual void  FinalizeAsync();
    virtual bool  ExecuteCommandAsync(SRB_ExecSCSICmd& cmd);

    virtual int   GetDeviceCount();
    virtual void  GetDeviceString(CString& Vendor, CString& Product, CString& Revision, CString& BusAddress);
    virtual void  SetDevice(int DeviceNo);
    virtual int   GetCurrentDevice();

public:
    int debug = 0; // preserved

private:
    class UniqueHandle
    {
    public:
        UniqueHandle() : h_(NULL) {}
        explicit UniqueHandle(HANDLE h) : h_(h) {}
        ~UniqueHandle() { reset(); }

        UniqueHandle(const UniqueHandle&) = delete;
        UniqueHandle& operator=(const UniqueHandle&) = delete;

        UniqueHandle(UniqueHandle&& other) noexcept : h_(other.release()) {}
        UniqueHandle& operator=(UniqueHandle&& other) noexcept
        {
            if (this != &other) reset(other.release());
            return *this;
        }

        HANDLE get() const { return h_; }
        operator bool() const { return h_ && h_ != INVALID_HANDLE_VALUE; }

        HANDLE release()
        {
            HANDLE tmp = h_;
            h_ = NULL;
            return tmp;
        }

        void reset(HANDLE nh = NULL)
        {
            if (h_ && h_ != INVALID_HANDLE_VALUE)
                ::CloseHandle(h_);
            h_ = nh;
        }

    private:
        HANDLE h_;
    };

private:
    void open_device_by_letter(char driveLetter);
    static DWORD WINAPI thread_proc(LPVOID self);

private:
    std::array<char, 26> m_DeviceLetters{};
    int m_DeviceCount = 0;
    int m_CurrentDevice = -1;

    CPBBuffer m_Buffer; // preserved

    UniqueHandle m_Device;
    UniqueHandle m_WaitEvent;     // auto-reset
    UniqueHandle m_CommandEvent;  // auto-reset
    UniqueHandle m_Thread;

    DWORD m_ThreadID = 0;
    std::atomic_bool m_ExitFlag{ false };

    SPTD_WITH_SENSE m_Spti{};
    BOOL m_Status = FALSE;
};
