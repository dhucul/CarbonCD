#include "stdafx.h"
#include "CDM.h"
#include "WriteProgressDialog.h"
#include "Mmsystem.h"
#include "MessageDialog.h"

#include "Setting.h"
#define STR(i) (theSetting.m_Lang.m_Str[LP_WRITEP + i])

#include <memory> // std::unique_ptr

#include "WriteUiMessages.h" // ✅ shared structs + WM_APP ids

IMPLEMENT_DYNAMIC(CWriteProgressDialog, CDialog)

CWriteProgressDialog::CWriteProgressDialog(CWnd* pParent /*=nullptr*/)
    : CDialog(IDD, pParent)
    , m_Message(_T(""))
    , m_Percent(_T(""))
    , m_NoConfirm(false)
    , m_RawFlag(_T(""))
{
}

CWriteProgressDialog::~CWriteProgressDialog()
{
    // Best-effort shutdown
    m_Thread.StopThread();
}

void CWriteProgressDialog::DoDataExchange(CDataExchange* pDX)
{
    CDialog::DoDataExchange(pDX);
    DDX_Text(pDX, IDC_MESSAGE, m_Message);
    DDX_Control(pDX, IDC_PROGRESS, m_Progress);
    DDX_Text(pDX, IDC_PERCENT, m_Percent);
    DDX_Control(pDX, IDCANCEL, m_CancelButton);
    DDX_Text(pDX, IDC_RAWFLAG, m_RawFlag);
}

BEGIN_MESSAGE_MAP(CWriteProgressDialog, CDialog)
    ON_BN_CLICKED(IDOK, OnBnClickedOk)
    ON_BN_CLICKED(IDCANCEL, OnBnClickedCancel)
    ON_BN_CLICKED(IDC_LOG, OnBnClickedLog)
    ON_COMMAND(ID_WINDOW_CLOSE, OnWindowClose)
    ON_COMMAND(ID_UPDATE_DIALOG, OnUpdateDialog)

    // ✅ worker->UI marshaling
    ON_MESSAGE(WM_APP_WRITE_UI_UPDATE, &CWriteProgressDialog::OnWriteUiUpdate)
    ON_MESSAGE(WM_APP_WRITE_QUERY_YESNO, &CWriteProgressDialog::OnWriteQueryYesNo)

    ON_WM_CLOSE()
END_MESSAGE_MAP()

void CWriteProgressDialog::OnBnClickedOk()
{
    // progress dialog shouldn't close via OK
}

void CWriteProgressDialog::OnBnClickedCancel()
{
    if (m_Thread.m_LogWnd)
        m_Thread.m_LogWnd->AddMessage(LOG_WARNING, MSG(17));

    // atomic stop request
    m_Thread.m_StopFlag.store(true, std::memory_order_release);

    if (::IsWindow(m_CancelButton.GetSafeHwnd()))
        m_CancelButton.EnableWindow(FALSE);
}

BOOL CWriteProgressDialog::OnInitDialog()
{
    CDialog::OnInitDialog();

    SetWindowText(STR(0));
    SetDlgItemText(IDC_LOG, STR(1));
    SetDlgItemText(IDCANCEL, STR(2));

    m_Progress.SetRange(0, 100);

    // ✅ SAFETY: store HWND only (no CWnd* shared with worker thread)
    m_Thread.m_hParentWnd = this->GetSafeHwnd();

    // If your dialog is used by DoModal callers, they often set thread params before DoModal.
    // Starting here is fine if that's your design.
    m_Thread.StartThread();
    return TRUE;
}

void CWriteProgressDialog::WriteDisc(LPCSTR FileName, CCDController* CD, CLogWindow* Log)
{
    m_Thread.m_LogWnd = Log;
    m_Thread.m_CD = CD;
    m_Thread.m_CueFileName = FileName;

    // ✅ SAFETY
    m_Thread.m_hParentWnd = this->GetSafeHwnd();

    DoModal();

    m_Thread.StopThread();
}

void CWriteProgressDialog::Mastering(
    CDirStructure* Dir,
    LPCSTR VolumeLabel,
    CListCtrl* List,
    CCDController* CD,
    CLogWindow* Log)
{
    m_Thread.m_LogWnd = Log;
    m_Thread.m_CD = CD;
    m_Thread.m_List = List;
    m_Thread.m_Dir = Dir;
    m_Thread.m_VolumeLabel = VolumeLabel;

    // ✅ SAFETY
    m_Thread.m_hParentWnd = this->GetSafeHwnd();

    DoModal();

    m_Thread.StopThread();
}

void CWriteProgressDialog::OnBnClickedLog()
{
    if (!m_Thread.m_LogWnd)
        return;

    if (m_Thread.m_LogWnd->IsWindowVisible())
        m_Thread.m_LogWnd->ShowWindow(SW_HIDE);
    else
        m_Thread.m_LogWnd->ShowWindow(SW_SHOW);
}

void CWriteProgressDialog::OnWindowClose()
{
    CMessageDialog dlg;

    // Stop worker before closing dialog
    m_Thread.StopThread();

    if (!m_Thread.m_Success)
    {
        CString cs;
        cs.Format(_T("%s\n%s"), m_Message.GetString(), MSG(18));

        if (!theSetting.m_WavOnFail.IsEmpty())
            PlaySound(theSetting.m_WavOnFail, nullptr, SND_ASYNC | SND_FILENAME);

        dlg.MessageBox(CONF_MSG, cs);
    }
    else if (!m_NoConfirm)
    {
        if (!theSetting.m_WavOnSuccess.IsEmpty())
            PlaySound(theSetting.m_WavOnSuccess, nullptr, SND_ASYNC | SND_FILENAME);

        dlg.MessageBox(CONF_MSG, MSG(19));
    }

    OnCancel();
}

void CWriteProgressDialog::OnUpdateDialog()
{
    // Legacy: okay to keep
    UpdateData(FALSE);
}

// ✅ Apply worker->UI updates on UI thread
LRESULT CWriteProgressDialog::OnWriteUiUpdate(WPARAM, LPARAM lParam)
{
    std::unique_ptr<WriteUiUpdate> up(reinterpret_cast<WriteUiUpdate*>(lParam));
    if (!up)
        return 0;

    if (up->hasMessage)  m_Message = up->message;
    if (up->hasPercent)  m_Percent = up->percent;
    if (up->hasRawFlag)  m_RawFlag = up->rawFlag;

    if (up->hasProgress)
        m_Progress.SetPos(up->progress);

    // LogWindow updates must happen on UI thread
    if (up->hasLog && m_Thread.m_LogWnd)
        m_Thread.m_LogWnd->AddMessage(up->logLevel, up->logText);

    if (up->requestAutoSave && m_Thread.m_LogWnd)
        m_Thread.m_LogWnd->AutoSave();

    UpdateData(FALSE);

    if (up->requestClose)
        PostMessage(WM_COMMAND, ID_WINDOW_CLOSE, 0);

    return 0;
}

// ✅ Worker thread asks UI to show a Yes/No prompt
LRESULT CWriteProgressDialog::OnWriteQueryYesNo(WPARAM, LPARAM lParam)
{
    auto* q = reinterpret_cast<WriteUiQueryYesNo*>(lParam);
    if (!q)
        return IDNO;

    return (LRESULT)::MessageBox(m_hWnd, q->text, q->caption, q->flags);
}

void CWriteProgressDialog::OnClose()
{
    OnWindowClose();
}
