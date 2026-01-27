#include "StdAfx.h"
#include "cdcontroller.h"

#include "Setting.h"
#include "AspiDriver.h"
#include "SptiDriver.h"

#include <utility> // (optional)

CCDController::CCDController(void)
{
    InitializeAspi();
}

// unique_ptr handles deletion
CCDController::~CCDController(void) = default;

void CCDController::InitializeAspi(void)
{
    if (theSetting.m_UseSPTI)
        m_Aspi = std::make_unique<CSptiDriver>();
    else
        m_Aspi = std::make_unique<CAspiDriver>();

    m_Reader.Initialize(m_Aspi.get());
    m_SubReader.Initialize(m_Aspi.get());
    m_Writer.Initialize(m_Aspi.get());
}

CAspi* CCDController::GetAspiCtrl(void)
{
    return m_Aspi.get();
}

bool CCDController::ReadTOC(void)
{
    return m_Reader.ReadTOCFromSession(m_Toc);
}

TableOfContents* CCDController::GetTOC(void)
{
    return &m_Toc;
}

bool CCDController::GetDriveName(CString& String)
{
    CAspi* aspi = GetAspiCtrl();
    if (!aspi) return false;

    CString Vendor, Product, Revision, Address;
    aspi->GetDeviceString(Vendor, Product, Revision, Address);

    String.Format(_T("(%s) %s %s %s"),
        (LPCTSTR)Address, (LPCTSTR)Vendor, (LPCTSTR)Product, (LPCTSTR)Revision);

    return true;
}

bool CCDController::ReadCDRaw(MSFAddress MSF, BYTE* Buffer)
{
    if (!Buffer) return false;
    return m_Reader.ReadCDRaw(MSF, reinterpret_cast<LPSTR>(Buffer)) != FALSE;
}

bool CCDController::ReadCDAudio(MSFAddress MSF, BYTE* Buffer)
{
    if (!Buffer) return false;

    switch (theSetting.m_ReadAudioMethod)
    {
    case 0:  return m_Reader.ReadCD_D8(MSF, reinterpret_cast<LPSTR>(Buffer)) != FALSE;
    case 1:  return m_Reader.ReadCDDA_LBA(MSF, reinterpret_cast<LPSTR>(Buffer)) != FALSE;
    case 2:  return m_Reader.ReadCDDA(MSF, reinterpret_cast<LPSTR>(Buffer)) != FALSE;
    case 3:  return m_Reader.ReadCD_LBA(MSF, reinterpret_cast<LPSTR>(Buffer)) != FALSE;
    case 4:  return m_Reader.ReadCD(MSF, reinterpret_cast<LPSTR>(Buffer)) != FALSE;
    case 5:  return m_Reader.ReadCDRaw_LBA(MSF, reinterpret_cast<LPSTR>(Buffer)) != FALSE;
    case 6:  return m_Reader.ReadCDRaw(MSF, reinterpret_cast<LPSTR>(Buffer)) != FALSE;
    case 7:  return m_Reader.ReadCD_Read10(MSF, reinterpret_cast<LPSTR>(Buffer)) != FALSE;
    case 8:  return m_Reader.ReadCD_D4(MSF, reinterpret_cast<LPSTR>(Buffer)) != FALSE;
    case 9:  return m_Reader.ReadCD_D4_2(MSF, reinterpret_cast<LPSTR>(Buffer)) != FALSE;
    case 10: return m_Reader.ReadCD_D5(MSF, reinterpret_cast<LPSTR>(Buffer)) != FALSE;
    default: return false;
    }
}

void CCDController::SetSpeed(BYTE ReadSpeed, BYTE WriteSpeed)
{
    m_Reader.SetCDSpeed(ReadSpeed, WriteSpeed);
}

DWORD CCDController::GetErrorStatus(void)
{
    return  (DWORD)m_Reader.m_SRB_Status
        | ((DWORD)m_Reader.m_SK << 24)
        | ((DWORD)m_Reader.m_ASC << 16)
        | ((DWORD)m_Reader.m_ASCQ << 8);
}

bool CCDController::ReadSubQ(MSFAddress msf, BYTE* Buffer)
{
    if (!Buffer) return false;
    return m_Reader.ReadSubQ(msf, Buffer);
}

bool CCDController::SetErrorCorrectMode(bool CorrectFlag)
{
    return m_Reader.SetErrorCorrectMode(CorrectFlag);
}

bool CCDController::ReadRawSub(MSFAddress& MSF, BYTE* Buffer, int Method)
{
    if (!Buffer) return false;

    switch (Method)
    {
    case 0: return m_SubReader.ReadRaw96(MSF, Buffer);
    case 1: return m_SubReader.ReadCD96(MSF, Buffer);
    case 2: return m_SubReader.ReadCDDA96(MSF, Buffer);
    case 3: return m_SubReader.ReadRaw16(MSF, Buffer);
    case 4: return m_SubReader.ReadCD16(MSF, Buffer);
    case 5: return m_SubReader.ReadCDDA16(MSF, Buffer);
    default: return false;
    }
}

bool CCDController::ReadATIP(BYTE* Buffer)
{
    if (!Buffer) return false;
    return m_Reader.ReadATIP(Buffer);
}

int CCDController::ReadCDText(BYTE* Buffer)
{
    if (!Buffer) return 0;
    return m_Reader.ReadCDText(Buffer);
}

// writer's stuff

LPCSTR CCDController::GetWriteError(void) { return m_Writer.GetErrorMessage(); }
bool   CCDController::ParseCueSheetFile(LPCSTR FileName) { return m_Writer.ParseCueSheetFile(FileName); }
bool   CCDController::ParseCueSheet(LPCSTR cue, DWORD ImageSize) { return m_Writer.ParseCueSheet(cue, ImageSize); }
void   CCDController::FinishWriting(void) { m_Writer.FinishWriting(); }
DWORD  CCDController::GetLeadInSize(void) { return m_Writer.GetLeadInSize(); }

bool CCDController::WriteRaw(BYTE* Buffer)
{
    if (!Buffer) return false;
    return m_Writer.WriteRaw(Buffer);
}

bool CCDController::WriteRawLeadIn(void) { return m_Writer.WriteRawLeadIn(); }
bool CCDController::WriteRawGap(void) { return m_Writer.WriteRawGap(); }
bool CCDController::StartWriting(bool ModeMS) { return m_Writer.StartWriting(ModeMS); }
bool CCDController::OPC(void) { return m_Writer.OPC(); }

bool CCDController::SetWritingParams(int WritingMode, bool BurnProof, bool TestMode, int BufferingFrames)
{
    return m_Writer.SetWritingParams(WritingMode, BurnProof, TestMode, BufferingFrames);
}

bool CCDController::LoadTray(bool LoadingMode) { return m_Writer.LoadTray(LoadingMode); }
LPCSTR CCDController::GetImageFileName(void) { return m_Writer.GetImageFileName(); }
DWORD  CCDController::GetImageFrames(void) { return m_Writer.GetImageFrames(); }

void CCDController::GetWriteErrorParams(BYTE& SK, BYTE& ASC, BYTE& ASCQ)
{
    m_Writer.GetErrorParams(SK, ASC, ASCQ);
}

void CCDController::ForceScramble(BYTE* Buffer)
{
    if (!Buffer) return;
    m_Writer.ForceScramble(Buffer);
}

bool CCDController::IsCDR(void) { return m_Writer.IsCDR(); }
void CCDController::AbortWriting(void) { m_Writer.AbortWriting(); }
bool CCDController::EraseMedia(bool FastErase) { return m_Writer.EraseMedia(FastErase); }
int  CCDController::GetWritingMode(void) { return m_Writer.GetWritingMode(); }
int  CCDController::GetBufferSize(void) { return m_Writer.GetBufferSize(); }
bool CCDController::CheckDisc(void) { return m_Writer.CheckDisc(); }

bool CCDController::WriteRaw96(BYTE* Buffer, DWORD lba)
{
    if (!Buffer) return false;
    return m_Writer.WriteRaw96(Buffer, lba);
}

void CCDController::SetReadingBCDMode(bool TransBCD)
{
    m_SubReader.SetBCDMode(TransBCD);
}
