#pragma once
#include "afxcmn.h"
#include "CDController.h"
#include "LogWindow.h"
#include "writethread.h"
#include "Resource.h"
#include "afxwin.h"
#include "DirStructure.h"

// ✅ UI-marshaling messages
static const UINT WM_APP_WRITE_UI_UPDATE = WM_APP + 101;
static const UINT WM_APP_WRITE_QUERY_YESNO = WM_APP + 102;

// ✅ forward declarations for payloads (defined in .cpp)
struct WriteUiUpdate;
struct WriteUiQueryYesNo;

class CWriteProgressDialog : public CDialog
{
    DECLARE_DYNAMIC(CWriteProgressDialog)

public:
    CWriteProgressDialog(CWnd* pParent = nullptr);
    ~CWriteProgressDialog() override;

    enum { IDD = IDD_WRITEPROGRESS };

protected:
    void DoDataExchange(CDataExchange* pDX) override;

    // ✅ required for X close + custom message handlers
    afx_msg void OnClose();
    afx_msg LRESULT OnWriteUiUpdate(WPARAM, LPARAM);
    afx_msg LRESULT OnWriteQueryYesNo(WPARAM, LPARAM);

    DECLARE_MESSAGE_MAP()

public:
    CString m_Message;
    CProgressCtrl m_Progress;
    CString m_Percent;

    afx_msg void OnBnClickedOk();
    afx_msg void OnBnClickedCancel();
    BOOL OnInitDialog() override;
    void WriteDisc(LPCSTR FileName, CCDController* CD, CLogWindow* Log);
    void Mastering(CDirStructure* Dir, LPCSTR VolumeLabel, CListCtrl* List, CCDController* CD, CLogWindow* Log);

    afx_msg void OnBnClickedLog();

protected:
    CWriteThread m_Thread;

public:
    CButton m_CancelButton;
    afx_msg void OnWindowClose();
    bool m_NoConfirm;
    CString m_RawFlag;
    afx_msg void OnUpdateDialog();
};
