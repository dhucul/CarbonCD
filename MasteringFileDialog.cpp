// MasteringFileDialog.cpp
#include "stdafx.h"
#include "CDM.h"
#include "MasteringFileDialog.h"
#include "IsoCreator.h"
#include "CreateProgressDialog.h"
#include "WriteSettingDialog.h"
#include "WriteProgressDialog.h"
#include "Setting.h"

// Needed for theTheme (your project global/theme wrapper)
#include "ThemeController.h"

// Win32 helpers used in this file
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <cstring>   // std::memset

#define STR(i)  (theSetting.m_Lang.m_Str[LP_MASTERING + (i)])

IMPLEMENT_DYNAMIC(CMasteringFileDialog, CDialog)

namespace
{
    static DWORD CeilDiv(DWORD num, DWORD den)
    {
        return (den == 0) ? 0 : (num + den - 1) / den;
    }

    static DWORD ClampU64ToDword(ULONGLONG v)
    {
        return (v > 0xFFFFFFFFULL) ? 0xFFFFFFFFUL : static_cast<DWORD>(v);
    }

    static ULONGLONG SafeGetFileSizeBytes(LPCTSTR path)
    {
        HANDLE hFile = ::CreateFile(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE)
            return 0;

        LARGE_INTEGER li{};
        const BOOL ok = ::GetFileSizeEx(hFile, &li);
        ::CloseHandle(hFile);

        if (!ok || li.QuadPart < 0)
            return 0;

        return static_cast<ULONGLONG>(li.QuadPart);
    }
}

CMasteringFileDialog::CMasteringFileDialog(CWnd* pParent /*=nullptr*/)
    : CDialog(IDD, pParent)
    , m_Size(_T(""))
{
    // cache pointers into pages (members exist immediately)
    m_TrackList = &m_Page1.m_TrackList;
    m_VolumeLabel = &m_Page2.m_VolumeLabel;
    m_List = &m_Page2.m_List;
    m_Tree = &m_Page2.m_Tree;

    m_LeadOutPos = 0;
    m_ImageSize = 0;
}

CMasteringFileDialog::~CMasteringFileDialog() = default;

void CMasteringFileDialog::DoDataExchange(CDataExchange* pDX)
{
    CDialog::DoDataExchange(pDX);
    DDX_Control(pDX, IDC_TAB, m_Tab);
    DDX_Text(pDX, IDC_IMAGESIZE, m_Size);
    DDX_Control(pDX, IDC_DRIVELIST, m_DriveList);
}

BEGIN_MESSAGE_MAP(CMasteringFileDialog, CDialog)
    ON_COMMAND(ID_EDIT_ADDFOLDER, OnEditAddfolder)
    ON_COMMAND(ID_EDIT_LABEL, OnEditLabel)
    ON_COMMAND(ID_EDIT_DELETEFOLDER, OnEditDeletefolder)
    ON_COMMAND(ID_EDIT_ADDFILE, OnEditAddfile)
    ON_BN_CLICKED(IDC_CREATE_ISO, OnBnClickedCreateIso)
    ON_BN_CLICKED(IDOK, OnBnClickedOk)
    ON_COMMAND(ID_EDIT_ADDAUDIO, OnEditAddaudio)
    ON_COMMAND(ID_EDIT_DELETETRACK, OnEditDeletetrack)
    ON_COMMAND(ID_EDIT_ADDDATA, OnEditAdddata)
    ON_NOTIFY(TCN_SELCHANGE, IDC_TAB, OnTcnSelchangeTab)
    ON_BN_CLICKED(IDC_WRITING, OnBnClickedWriting)
    ON_COMMAND(ID_WINDOW_CLOSE, OnWindowClose)
    ON_COMMAND(ID_EDIT_INSERTFOLDER, OnEditInsertfolder)
    ON_WM_SETFOCUS()
    ON_COMMAND(ID_TRACK_ISO, OnTrackIso)
    ON_BN_CLICKED(IDC_EXPLORER, OnBnClickedExplorer)
    ON_CBN_SELCHANGE(IDC_DRIVELIST, OnCbnSelchangeDrivelist)
    ON_COMMAND(ID_CD_ERASE, OnCdErase)
    ON_COMMAND(ID_CD_ERASE_FAST, OnCdEraseFast)
END_MESSAGE_MAP()

BOOL CMasteringFileDialog::OnInitDialog()
{
    CDialog::OnInitDialog();

    m_Dir.m_RealFileName = _T("image.iso");
    m_Dir.m_ImageFileName = _T("ISO_Image:\\");

    m_Tab.InsertItem(0, STR(3));
    m_Tab.InsertItem(1, STR(4));
    m_Tab.InsertItem(2, STR(19));
    m_Tab.SetCurSel(1);

    m_Size = STR(5) + _T(" 00:00:00");

    SetLanguage();

    // Wire pages
    m_Page1.m_MainDialog = this;
    m_Page2.m_Dir = &m_Dir;
    m_Page2.m_Page1 = &m_Page1;
    m_Page2.m_MainDialog = this;
    m_Page3.m_MainDialog = this;

    m_Page1.Create(IDD_MASTERING_1, this);
    m_Page2.Create(IDD_MASTERING_2, this);
    m_Page3.Create(IDD_MASTERING_3, this);

    // Refresh cached pointers after Create
    m_TrackList = &m_Page1.m_TrackList;
    m_VolumeLabel = &m_Page2.m_VolumeLabel;
    m_List = &m_Page2.m_List;
    m_Tree = &m_Page2.m_Tree;

    // Theme (requires theTheme)
    theTheme.EnableThemeDialogTexture(m_Page1.m_hWnd, ETDT_ENABLETAB);
    theTheme.EnableThemeDialogTexture(m_Page2.m_hWnd, ETDT_ENABLETAB);
    theTheme.EnableThemeDialogTexture(m_Page3.m_hWnd, ETDT_ENABLETAB);

    ChangeTab();
    UpdateDialog(FALSE);
    CalcSize();

    m_Tab.SetWindowPos(&wndBottom, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);

    if (m_CD && m_CD->GetAspiCtrl())
    {
        m_DriveList.InitializeShortVer(m_CD->GetAspiCtrl());
        m_DriveList.SetCurSel(m_CD->GetAspiCtrl()->GetCurrentDevice());
    }

    if (theSetting.m_Mastering_AlwaysOnTop)
    {
        SetWindowPos(&wndTopMost, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    }

    return TRUE;
}

void CMasteringFileDialog::OnEditAddfolder()
{
    if (!m_Page2.IsWindowVisible())
        return;

    CString cs;
    HTREEITEM ht = m_Tree->GetSelectedItem();

    for (int i = 0; i < 100; i++)
    {
        if (i == 0) cs = MSG(83);
        else        cs.Format(MSG(84), i);

        if (m_Page2.AddFolder(cs))
        {
            m_Tree->Expand(ht, TVE_EXPAND);
            break;
        }
    }
}

void CMasteringFileDialog::OnEditLabel()
{
    if (!m_Page2.IsWindowVisible())
        return;

    POSITION pos = m_List->GetFirstSelectedItemPosition();
    if (pos != nullptr)
    {
        const int id = m_List->GetNextSelectedItem(pos);
        m_List->EditLabel(id);
    }
}

void CMasteringFileDialog::OnEditDeletefolder()
{
    if (m_Page2.IsWindowVisible())
        m_Page2.DeleteSelectedItems();
}

void CMasteringFileDialog::OnEditAddfile()
{
    if (!m_Page2.IsWindowVisible())
        return;

    CFileDialog Dlg(TRUE);
    UpdateDialog(TRUE);

    if (Dlg.DoModal() == IDOK)
        m_Page2.AddFile(Dlg.GetPathName());

    UpdateDialog(FALSE);
    CalcSize();
}

void CMasteringFileDialog::OnBnClickedOk()
{
    OnOK();
}

void CMasteringFileDialog::OnEditAddaudio()
{
    if (m_Page1.IsWindowVisible())
        m_Page1.InsertWaveAudioTrack();
}

void CMasteringFileDialog::OnEditDeletetrack()
{
    if (m_Page1.IsWindowVisible())
        m_Page1.DeleteSelectedTracks();
}

void CMasteringFileDialog::OnEditAdddata()
{
    if (!m_Page1.IsWindowVisible())
        return;

    m_Page1.InsertMode1MasteringTrack();
    m_Tab.SetCurSel(1);
    ChangeTab();
}

void CMasteringFileDialog::OnTrackIso()
{
    if (!m_Page1.IsWindowVisible())
        return;

    CFileDialog dlg(TRUE, nullptr, nullptr,
        OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT | OFN_ALLOWMULTISELECT,
        MSG(90));

    TCHAR buf[2048] = { 0 };

    m_Page1.UpdateData(TRUE);

    LPTSTR old = dlg.m_ofn.lpstrFile;
    dlg.m_ofn.lpstrFile = buf;
    dlg.m_ofn.nMaxFile = _countof(buf);

    if (dlg.DoModal() == IDOK)
        m_Page1.InsertIsoTrack(dlg.GetPathName());

    dlg.m_ofn.lpstrFile = old;

    m_Page1.UpdateData(FALSE);
    CalcSize();
}

void CMasteringFileDialog::OnTcnSelchangeTab(NMHDR* /*pNMHDR*/, LRESULT* pResult)
{
    *pResult = 0;
    ChangeTab();
}

void CMasteringFileDialog::OnBnClickedCreateIso()
{
    CFileDialog Dlg(FALSE, _T(".cue"), _T(""),
        OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT,
        MSG(91));

    UpdateDialog(TRUE);

    if (!m_TrackList || m_TrackList->GetItemCount() == 0)
    {
        MessageBox(MSG(92), CONF_MSG);
        UpdateDialog(FALSE);
        return;
    }

    SetWindowPos(&wndNoTopMost, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);

    if (Dlg.DoModal() == IDOK)
    {
        CCreateProgressDialog CreateDlg;
        CreateDlg.CreateIso(Dlg.GetPathName(), *m_VolumeLabel, m_TrackList, &m_Dir, m_LogWnd);
    }

    UpdateDialog(FALSE);

    if (theSetting.m_Mastering_AlwaysOnTop)
        SetWindowPos(&wndTopMost, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
}

void CMasteringFileDialog::OnBnClickedWriting()
{
    CWriteSettingDialog Dlg;
    UpdateDialog(TRUE);

    if (!m_TrackList || m_TrackList->GetItemCount() == 0)
    {
        MessageBox(MSG(93), CONF_MSG);
        UpdateDialog(FALSE);
        return;
    }

    CalcSize();

    if (m_LeadOutPos > 0 && m_ImageSize > (m_LeadOutPos - 150))
    {
        if (MessageBox(STR(5), CONF_MSG, MB_YESNO) == IDNO)
        {
            UpdateDialog(FALSE);
            return;
        }
    }

    SetWindowPos(&wndNoTopMost, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);

    Dlg.m_CD = m_CD;
    Dlg.m_CueSheetName = MSG(94);
    Dlg.m_DisableChangingFile = true;

    if (Dlg.DoModal() == IDOK)
    {
        CWriteProgressDialog WriteDlg;
        WriteDlg.Mastering(&m_Dir, *m_VolumeLabel, m_TrackList, m_CD, m_LogWnd);
    }

    UpdateDialog(FALSE);

    if (theSetting.m_Mastering_AlwaysOnTop)
        SetWindowPos(&wndTopMost, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
}

void CMasteringFileDialog::GetLeadOutPos()
{
    BYTE Buffer[400];
    std::memset(Buffer, 0, sizeof(Buffer));

    if (m_CD && m_CD->ReadATIP(Buffer))
    {
        // ATIP lead-out is in MSF at [12..14] (assumes already binary MSF)
        m_LeadOutPos = ((Buffer[12] * 60) + Buffer[13]) * 75 + Buffer[14];
    }
    else
    {
        m_LeadOutPos = 0;
    }
}

void CMasteringFileDialog::CalcSize()
{
    MSFAddress msf;
    ULONGLONG sizeFrames = 0;

    UpdateData(TRUE);

    const int count = (m_TrackList) ? m_TrackList->GetItemCount() : 0;
    for (int i = 0; i < count; i++)
    {
        if (m_TrackList->GetItemData(i) == 0)
        {
            const CString tt = m_TrackList->GetItemText(i, 1);

            if (tt == _T("Mastering"))
            {
                CIsoCreator iso;
                iso.SetParams(_T(""), theSetting.m_CopyProtectionSize);
                iso.CreateJolietHeader(&m_Dir);
                sizeFrames += iso.GetImageSize();
            }
            else
            {
                const CString path = m_TrackList->GetItemText(i, 2);
                const ULONGLONG fileBytes64 = SafeGetFileSizeBytes(path);
                const DWORD fileBytes = ClampU64ToDword(fileBytes64);

                if (tt == _T("MODE1/2048"))
                {
                    sizeFrames += CeilDiv(fileBytes, 2048);
                }
                else if (tt == _T("MODE1/2352") || tt == _T("MODE2/2352"))
                {
                    sizeFrames += CeilDiv(fileBytes, 2352);
                }
            }
        }
        else
        {
            const CString path = m_TrackList->GetItemText(i, 2);
            const ULONGLONG fileBytes64 = SafeGetFileSizeBytes(path);
            const DWORD fileBytes = ClampU64ToDword(fileBytes64);

            sizeFrames += CeilDiv(fileBytes, 2352);
        }
    }

    const DWORD sizeFrames32 = ClampU64ToDword(sizeFrames);
    msf = sizeFrames32;
    m_ImageSize = sizeFrames32;

    GetLeadOutPos();

    if (m_LeadOutPos == 0)
    {
        m_Size.Format(_T("%02d:%02d:%02d / **:**:**"),
            msf.Minute, msf.Second, msf.Frame);
    }
    else
    {
        MSFAddress lo;
        lo = m_LeadOutPos - 150;
        m_Size.Format(_T("%02d:%02d:%02d / %02d:%02d:%02d"),
            msf.Minute, msf.Second, msf.Frame,
            lo.Minute, lo.Second, lo.Frame);
    }

    UpdateData(FALSE);
}

void CMasteringFileDialog::SetLanguage()
{
    DWORD MenuString[][2] =
    {
        {IDCANCEL, 4},
        {ID_CREATE_IMAGE, 5},
        {ID_CREATE_CD, 6},
        {ID_EDIT_ADDDATA, 7},
        {ID_EDIT_ADDAUDIO, 8},
        {ID_EDIT_DELETETRACK, 9},
        {ID_EDIT_ADDFOLDER, 10},
        {ID_EDIT_ADDFILE, 11},
        {ID_EDIT_DELETEFOLDER, 12},
        {ID_EDIT_LABEL, 13},
        {ID_EDIT_INSERTFOLDER, 14},
        {ID_TRACK_ISO, 15},
        {IDC_EXPLORER, 16},
    };

    DWORD CtrlString[][2] =
    {
        {IDC_CREATE_ISO, 6},
        {IDC_WRITING, 7},
        {IDCANCEL, 8},
        {IDC_SELECTDRIVE, 20},
        {IDC_EXPLORER, 24},
    };

    CMenu* menu = GetMenu();
    if (menu)
    {
        for (int i = 0; i < 4; i++)
        {
            menu->ModifyMenu(i, MF_BYPOSITION | MF_STRING, 0,
                theSetting.m_Lang.m_Str[LP_MASTERINGMENU + i]);
        }

        for (int i = 0; i < 13; i++)
        {
            menu->ModifyMenu(MenuString[i][0], MF_BYCOMMAND | MF_STRING, MenuString[i][0],
                theSetting.m_Lang.m_Str[LP_MASTERINGMENU + MenuString[i][1]]);
        }

        menu->ModifyMenu(ID_CD_ERASE, MF_BYCOMMAND | MF_STRING, ID_CD_ERASE,
            theSetting.m_Lang.m_Str[LP_MAINMENU + 9]);
        menu->ModifyMenu(ID_CD_ERASE_FAST, MF_BYCOMMAND | MF_STRING, ID_CD_ERASE_FAST,
            theSetting.m_Lang.m_Str[LP_MAINMENU + 10]);
    }

    SetWindowText(STR(0));

    for (int i = 0; i < 5; i++)
        SetDlgItemText(CtrlString[i][0], STR(CtrlString[i][1]));

    // NOTE: ID_CD_ERASE / ID_CD_ERASE_FAST are menu command IDs, not dialog control IDs.
    // If you intended to change menu text, ModifyMenu above is correct.
}

void CMasteringFileDialog::OnWindowClose()
{
    OnOK();
}

void CMasteringFileDialog::OnEditInsertfolder()
{
    if (!m_Page2.IsWindowVisible())
        return;

    UpdateDialog(TRUE);

    BROWSEINFO bi{};
    bi.hwndOwner = m_hWnd;

    TCHAR pathList[MAX_PATH + 1] = { 0 };
    TCHAR pathName[MAX_PATH] = { 0 };

    bi.pszDisplayName = pathList;
    bi.lpszTitle = STR(18);
    bi.ulFlags = BIF_RETURNONLYFSDIRS;

    LPMALLOC pMalloc = nullptr;
    LPITEMIDLIST pidl = nullptr;

    if (SUCCEEDED(SHGetMalloc(&pMalloc)) && pMalloc)
    {
        pidl = SHBrowseForFolder(&bi);
        if (pidl)
        {
            SHGetPathFromIDList(pidl, pathName);
            pMalloc->Free(pidl);
        }
        pMalloc->Release();
    }

    if (!pidl)
    {
        UpdateDialog(FALSE);
        return; // cancelled/failed
    }

    m_Page2.AddFileRec(pathName);

    UpdateDialog(FALSE);
    CalcSize();
}

void CMasteringFileDialog::ChangeTab()
{
    m_Page1.ShowWindow(SW_HIDE);
    m_Page2.ShowWindow(SW_HIDE);
    m_Page3.ShowWindow(SW_HIDE);

    const int sel = m_Tab.GetCurSel();
    if (sel == 0) m_Page1.ShowWindow(SW_SHOW);
    else if (sel == 1) m_Page2.ShowWindow(SW_SHOW);
    else if (sel == 2) m_Page3.ShowWindow(SW_SHOW);
}

void CMasteringFileDialog::UpdateDialog(bool bSaveAndValidate)
{
    m_Page1.UpdateData(bSaveAndValidate);
    m_Page2.UpdateData(bSaveAndValidate);
    m_Page3.UpdateData(bSaveAndValidate);
    UpdateData(bSaveAndValidate);
}

void CMasteringFileDialog::OnSetFocus(CWnd* pOldWnd)
{
    CDialog::OnSetFocus(pOldWnd);

    const int sel = m_Tab.GetCurSel();
    if (sel == 0) m_Page1.SetFocus();
    else if (sel == 1) m_Page2.SetFocus();
    else if (sel == 2) m_Page3.SetFocus();
}

void CMasteringFileDialog::OnBnClickedExplorer()
{
    ::ShellExecute(m_hWnd, _T("explore"), nullptr, nullptr, nullptr, SW_SHOWNORMAL);
}

void CMasteringFileDialog::OnCbnSelchangeDrivelist()
{
    if (!m_CD || !m_CD->GetAspiCtrl())
        return;

    const int index = m_DriveList.GetCurSel();
    if (index == CB_ERR)
        return;

    m_CD->GetAspiCtrl()->SetDevice(index);
    GetLeadOutPos();
    CalcSize();
}

void CMasteringFileDialog::OnCancel()
{
    CDialog::OnCancel();
}

void CMasteringFileDialog::OnOK()
{
    // original intentionally did not close;
    // if you want OK to close, replace with: CDialog::OnOK();
}

void CMasteringFileDialog::OnCdErase()
{
    GetParent()->SendMessage(WM_COMMAND, ID_CD_ERASE, 0);
}

void CMasteringFileDialog::OnCdEraseFast()
{
    GetParent()->SendMessage(WM_COMMAND, ID_CD_ERASE_FAST, 0);
}
