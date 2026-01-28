#include "stdafx.h"
#include "cdm.h"
#include "MessageDialog.h"
#include "./messagedialog.h"

#ifdef SPTI_DEBUG_LOG
#include <windows.h>

// Safe logging helper for LPCTSTR (works in ANSI or UNICODE builds)
static void LogMessageDialog(LPCTSTR title, LPCTSTR message)
{
    ::OutputDebugString(_T("=============== CMessageDialog::MessageBox ===============\n"));
    ::OutputDebugString(_T("TITLE: "));
    ::OutputDebugString(title ? title : _T("<null>"));
    ::OutputDebugString(_T("\n"));

    ::OutputDebugString(_T("TEXT : "));
    ::OutputDebugString(message ? message : _T("<null>"));
    ::OutputDebugString(_T("\n"));
    ::OutputDebugString(_T("==========================================================\n"));
}
#else
static void LogMessageDialog(LPCTSTR, LPCTSTR) {}
#endif

IMPLEMENT_DYNAMIC(CMessageDialog, CDialog)

CMessageDialog::CMessageDialog(CWnd* pParent /*=NULL*/)
    : CDialog(IDD, pParent)
    , m_Message(_T(""))
    , m_Title(_T(""))
{
}

CMessageDialog::~CMessageDialog()
{
}

void CMessageDialog::DoDataExchange(CDataExchange* pDX)
{
    CDialog::DoDataExchange(pDX);
    DDX_Text(pDX, IDC_MESSAGE, m_Message);
}

BEGIN_MESSAGE_MAP(CMessageDialog, CDialog)
END_MESSAGE_MAP()

void CMessageDialog::MessageBox(LPCTSTR Title, LPCTSTR Message)
{
#ifdef SPTI_DEBUG_LOG
    // STEP B: logs every message dialog shown (including your “lead-in” error)
    LogMessageDialog(Title, Message);
#endif

    m_Title = Title;
    m_Message = Message;
    DoModal();
}

BOOL CMessageDialog::OnInitDialog()
{
    CDialog::OnInitDialog();
    SetWindowText(m_Title);
    UpdateData(FALSE);
    return TRUE; // return TRUE unless you set the focus to a control
}
