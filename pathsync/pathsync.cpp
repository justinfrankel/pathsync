/*
    PathSync - pathsync.cpp
    Copyright (C) 2004-2005 Cockos Incorporated and others

    Other contributors:
       Alan Davies
       Francis Gastellu
    
    And now using filename matching from the GNU C library!

    PathSync is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    PathSync is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with PathSync; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/


#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <stdio.h>
#include <search.h>
#include <stdlib.h>
#include <time.h>

#include "resource.h"

#include "../WDL/ptrlist.h"
#include "../WDL/string.h"
#include "../WDL/dirscan.h"

#include "../WDL/wingui/wndsize.h"
#include "fnmatch.h"

#define PATHSYNC_VER "v0.31"

HINSTANCE g_hInstance;

#define ACTION_RECV "Remote->Local"
#define ACTION_SEND "Local->Remote"
#define ACTION_NONE "No Action"
#define REMOTE_ONLY_STR "Remote Only"
#define LOCAL_ONLY_STR "Local Only"

class dirItem {

public:
  dirItem() { refcnt=0; }
  ~dirItem() { }

  WDL_String relativeFileName;
  ULARGE_INTEGER fileSize;
  FILETIME lastWriteTime;

  int refcnt;

};


char *g_syncactions[]=
{
  "Bidirectional (default)",
  ACTION_SEND " (do not delete missing files)",
  ACTION_RECV " (do not delete missing files)",
  ACTION_SEND,
  ACTION_RECV,
};

WDL_PtrList<WDL_String> m_dirscanlist[2];
WDL_DirScan m_curscanner[2];
WDL_String m_curscanner_relpath[2],m_curscanner_basepath[2];

WDL_PtrList<dirItem> m_files[2];
WDL_PtrList<dirItem> m_listview_recs;

WDL_PtrList<WDL_String> m_include_files;

int g_ignflags,g_defbeh; // used only temporarily
int m_comparing; // second and third bits mean done for each side
int m_comparing_pos,m_comparing_pos2;
HWND m_listview;
char m_inifile[2048];
char m_lastsettingsfile[2048];
char g_loadsettingsfile[2048];
bool g_autorun = false;
bool g_systray = false;
bool g_intray = false;
HWND g_copydlg = NULL;
HWND g_dlg = NULL;
int g_lasttraypercent = -1;
int g_throttle,g_throttlespd=1024;
DWORD g_throttle_sttime;
__int64 g_throttle_bytes;

const int endislist[]={IDC_STATS,IDC_PATH1,IDC_PATH2,IDC_BROWSE1,IDC_BROWSE2,IDC_IGNORE_SIZE,IDC_IGNORE_DATE,IDC_IGNORE_MISSLOCAL,IDC_IGNORE_MISSREMOTE,IDC_DEFBEHAVIOR,IDC_LOG,IDC_LOGPATH,IDC_LOGBROWSE, IDC_LOCAL_LABEL, IDC_REMOTE_LABEL, IDC_IGNORE_LABEL, IDC_INCLUDE_LABEL, IDC_INCLUDE_FILES, IDC_MASKHELP };


#define CLEARPTRLIST(xxx) { int x; for (x = 0; x < xxx.GetSize(); x ++) { delete xxx.Get(x); } xxx.Empty(); }

int filenameCompareFunction(dirItem **a, dirItem **b)
{
  return stricmp((*a)->relativeFileName.Get(),(*b)->relativeFileName.Get());
}

void clearFileLists(HWND hwndDlg)
{
  CLEARPTRLIST(m_dirscanlist[0])
  CLEARPTRLIST(m_dirscanlist[1])
  CLEARPTRLIST(m_files[0])
  CLEARPTRLIST(m_files[1])

  // dont clear m_listview_recs[], cause they are just references
  ListView_DeleteAllItems(m_listview);
  m_listview_recs.Empty();
}
BOOL WINAPI copyFilesProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam);


void format_size_string(__int64 size, char *str)
{
  if (size < 1024) sprintf(str,"%uB",(int)size);
  else if (size < 1048576) sprintf(str,"%.2lfKB",(double) size / 1024.0);
  else if (size < 1073741824) sprintf(str,"%.2lfMB",(double) size / 1048576.0);
  else if (size < 1099511627776i64) sprintf(str,"%.2lfGB",(double) size / 1073741824.0);
  else sprintf(str,"%.2lfTB",(double) size / 1099511627776.0);
}

FILE * g_log = 0;

void RestartLogging(HWND hwndDlg)
{
    // Always close and re-open, filename may have changed,
    // logging may have been disabled in UI.
    if (g_log)
    {
        fclose(g_log);
        g_log = 0;
    }

    if (IsDlgButtonChecked(hwndDlg, IDC_LOG))
    {
        char name[1024] = "";
        GetDlgItemText(hwndDlg, IDC_LOGPATH, name, sizeof name);
        g_log=fopen(name, "at");

        if (!g_log)
        {
            char message[2048];
            sprintf(message, "Couldn't open logfile %s", name);
            MessageBox(hwndDlg, message, "Error", 0);
        }
    }
}

void LogMessage(const char * pStr)
{
    if (g_log)
    {
        char timestr[100] = "";
        time_t curtime = time(0);
        strftime(timestr, sizeof timestr - 1, "%Y-%m-%d %H:%M:%S ", localtime(&curtime)); 
        fprintf(g_log, timestr);
        fprintf(g_log, pStr);
        fprintf(g_log, "\n");
        fflush(g_log);
    }
}

__int64 m_total_copy_size = 0;

void calcStats(HWND hwndDlg)
{
  ULARGE_INTEGER totalbytescopy;
  int totalfilesdelete=0,totalfilescopy=0;
  totalbytescopy.QuadPart=0;
  int x,l=ListView_GetItemCount(m_listview);
  for (x = 0; x < l; x ++)
  {
    char action[128];
    LVITEM lvi={LVIF_PARAM|LVIF_TEXT,x,2};
    lvi.pszText=action;
    lvi.cchTextMax=sizeof(action);
    ListView_GetItem(m_listview,&lvi);

    int x=lvi.lParam;
    dirItem **its=m_listview_recs.GetList()+x;
    if (strcmp(action,ACTION_NONE))
    {
      int isSend=!strcmp(action,ACTION_SEND);

      if (its[!isSend])
      {
        totalfilescopy++;
        totalbytescopy.QuadPart += its[!isSend]->fileSize.QuadPart;
      }
      else // delete
      {
        totalfilesdelete++;
      }     
      // calculate loc/rem here
    }
  }
  char buf[1024];
  strcpy(buf,"Synchronizing will ");
  if (totalfilescopy)
  {
    char tmp[128];
    format_size_string(totalbytescopy.QuadPart,tmp);
    sprintf(buf+strlen(buf),"copy %s in %d file%s",tmp,totalfilescopy,totalfilescopy==1?"":"s");
  }
  if (totalfilesdelete)
  {
    if (totalfilescopy) strcat(buf,", and ");
    sprintf(buf+strlen(buf),"delete %d file%s",totalfilesdelete,totalfilesdelete==1?"":"s");
  }
  
  if (!totalfilesdelete && !totalfilescopy)
  {
    strcat(buf,"not perform any actions");
    EnableWindow(GetDlgItem(hwndDlg,IDC_GO),0);
  }
  else EnableWindow(GetDlgItem(hwndDlg,IDC_GO),1);

  LogMessage(buf);

  strcat(buf," (select and right click items to change their actions)");
  SetDlgItemText(hwndDlg,IDC_STATS,buf);
  m_total_copy_size=totalbytescopy.QuadPart;
}

void set_current_settings_file(HWND hwndDlg, char *fn)
{
  lstrcpyn(m_lastsettingsfile,fn,sizeof(m_lastsettingsfile));

  char buf[4096];
  char *p=fn;
  while (*p) p++;
  while (p >= fn && *p != '\\' && *p != '/') p--;
  sprintf(buf,"PathSync " PATHSYNC_VER " - Analysis - %s",p+1);
  SetWindowText(hwndDlg,buf);
}

#define WM_SYSTRAY              WM_USER + 0x200
#define WM_COPYDIALOGEND        WM_USER + 0x201
#define CMD_EXITPATHSYNC        30000
#define CMD_SHOWWINDOWFROMTRAY  30001
#define CMD_CANCELCOPY          30002
#define CMD_CANCELANALYSIS      30003

BOOL systray_add(HWND hwnd, UINT uID, HICON hIcon, LPSTR lpszTip)
{
  NOTIFYICONDATA tnid;
  tnid.cbSize = sizeof(NOTIFYICONDATA);
  tnid.hWnd = hwnd;
  tnid.uID = uID;
  tnid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
  tnid.uCallbackMessage = WM_SYSTRAY;
  tnid.hIcon = hIcon;
  lstrcpyn(tnid.szTip,lpszTip,sizeof(tnid.szTip)-1);
  return (Shell_NotifyIcon(NIM_ADD, &tnid));
}

BOOL systray_del(HWND hwnd, UINT uID) {
  NOTIFYICONDATA tnid;
  tnid.cbSize = sizeof(NOTIFYICONDATA);
  tnid.hWnd = hwnd;
  tnid.uID = uID;
  return(Shell_NotifyIcon(NIM_DELETE, &tnid));
}

BOOL systray_mod(HWND hwnd, UINT uID, LPSTR lpszTip) {
  NOTIFYICONDATA tnid;
  tnid.cbSize = sizeof(NOTIFYICONDATA);
  tnid.hWnd = hwnd;
  tnid.uID = uID;
  tnid.uFlags = NIF_TIP;
  strncpy(tnid.szTip,lpszTip,sizeof(tnid.szTip)-1);
  return (Shell_NotifyIcon(NIM_MODIFY, &tnid));
}

void show_window_from_tray() 
{
  HWND wnd;
  if (g_copydlg) wnd = g_copydlg;
  else wnd = g_dlg;
  if (IsIconic(wnd)) ShowWindow(wnd, SW_RESTORE);
  else ShowWindow(wnd, SW_NORMAL);
  SetForegroundWindow(wnd);
  g_intray = false;
}

void free_pattern_list(WDL_PtrList<WDL_String> *list) 
{
  while (list->GetSize() > 0) 
  { 
    int idx = list->GetSize()-1; 
    delete list->Get(idx); 
    list->Delete(idx);
  }
}

void parse_pattern_list(HWND hwndDlg, char *str, WDL_PtrList<WDL_String> *list) 
{
  char pattern[2048]="";
  char *p = str;
  char *d = pattern;
  while (*p) 
  {
    if (*p == ';') 
    {
      *d = 0;
      list->Add(new WDL_String(pattern));
      *pattern = 0;
      d = pattern,
      p++;
      continue;
    }
    *d++ = *p++;
  }
  if (*pattern) 
  { 
    *d = 0; 
    list->Add(new WDL_String(pattern)); 
  }
}

int test_file_pattern(char *file, int is_dir)
{
  int s=m_include_files.GetSize();
  if (!s) return 1;

  for (int i=0;i<s;i++)
  {
    char *p=m_include_files.Get(i)->Get();
    int isnot=0;
    if (*p == '!') isnot++,p++;

    if (is_dir) // we do not want to exclude anything by this list, rather just 
                // detect things that might be valid. 
    {
      if (*p == '*' && !isnot) return 1; // detect *.bla wildcards

      int l=strlen(file);
      while (l>0 && file[l-1]=='\\') l--;
      if (!strnicmp(p,file,l)) return 1; // if partial match
    }

    if (fnmatch(p, file, 0) == 0) return !isnot;
  }
  return 0;
}

void stopAnalyzeAndClearList(HWND hwndDlg)
{
  if (m_comparing)
  {
    KillTimer(hwndDlg,32);
    SetDlgItemText(hwndDlg,IDC_ANALYZE,"Analyze");
    SetDlgItemText(hwndDlg,IDC_STATUS,"Status: Stopped");
    m_comparing=0;
    int x;
    for (x = 0; x < sizeof(endislist)/sizeof(endislist[0]); x ++)
      EnableWindow(GetDlgItem(hwndDlg,endislist[x]),1);
    free_pattern_list(&m_include_files);
    systray_mod(hwndDlg, 0, "PathSync");
    g_lasttraypercent = -1;
  }
  clearFileLists(hwndDlg);
  SetDlgItemText(hwndDlg,IDC_STATS,"");
  SetDlgItemText(hwndDlg,IDC_STATUS,"");
  EnableWindow(GetDlgItem(hwndDlg,IDC_GO),0);
}

int load_settings(HWND hwndDlg, char *sec, char *fn) // return version
{
  char path[2048];
  GetPrivateProfileString(sec,"path1","",path,sizeof(path),fn);
  SetDlgItemText(hwndDlg,IDC_PATH1,path);
  GetPrivateProfileString(sec,"path2","",path,sizeof(path),fn);
  SetDlgItemText(hwndDlg,IDC_PATH2,path);
  int ignflags=GetPrivateProfileInt(sec,"ignflags",0,fn);
  CheckDlgButton(hwndDlg,IDC_IGNORE_SIZE,(ignflags&1)?BST_CHECKED:BST_UNCHECKED);
  CheckDlgButton(hwndDlg,IDC_IGNORE_DATE,(ignflags&2)?BST_CHECKED:BST_UNCHECKED);
  CheckDlgButton(hwndDlg,IDC_IGNORE_MISSLOCAL,(ignflags&4)?BST_CHECKED:BST_UNCHECKED);
  CheckDlgButton(hwndDlg,IDC_IGNORE_MISSREMOTE,(ignflags&8)?BST_CHECKED:BST_UNCHECKED);
  SendDlgItemMessage(hwndDlg,IDC_DEFBEHAVIOR,CB_SETCURSEL,(WPARAM)GetPrivateProfileInt(sec,"defbeh",0,fn),0);
  GetPrivateProfileString(sec,"logpath","",path,sizeof(path),fn);

  if (strlen(path) && path[0] != '!')
  {
    CheckDlgButton(hwndDlg,IDC_LOG, 1);
  }
  else
  {
    CheckDlgButton(hwndDlg,IDC_LOG, 0);
  }
  SetDlgItemText(hwndDlg,IDC_LOGPATH, path[0] == '!' ? path+1 : path);

  GetPrivateProfileString(sec,"include","",path,sizeof(path),fn);
  SetDlgItemText(hwndDlg,IDC_INCLUDE_FILES,path);

  g_throttle=GetPrivateProfileInt(sec,"throttle",0,fn);
  g_throttlespd=GetPrivateProfileInt(sec,"throttlespd",1024,fn);

  return GetPrivateProfileInt(sec,"pssversion",0,fn);
}


void save_settings(HWND hwndDlg, char *sec, char *fn)
{
  char path[2048];
  if (strcmp(sec,"config")) WritePrivateProfileString(sec,"pssversion","1",fn);

  GetDlgItemText(hwndDlg,IDC_PATH1,path,sizeof(path));
  WritePrivateProfileString(sec,"path1",path,fn);
  GetDlgItemText(hwndDlg,IDC_PATH2,path,sizeof(path));
  WritePrivateProfileString(sec,"path2",path,fn);
  int ignflags=0;
  if (IsDlgButtonChecked(hwndDlg,IDC_IGNORE_SIZE)) ignflags |= 1;
  if (IsDlgButtonChecked(hwndDlg,IDC_IGNORE_DATE)) ignflags |= 2;
  if (IsDlgButtonChecked(hwndDlg,IDC_IGNORE_MISSLOCAL)) ignflags |= 4;
  if (IsDlgButtonChecked(hwndDlg,IDC_IGNORE_MISSREMOTE)) ignflags |= 8;
  wsprintf(path,"%d",ignflags);
  WritePrivateProfileString(sec,"ignflags",path,fn);       
  wsprintf(path,"%d",SendDlgItemMessage(hwndDlg,IDC_DEFBEHAVIOR,CB_GETCURSEL,0,0));
  WritePrivateProfileString(sec,"defbeh",path,fn);

  if (IsWindowEnabled(GetDlgItem(hwndDlg, IDC_LOG)))
  {
    GetDlgItemText(hwndDlg, IDC_LOGPATH, path, sizeof(path));
    WritePrivateProfileString(sec, "logpath", path, fn);
  }
  else
  {
    path[0]='!';
    GetDlgItemText(hwndDlg, IDC_LOGPATH, path+1, sizeof(path)-1);
    WritePrivateProfileString(sec, "logpath", path, fn);
  }
  GetDlgItemText(hwndDlg,IDC_INCLUDE_FILES,path,sizeof(path));
  WritePrivateProfileString(sec,"include",path,fn);

  wsprintf(path,"%d",g_throttlespd);
  WritePrivateProfileString(sec,"throttlespd", path,fn);
  WritePrivateProfileString(sec,"throttle", g_throttle?"1":"0",fn);

}


void EnableOrDisableLoggingControls(HWND hwndDlg)
{
    if (IsDlgButtonChecked(hwndDlg, IDC_LOG))
    {
        EnableWindow(GetDlgItem(hwndDlg, IDC_LOGPATH),1);
        EnableWindow(GetDlgItem(hwndDlg, IDC_LOGBROWSE),1);
    }
    else
    {
        EnableWindow(GetDlgItem(hwndDlg, IDC_LOGPATH), 0);
        EnableWindow(GetDlgItem(hwndDlg, IDC_LOGBROWSE), 0);
    }
}

void cancel_analysis(HWND dlg)
{
  if (m_comparing)
  {
    KillTimer(dlg,32);
    SetDlgItemText(dlg,IDC_ANALYZE,"Analyze");
    SetDlgItemText(dlg,IDC_STATUS,"Status: Stopped");
    m_comparing=0;
    int x;
    for (x = 0; x < sizeof(endislist)/sizeof(endislist[0]); x ++)
      EnableWindow(GetDlgItem(dlg,endislist[x]),1);
    free_pattern_list(&m_include_files);
    systray_mod(dlg, 0, "PathSync");
    g_lasttraypercent = -1;
  }
  if (g_autorun) PostQuitMessage(1);
}

BOOL WINAPI mainDlgProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
  static WDL_WndSizer resizer;
  switch (uMsg)
  {
    case WM_INITDIALOG:
      {
        resizer.init(hwndDlg);
        resizer.init_item(IDC_PATH1, 0, 0, 0.5, 0);
        resizer.init_item(IDC_BROWSE1, 0.5, 0, 0.5, 0);
        resizer.init_item(IDC_REMOTE_LABEL, 0.5, 0, 0.5, 0);
        resizer.init_item(IDC_PATH2, 0.5, 0, 1.0, 0);
        resizer.init_item(IDC_BROWSE2, 1.0, 0, 1.0, 0);
        resizer.init_item(IDC_ANALYZE, 1.0, 0, 1.0, 0);
        resizer.init_item(IDC_STATUS, 0, 0, 1.0, 0);
        resizer.init_item(IDC_LIST1, 0, 0, 1.0, 1.0);
        resizer.init_item(IDC_STATS, 0, 1.0, 1.0, 1.0);
        resizer.init_item(IDC_GO, 1.0, 1.0, 1.0, 1.0);
        resizer.init_item(IDC_DEFBEHAVIOR,0.0,0.0,1.0,0.0);
        resizer.init_item(IDC_INCLUDE_FILES, 0.0, 0, 1, 0);
        resizer.init_item(IDC_MASKHELP,1,0,1,0);

        SetWindowText(hwndDlg,"PathSync " PATHSYNC_VER " - Analysis");
      
        HICON icon = LoadIcon(g_hInstance,MAKEINTRESOURCE(IDI_ICON1));
        SetClassLong(hwndDlg,GCL_HICON,(long)icon);
  		  if (g_systray) systray_add(hwndDlg, 0, icon, "PathSync");
        m_listview=GetDlgItem(hwndDlg,IDC_LIST1);
        {
          LVCOLUMN lvc={LVCF_TEXT|LVCF_WIDTH,0,300,"Filename"};
          ListView_InsertColumn(m_listview,0,&lvc);
          lvc.pszText="Status";
          lvc.cx=200;
          ListView_InsertColumn(m_listview,1,&lvc);
          lvc.pszText="Action";
          lvc.cx=100;
          ListView_InsertColumn(m_listview,2,&lvc);
          int x;
          for (x = 0; x < sizeof(g_syncactions) / sizeof(g_syncactions[0]); x ++)
          {
            SendDlgItemMessage(hwndDlg,IDC_DEFBEHAVIOR,CB_ADDSTRING,0,(LPARAM)g_syncactions[x]);
          }

        }

        SetDlgItemText(hwndDlg, IDC_STATS, "");
        
        if (g_loadsettingsfile[0] && load_settings(hwndDlg,"pathsync settings",g_loadsettingsfile)>0)
        {
          set_current_settings_file(hwndDlg,g_loadsettingsfile);
        }
        else 
        {
          if (g_loadsettingsfile[0])
          {
            if (g_autorun)
            {
              PostQuitMessage(1);
            }
            else
              MessageBox(hwndDlg,"Error loading PSS file, loading last config","PathSync Warning",MB_OK);
          }
          load_settings(hwndDlg,"config",m_inifile);
        }
        g_loadsettingsfile[0]=0;
 
        EnableOrDisableLoggingControls(hwndDlg);
    
        if (g_autorun)
        {
          if (IsWindowEnabled(GetDlgItem(hwndDlg, IDC_ANALYZE)))
          {
            PostMessage(hwndDlg, WM_COMMAND, MAKEWPARAM(IDC_ANALYZE, 1), (LPARAM)GetDlgItem(hwndDlg, IDC_ANALYZE));
          }
          else if (g_autorun)
          {
            PostQuitMessage(1);
          }
        }
      }
        
    return 0;
    case WM_GETMINMAXINFO:
      {
        LPMINMAXINFO p=(LPMINMAXINFO)lParam;
        p->ptMinTrackSize.x = 444;
        p->ptMinTrackSize.y = 238;
      }
    return 0;
    case WM_SIZE:
      if (wParam != SIZE_MINIMIZED) {
        resizer.onResize();
      }
      if (g_systray)
      {
			  if (wParam == SIZE_MINIMIZED)
			  {
          g_intray = true;
				  ShowWindow(hwndDlg,SW_HIDE);
			  }
			  else if (wParam == SIZE_RESTORED)
			  {
          g_intray = false;
				  ShowWindow(hwndDlg,SW_SHOW);
			  }
      }
    return 0;

    case WM_SYSCOMMAND:
      switch (wParam)
      {
        case SC_CLOSE: PostQuitMessage(0); break;
      }
    return 0;
    case WM_DESTROY:
      systray_del(hwndDlg, 0);
      save_settings(hwndDlg,"config",m_inifile);
    return 0;
    case WM_SYSTRAY: 
			switch (LOWORD(lParam))
			{
				case WM_LBUTTONDOWN:
          show_window_from_tray();
				return 0;
        case WM_RBUTTONUP:
          {
            SetForegroundWindow(g_dlg);
            HMENU popup = CreatePopupMenu();
            POINT pt;
            GetCursorPos(&pt);
            if (g_intray)
            {
              InsertMenu(popup, -1, MF_STRING|MF_BYCOMMAND, CMD_SHOWWINDOWFROMTRAY, g_copydlg ? "&Show Progress" : "&Show Analysis");
              if (m_comparing) InsertMenu(popup, -1, MF_STRING|MF_BYCOMMAND, CMD_CANCELANALYSIS, "&Cancel Analysis");
              if (g_copydlg) InsertMenu(popup, -1, MF_STRING|MF_BYCOMMAND, CMD_CANCELCOPY, "&Cancel Synchronization");
              if (!g_copydlg && !m_comparing) InsertMenu(popup, -1, MF_SEPARATOR, 0, NULL);
            }
            if (!g_copydlg && !m_comparing) InsertMenu(popup, -1, MF_STRING|MF_BYCOMMAND, CMD_EXITPATHSYNC, "&Exit PathSync");
            SetMenuDefaultItem(popup, g_intray ? CMD_SHOWWINDOWFROMTRAY : CMD_EXITPATHSYNC, FALSE);
            TrackPopupMenuEx(popup, TPM_LEFTALIGN|TPM_LEFTBUTTON, pt.x, pt.y, g_dlg, NULL);
          }
        return 0;
			}
    return 0;
    case WM_DROPFILES:
      {
        HDROP hDrop=(HDROP)wParam;
        char buf[2048];
        if (DragQueryFile(hDrop,0,buf,sizeof(buf))>4)
        {
          if (!stricmp(buf+strlen(buf)-4,".pss"))
          {
              stopAnalyzeAndClearList(hwndDlg);
              if (load_settings(hwndDlg,"pathsync settings",buf) > 0)
              {
                set_current_settings_file(hwndDlg,buf);
              }
          }
        }
        DragFinish(hDrop);
      }
    break;
    case WM_COMMAND:
      switch (LOWORD(wParam))
      {
        // todo: shell integration
        case IDM_LOAD_SYNC_SETTINGS:
          {
            char cpath[MAX_PATH*2];
            char temp[4096];
            OPENFILENAME l={sizeof(l),};
            strcpy(temp,m_lastsettingsfile);
            l.hwndOwner = hwndDlg;
            l.lpstrFilter = "PathSync Settings (*.PSS)\0*.PSS\0All Files\0*.*\0\0";
            l.lpstrFile = temp;
            l.nMaxFile = sizeof(temp)-1;
            l.lpstrTitle = "Load SyncSettings from file:";
            l.lpstrDefExt = "PSS";
            GetCurrentDirectory(MAX_PATH*2,cpath);
            l.lpstrInitialDir=cpath;
            l.Flags = OFN_HIDEREADONLY|OFN_EXPLORER;
            if (GetOpenFileName(&l)) 
            {
              stopAnalyzeAndClearList(hwndDlg);
              if (load_settings(hwndDlg,"pathsync settings",temp) > 0)
                set_current_settings_file(hwndDlg,temp);
            }
          }
        break;
        case IDM_SAVE_SYNC_SETTINGS:
          {
            char cpath[MAX_PATH*2];
            char temp[4096];
            strcpy(temp,m_lastsettingsfile);
            OPENFILENAME l={sizeof(l),};
            l.hwndOwner = hwndDlg;
            l.lpstrFilter = "PathSync Settings (*.PSS)\0*.PSS\0All Files\0*.*\0\0";
            l.lpstrFile = temp;
            l.nMaxFile = sizeof(temp)-1;
            l.lpstrTitle = "Save SyncSettings to file:";
            l.lpstrDefExt = "PSS";
            GetCurrentDirectory(MAX_PATH*2,cpath);
            l.lpstrInitialDir=cpath;
            l.Flags = OFN_HIDEREADONLY|OFN_EXPLORER;
            if (GetSaveFileName(&l)) 
            {
              save_settings(hwndDlg,"pathsync settings",temp);
              set_current_settings_file(hwndDlg,temp);
            }
          }
        break;
        case IDM_ABOUT:
          MessageBox(hwndDlg,"PathSync " PATHSYNC_VER "\r\nCopyright (C) 2004-2005, Cockos Inc.\r\n"
            "For updates visit http://www.cockos.com/pathsync/\r\n"
            "\r\n"
                    "PathSync is free software; you can redistribute it and/or modify\r\n"
                        "it under the terms of the GNU General Public License as published by\r\n"
                        "the Free Software Foundation; either version 2 of the License, or\r\n"
                        "(at your option) any later version.\r\n"
                     "\r\n"
                        "PathSync is distributed in the hope that it will be useful,\r\n"
                        "but WITHOUT ANY WARRANTY; without even the implied warranty of\r\n"
                        "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\r\n"
                        "GNU General Public License for more details.\r\n"
                    "\r\n"
                        "You should have received a copy of the GNU General Public License\r\n"
                        "along with PathSync; if not, write to the Free Software\r\n"
                        "Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA\r\n"                        
            ,
                                  
            "About PathSync",MB_OK);
        break;
        case IDM_EXIT:
          PostQuitMessage(0);
        break;
        case CMD_SHOWWINDOWFROMTRAY: 
          show_window_from_tray();
        break;
        case CMD_EXITPATHSYNC:
          if (!g_copydlg) PostQuitMessage(0);
        break;
        case CMD_CANCELCOPY:
          if (g_copydlg) PostMessage(g_copydlg, WM_COMMAND, IDCANCEL, 0);
        break;
        case CMD_CANCELANALYSIS:
          cancel_analysis(hwndDlg);
        break;
        case IDC_DEFBEHAVIOR:
          if (HIWORD(wParam) == CBN_SELCHANGE)
          {
            stopAnalyzeAndClearList(hwndDlg);
          }
        break;
        case IDC_ANALYZE:
          if (m_comparing)
          {
            cancel_analysis(hwndDlg);
          }
          else
          {
            RestartLogging(hwndDlg);
            LogMessage("Analysis");
            systray_mod(hwndDlg, 0, "PathSync - Analysis in progress");

            g_ignflags=0;
            if (IsDlgButtonChecked(hwndDlg,IDC_IGNORE_SIZE)) g_ignflags |= 1;
            if (IsDlgButtonChecked(hwndDlg,IDC_IGNORE_DATE)) g_ignflags |= 2;
            if (IsDlgButtonChecked(hwndDlg,IDC_IGNORE_MISSLOCAL)) g_ignflags |= 4;
            if (IsDlgButtonChecked(hwndDlg,IDC_IGNORE_MISSREMOTE)) g_ignflags |= 8;
            g_defbeh=SendDlgItemMessage(hwndDlg,IDC_DEFBEHAVIOR,CB_GETCURSEL,0,0);

            clearFileLists(hwndDlg);
            SetDlgItemText(hwndDlg,IDC_STATS,"");

            char buf[2048];
            GetDlgItemText(hwndDlg,IDC_PATH1,buf,sizeof(buf));
            while (buf[0] && buf[strlen(buf)-1] == '\\') buf[strlen(buf)-1]=0;
            m_curscanner_basepath[0].Set(buf);
            GetDlgItemText(hwndDlg,IDC_PATH2,buf,sizeof(buf));
            while (buf[0] && buf[strlen(buf)-1] == '\\') buf[strlen(buf)-1]=0;
            m_curscanner_basepath[1].Set(buf);

            // just in case it didn't get cleared at the end of the last analysis, somehow
            free_pattern_list(&m_include_files);

            GetDlgItemText(hwndDlg,IDC_INCLUDE_FILES,buf,sizeof(buf));
            parse_pattern_list(hwndDlg, buf, &m_include_files);

            m_curscanner_relpath[0].Set("");
            m_curscanner_relpath[1].Set("");

            WDL_String msgLocal("Local Path: ");
            msgLocal.Append(m_curscanner_basepath[0].Get());
            LogMessage(msgLocal.Get());
            
            WDL_String msgRemote("Remote Path: ");
            msgRemote.Append(m_curscanner_basepath[1].Get());
            LogMessage(msgRemote.Get());
            
            if (m_curscanner[0].First(m_curscanner_basepath[0].Get()))
            {
              WDL_String msg("Error reading path: ");
              msg.Append(m_curscanner_basepath[0].Get());
              if (!g_autorun)
        	  {
                MessageBox(hwndDlg, msg.Get(), "Error", MB_OK);
              }
              LogMessage(msg.Get());
        	}
        	else if (m_curscanner[1].First(m_curscanner_basepath[1].Get()))
        	{
              WDL_String msg("Error reading path: ");
              msg.Append(m_curscanner_basepath[1].Get());
              if (!g_autorun)
              {
                MessageBox(hwndDlg, msg.Get(), "Error", MB_OK);
              }
              LogMessage(msg.Get());
            }
            else
            {
              // start new scan
              SetDlgItemText(hwndDlg,IDC_ANALYZE,"Stop...");
              m_comparing=1;
              SetTimer(hwndDlg,32,50,NULL);
              int x;
              for (x = 0; x < sizeof(endislist)/sizeof(endislist[0]); x ++)
                EnableWindow(GetDlgItem(hwndDlg,endislist[x]),0);
              EnableWindow(GetDlgItem(hwndDlg,IDC_GO),0);              
            }

          }
        break;
        case IDOK:
        break;
        case IDC_MASKHELP:
          MessageBox(hwndDlg,"The filename mask box allows you to specify a list of rules to\r\n"
                             "let you either include or exclude files from the analysis/copy.\r\n"
                             "The rules are seperated by semicolons, and are evaluated from left\r\n"
                             "to right. When a rule is matched, no further rules are checked.\r\n"
                             "Rules beginning with !, when matched, mean the item is excluded.\r\n"
                             "Examples:\r\n"
                             "\t*\t\t (includes everything)\r\n"
                             "\t!*.pch;*\t\t (includes everything but files ending in .pch)\r\n"
                             "\t*.mp3;*.jpg;*.avi\t(includes only mp3, jpg, and avi files)\r\n"
                             "\t!temp\\;*\t\t(includes everything except the temp\\ directory)\r\n"
                                   
            ,"Filename Mask Help", MB_OK);
        break;
        case IDC_BROWSE1:
        case IDC_BROWSE2:
          {
            char name[1024];
            BROWSEINFO bi={hwndDlg,NULL,name,"Choose a Directory",BIF_RETURNONLYFSDIRS,NULL,};
            ITEMIDLIST *id=SHBrowseForFolder(&bi);
            if (id)
            {
				      SHGetPathFromIDList( id, name );        

	            IMalloc *m;
	            SHGetMalloc(&m);
	            m->Free(id);
              SetDlgItemText(hwndDlg,LOWORD(wParam) == IDC_BROWSE1 ? IDC_PATH1 : IDC_PATH2, name);
                m->Release();
            }
          }
        break;            
        case IDC_LOGBROWSE:
          {
            char name[1024] = "";
            OPENFILENAME ofn = {0};
            ofn.lStructSize = sizeof OPENFILENAME;
            ofn.hwndOwner = hwndDlg;
            ofn.lpstrTitle = "Choose a Log File";
            ofn.lpstrFilter = "Log Files (.log)\0*.log\0All Files (*.*)\0*.*\0";
            ofn.lpstrFile = name;
            ofn.nMaxFile = sizeof name;

            if (GetOpenFileName(&ofn))
            {
              SetDlgItemText(hwndDlg, IDC_LOGPATH, name);
            }
          }
        break;
        case IDC_GO:
          if (ListView_GetItemCount(m_listview))
          {
            if (!g_autorun) // no need for duplicate request to run during autorun
            {
              RestartLogging(hwndDlg);
            }
            LogMessage("Sync");

            ShowWindow(hwndDlg,SW_HIDE);
            g_copydlg = CreateDialog(g_hInstance,MAKEINTRESOURCE(IDD_DIALOG2),NULL,copyFilesProc);
            if (!g_systray || !g_intray) ShowWindow(g_copydlg, SW_NORMAL);
          }
        break;
        case IDC_LOG:
            EnableOrDisableLoggingControls(hwndDlg);
        break;
      }
    break;
    case WM_COPYDIALOGEND:
      DestroyWindow(g_copydlg);
      g_copydlg = NULL;
      clearFileLists(hwndDlg);
      SetDlgItemText(hwndDlg,IDC_STATS,"");
      SetDlgItemText(hwndDlg,IDC_STATUS,"");
      EnableWindow(GetDlgItem(hwndDlg,IDC_GO),0);
      if (!g_systray || !g_intray) ShowWindow(hwndDlg,SW_SHOW);

      if (g_autorun)
      {
        PostQuitMessage(1);
      }
    return 0;
    case WM_NOTIFY:
      {
        LPNMHDR l=(LPNMHDR)lParam;
        if (l->idFrom == IDC_LIST1 && l->code == NM_RCLICK && !m_comparing && ListView_GetSelectedCount(m_listview))
        {
          HMENU h=LoadMenu(g_hInstance,MAKEINTRESOURCE(IDR_MENU1));
          if (h)
          {
            HMENU h2=GetSubMenu(h,0);
            if (h2)
            {
              int sel=0;
              int localonly=0;
              int remoteonly=0;

              POINT p;
              GetCursorPos(&p);

              int y,l=ListView_GetItemCount(m_listview);
              for (y = 0; y < l; y ++)
              {
                if (ListView_GetItemState(m_listview, y, LVIS_SELECTED) & LVIS_SELECTED)
                {
                  char buf[128];
                  ListView_GetItemText(m_listview,y,1,buf,sizeof(buf));

                  if (!strcmp(buf,LOCAL_ONLY_STR)) localonly++;
                  else if (!strcmp(buf,REMOTE_ONLY_STR)) remoteonly++;


                  ListView_GetItemText(m_listview,y,2,buf,sizeof(buf));
                  //
                  if (!strcmp(buf,ACTION_RECV)) sel|=1;
                  else if (!strcmp(buf,ACTION_SEND)) sel|=2;
                  else if (!strcmp(buf,ACTION_NONE)) sel|=4;
                }
              }
              if (sel == 1)
              {
                CheckMenuItem(h2,IDM_2TO1,MF_CHECKED);
              }
              if (sel == 2)
              {
                CheckMenuItem(h2,IDM_1TO2,MF_CHECKED);
              }
              if (sel == 4)
              {
                CheckMenuItem(h2,IDM_NOACTION,MF_CHECKED);
              }

              int do_action_change=0;
              int x=TrackPopupMenu(h2,TPM_RETURNCMD,p.x,p.y,0,hwndDlg,NULL);
              switch (x)
              {
                case IDM_2TO1:
                  if (localonly)
                  {
                    char buf[512];
                    sprintf(buf,"Setting the action to " ACTION_RECV " will result in %d local file%s being removed.\r\n"
                        "If this is acceptable, select Yes. Otherwise, select No.",localonly,localonly==1?"":"s");
                    if (MessageBox(hwndDlg,buf,"PathSync Warning",MB_YESNO|MB_ICONQUESTION) == IDYES) do_action_change=1;
                  }
                  else do_action_change=1;
                break;
                case IDM_1TO2:
                  if (remoteonly)
                  { 
                    char buf[512];
                    sprintf(buf,"Setting the action to " ACTION_SEND " will result in %d remote file%s being removed.\r\n"
                      "If this is acceptable, select Yes. Otherwise, select No.",remoteonly,remoteonly==1?"":"s");
                    if (MessageBox(hwndDlg,buf,"PathSync Warning",MB_YESNO|MB_ICONQUESTION) == IDYES) do_action_change=2;
                  }
                  else do_action_change=2;
                break;
                case IDM_NOACTION:
                  do_action_change=3;
                break;
              }
              if (do_action_change)
              {
                for (y = 0; y < l; y ++)
                {
                  if (ListView_GetItemState(m_listview, y, LVIS_SELECTED) & LVIS_SELECTED)
                  {
                    char *s=ACTION_NONE;
                    if (do_action_change == 1) s=ACTION_RECV;
                    if (do_action_change == 2) s=ACTION_SEND;
                    ListView_SetItemText(m_listview,y,2,s);
                  }
                }
                calcStats(hwndDlg);
              }
            }           
            DestroyMenu(h);
          }
        }
      }
    break;
    case WM_TIMER:
      if (wParam == 32)
      {
        bool finished = false;
        static int in_timer;
        if (!in_timer)
        {
          in_timer=1;
          unsigned int start_t=GetTickCount();
          while (GetTickCount() - start_t < 100)
          {
            if (m_comparing < 7)
            {
              int x;
              for (x = 0; x < 2; x ++)
              {
                if (!(m_comparing&(2<<x)))
                {
                  // update item
                  char *ptr=m_curscanner[x].GetCurrentFN();
                  if (strcmp(ptr,".") && strcmp(ptr,".."))
                  {
                    WDL_String relname;
                    relname.Set(m_curscanner_relpath[x].Get());
                    if (m_curscanner_relpath[x].Get()[0]) relname.Append("\\");
                    relname.Append(ptr);
                    int isdir=m_curscanner[x].GetCurrentIsDirectory();
                    if (isdir) relname.Append("\\");

                    if (!test_file_pattern(relname.Get(),isdir))
                    {
                      // do nothing
                    }
                    else if (isdir)
                    {
                      WDL_String *s=new WDL_String(m_curscanner_relpath[x].Get());
                      if (m_curscanner_relpath[x].Get()[0]) s->Append("\\");
                      s->Append(ptr);
                      m_dirscanlist[x].Add(s);
                    }
                    else
                    {
                      dirItem *di=new dirItem;
                      di->relativeFileName.Set(relname.Get());

                      di->fileSize.LowPart = m_curscanner[x].GetCurrentFileSize(&di->fileSize.HighPart);
                      m_curscanner[x].GetCurrentLastWriteTime(&di->lastWriteTime);

                      m_files[x].Add(di);

                      WDL_String str2("Scanning file: ");
                      str2.Append(di->relativeFileName.Get());
                      str2.Append("\n");
                      SetDlgItemText(hwndDlg,IDC_STATUS,str2.Get());
    //                    OutputDebugString(str2.Get());
                    }
                  }

                  if (m_curscanner[x].Next())
                  {
                    int success=0;
                    m_curscanner[x].Close();
                    // done with this dir!
                    while (m_dirscanlist[x].GetSize()>0 && !success)
                    {
                      int i=m_dirscanlist[x].GetSize()-1;
                      WDL_String *str=m_dirscanlist[x].Get(i);
                      m_curscanner_relpath[x].Set(str->Get());
                      m_dirscanlist[x].Delete(i);
                      delete str;

                      WDL_String s(m_curscanner_basepath[x].Get());
                      s.Append("\\");
                      s.Append(m_curscanner_relpath[x].Get());
                      if (!m_curscanner[x].First(s.Get())) success++;
                    }
                    if (!success) m_comparing |= 2<<x;                  
                  }
                }
              } // each dir
            } // < 7
            else if (m_comparing == 7) // sort 1
            {
              if (m_files[0].GetSize()>1) 
                qsort(m_files[0].GetList(),m_files[0].GetSize(),sizeof(dirItem *),(int (*)(const void*, const void*))filenameCompareFunction);
              m_comparing++;
            }
            else if (m_comparing == 8) // sort 2!
            {
              // this isn't really necessary, but it's fast and then provides consistent output for the ordering
              if (m_files[1].GetSize()>1) 
                qsort(m_files[1].GetList(),m_files[1].GetSize(),sizeof(dirItem *),(int (*)(const void*, const void*))filenameCompareFunction);
              m_comparing++;
              m_comparing_pos=0;
              m_comparing_pos2=0;
            }         
            else if (m_comparing == 9) // search m_files[0] for every m_files[1], reporting missing and different files
            {
              if (!m_files[1].GetSize())
              {
                m_comparing++;
              }
              else
              {
                dirItem **p=m_files[1].GetList()+m_comparing_pos;

                dirItem **res=0;
                if (m_files[0].GetSize()>0) res=(dirItem **)bsearch(p,m_files[0].GetList(),m_files[0].GetSize(),sizeof(dirItem *),(int (*)(const void*, const void*))filenameCompareFunction);

                if (!res)
                {
                  if (!(g_ignflags&4))
                  {
                    int x=ListView_GetItemCount(m_listview);
                    LVITEM lvi={LVIF_PARAM|LVIF_TEXT,x};
                    lvi.pszText = (*p)->relativeFileName.Get();
                    lvi.lParam = m_listview_recs.GetSize();
                    m_listview_recs.Add(NULL);
                    m_listview_recs.Add(*p);
                    ListView_InsertItem(m_listview,&lvi);
                    ListView_SetItemText(m_listview,x,1,REMOTE_ONLY_STR);
                    ListView_SetItemText(m_listview,x,2,
                      g_defbeh == 3 ? ACTION_SEND : g_defbeh == 1 ? ACTION_NONE : ACTION_RECV                                    
                    );
                  }
                }
                else 
                {
                  (*res)->refcnt++;
                  ULARGE_INTEGER fta=*(ULARGE_INTEGER *)&(*p)->lastWriteTime;
                  ULARGE_INTEGER ftb=*(ULARGE_INTEGER *)&(*res)->lastWriteTime;
                  __int64 datediff = fta.QuadPart - ftb.QuadPart;
                  if (datediff < 0) datediff=-datediff;
                  int dateMatch = datediff < 10000000 || (g_ignflags & 2); // if difference is less than 1s, than they are equal
                  int sizeMatch = ((*p)->fileSize.QuadPart == (*res)->fileSize.QuadPart) || (g_ignflags & 1);
                  if (!sizeMatch || !dateMatch)
                  {
                    int x=ListView_GetItemCount(m_listview);
                    int insertpos=m_comparing_pos2++;
                    LVITEM lvi={LVIF_PARAM|LVIF_TEXT,insertpos};
                    lvi.pszText = (*p)->relativeFileName.Get();
                    lvi.lParam = m_listview_recs.GetSize();
                    m_listview_recs.Add(*res);
                    m_listview_recs.Add(*p);
                    ListView_InsertItem(m_listview,&lvi);
                    char *datedesc=0,*sizedesc=0;

                    if (!dateMatch)
                    {
                      if (fta.QuadPart > ftb.QuadPart) 
                      {
                        datedesc="Remote Newer";
                      }
                      else
                      {
                        datedesc="Local Newer";
                      }
                    }
                    if (!sizeMatch)
                    {
                      if ((*p)->fileSize.QuadPart > (*res)->fileSize.QuadPart) 
                      {
                        sizedesc="Remote Larger";
                      }
                      else
                      {
                        sizedesc="Local Larger";
                      }
                    }
                    char buf[512];
                    if (sizedesc && datedesc) 
                      sprintf(buf,"%s, %s",datedesc,sizedesc);
                    else
                      strcpy(buf,datedesc?datedesc:sizedesc);

                    ListView_SetItemText(m_listview,insertpos,1,buf);

                    if (g_defbeh == 1 || g_defbeh == 3)
                    {
                      ListView_SetItemText(m_listview,insertpos,2,ACTION_SEND);
                    }
                    else if (g_defbeh == 2 || g_defbeh == 4)
                    {
                      ListView_SetItemText(m_listview,insertpos,2,ACTION_RECV);
                    }
                    else
                      ListView_SetItemText(m_listview,insertpos,2,
                            dateMatch ? ((*p)->fileSize.QuadPart > (*res)->fileSize.QuadPart ? ACTION_RECV:ACTION_SEND) : 
                                         fta.QuadPart > ftb.QuadPart ? ACTION_RECV:ACTION_SEND);
                  }
                }

                m_comparing_pos++;
                if (m_comparing_pos >= m_files[1].GetSize())
                {
                  m_comparing++;
                  m_comparing_pos=0;
                }
              }
            }
            else if (m_comparing == 10) // scan for files in [0] that havent' been referenced
            {
              if (m_files[0].GetSize() < 1)
              {
                m_comparing++;
              }
              // at this point, we go through m_files[0] and search m_files[1] for files not 
              else 
              {
                dirItem **p=m_files[0].GetList()+m_comparing_pos;

                if (!(*p)->refcnt)
                {
                  if (!(g_ignflags & 8))
                  {
                    int x=ListView_GetItemCount(m_listview);
                    LVITEM lvi={LVIF_PARAM|LVIF_TEXT,x};
                    lvi.pszText = (*p)->relativeFileName.Get();
                    lvi.lParam = m_listview_recs.GetSize();
                    m_listview_recs.Add(*p);
                    m_listview_recs.Add(NULL);
                    ListView_InsertItem(m_listview,&lvi);
                    ListView_SetItemText(m_listview,x,1,LOCAL_ONLY_STR);

                    ListView_SetItemText(m_listview,x,2,
                      g_defbeh == 4 ? ACTION_RECV : g_defbeh == 2 ? ACTION_NONE : ACTION_SEND
                    );
                  }
                }

                m_comparing_pos++;
                if (m_comparing_pos >= m_files[0].GetSize())
                {
                  m_comparing++;
                  m_comparing_pos=0;
                }
              }
            }
            else if (m_comparing == 11)
            {
              KillTimer(hwndDlg,32);
              SetDlgItemText(hwndDlg,IDC_ANALYZE,"Analyze");
              SetDlgItemText(hwndDlg,IDC_STATUS,"Status: Done");
              m_comparing=0;

              int x;
              for (x = 0; x < sizeof(endislist)/sizeof(endislist[0]); x ++)
                EnableWindow(GetDlgItem(hwndDlg,endislist[x]),1);
              EnableWindow(GetDlgItem(hwndDlg,IDC_GO),1);     
              calcStats(hwndDlg);

              free_pattern_list(&m_include_files);
              systray_mod(hwndDlg, 0, "PathSync");
              g_lasttraypercent = -1;
    
              finished = true;
              break; // exit loop
            }
          } // while
          in_timer=0;
        } // if (!in_timer)

        if (finished && g_autorun)
        {
          if (IsWindowEnabled(GetDlgItem(hwndDlg, IDC_GO)))
          {
            PostMessage(hwndDlg, WM_COMMAND, MAKEWPARAM(IDC_GO, 1), (LPARAM)GetDlgItem(hwndDlg, IDC_GO));
          }
          else if (g_autorun)
          {
            PostQuitMessage(1);
          }
        }

      } // (wParam == 32)
    break;
  }

  return 0;
}

char * skip_root(char *path)
{
  char *p = CharNext(path);
  char *p2 = CharNext(p);

  if (*path && *p == ':' && *p2=='\\')
  {
    return CharNext(p2);
  }
  else if (*path == '\\' && *p == '\\')
  {
    // skip host and share name
    int x = 2;
    while (x--)
    {
      while (*p2 != '\\')
      {
        if (!*p2)
          return NULL;
        p2 = CharNext(p2);
      }
      p2 = CharNext(p2);
    }

    return p2;
  }
  else
    return NULL;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpszCmdParam, int nShowCmd)
{
  g_hInstance=hInstance;
  InitCommonControls();

  GetModuleFileName(hInstance,m_inifile,sizeof(m_inifile)-32);
  strcat(m_inifile,".ini");


  {
    int state=0;
    char *p=lpszCmdParam;
    while (*p)
    {
      char parm[2048];
      int parm_pos=0,qs=0;

      while (isspace(*p)) p++;
      if (!*p) break;

      while (*p && (!isspace(*p) || qs))
      {
        if (*p == '\"') qs=!qs;
        else if (parm_pos < (int)sizeof(parm)-1) parm[parm_pos++]=*p;       
        p++;
      }
      parm[parm_pos]=0;

      if (parm[0] == '/') parm[0]='-';
      switch (state)
      {
        case 0:
          if (!stricmp(parm,"-loadpss"))
          {
            state=1;
          }
          else if (!stricmp(parm,"-autorun"))
          {
            g_autorun = true;
          }
          else if (!stricmp(parm,"-systray"))
          {
            g_systray = true;
          }
          else
          {
            state=-1;
          }
        break;
        case 1:
          if (parm[0]=='-') state=-1;
          else
          {
            lstrcpyn(g_loadsettingsfile,parm,sizeof(g_loadsettingsfile));
            state=0;
          }
        break;
      }
      if (state < 0) break;
    }
    if (state)
    {
      MessageBox(NULL,"Usage:\r\npathsync [-loadpss <filename> [-autorun]]","PathSync Usage",MB_OK);
      return 0;
    }
  }

  // fg, 4/20/2005, changed from DialogBox to CreateDialogBox + messagequeue in order to be able to start the dialog hidden
  g_dlg = CreateDialog(hInstance,MAKEINTRESOURCE(IDD_DIALOG1),NULL,mainDlgProc);
  if (!g_systray) { ShowWindow(g_dlg, SW_NORMAL); }
  else g_intray = true;

  MSG msg;
  while (GetMessage(&msg, (HWND) NULL, 0, 0)) 
  { 
    if (IsDialogMessage(g_dlg, &msg)) continue;
    if (g_copydlg && IsDialogMessage(g_copydlg, &msg)) continue;
    // should never get here, but this code is there in case future extentions use CreateWindow*
    TranslateMessage(&msg); 
    DispatchMessage(&msg); 
  } 

  // Calls WM_DESTROY, saves settings
  DestroyWindow(g_dlg);

  return 0;
}


//////////// file copying code, eh

int m_copy_entrypos;
int m_copy_done;
int m_copy_deletes,m_copy_files;
unsigned int m_copy_starttime;
__int64 m_copy_bytestotalsofar;

class fileCopier
{
  public:
    fileCopier()
    {
      m_filepos=0;
      m_filesize.QuadPart=0;
      m_srcFile=m_dstFile=INVALID_HANDLE_VALUE;
      m_stt=GetTickCount();
      m_nud=0;
    }
    int openFiles(char *src, char *dest, HWND hwndParent, char *relfn) // returns 1 on error
    {
      m_fullsrcfn.Set(src);
      m_fulldestfn.Set(dest);
      m_relfn.Set(relfn);
      
      m_srcFile = CreateFile(src,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,NULL);
      if (m_srcFile == INVALID_HANDLE_VALUE)
      {
        WDL_String tmp("Error opening source: ");
        tmp.Append(src);

        LogMessage(tmp.Get());
        SendDlgItemMessage(hwndParent,IDC_LIST1,LB_ADDSTRING,0,(LPARAM)tmp.Get());
        return -1;
      }

      {
        WDL_String tmp(dest);
        char *p=tmp.Get();
        if (*p) 
        {
          p = skip_root(tmp.Get());
          if (p) for (;;)
          {
            while (*p != '\\' && *p) p=CharNext(p);
            if (!*p) break;

            char c=*p;
            *p=0;
            CreateDirectory(tmp.Get(),NULL);
            *p++ = c;
          }
        }
      }
      

      m_tmpdestfn.Set(dest);
      m_tmpdestfn.Append(".PSYN_TMP");
      m_dstFile = CreateFile(m_tmpdestfn.Get(),GENERIC_WRITE,0,NULL,CREATE_ALWAYS,0,NULL);
      if (m_dstFile == INVALID_HANDLE_VALUE)
      {
        WDL_String tmp("Error opening tmpdest: ");
        tmp.Append(m_tmpdestfn.Get());

        LogMessage(tmp.Get());
        SendDlgItemMessage(hwndParent,IDC_LIST1,LB_ADDSTRING,0,(LPARAM)tmp.Get());
        return -1;
      }

      m_filesize.LowPart = GetFileSize(m_srcFile,&m_filesize.HighPart);
      m_filepos=0;

      SendDlgItemMessage(hwndParent,IDC_FILEPROGRESS,PBM_SETRANGE,0,MAKELPARAM(0,10000));
      SendDlgItemMessage(hwndParent,IDC_FILEPROGRESS,PBM_SETPOS,0,0);
      SetDlgItemText(hwndParent,IDC_FILEPOS,"Initializing...");

      return 0;
    }

    int run(HWND hwndParent) // return 1 when done
    {
      char buf[256*1024];
      DWORD r;
      if (!ReadFile(m_srcFile,buf,sizeof(buf),&r,NULL))
      {
        WDL_String tmp("Error reading: ");
        tmp.Append(m_relfn.Get());
        LogMessage(tmp.Get());
        SendDlgItemMessage(hwndParent,IDC_LIST1,LB_ADDSTRING,0,(LPARAM)tmp.Get());
        return -1;
      }

      if (r)
      {
        m_copy_bytestotalsofar+=r;
        m_filepos += r;
        g_throttle_bytes+=r;

        DWORD or;
        if (!WriteFile(m_dstFile,buf,r,&or,NULL) || or != r)
        {
          WDL_String tmp("Error writing to: ");
          tmp.Append(m_relfn.Get());
          LogMessage(tmp.Get());
          SendDlgItemMessage(hwndParent,IDC_LIST1,LB_ADDSTRING,0,(LPARAM)tmp.Get());
          return -1;
        }
      }

      unsigned int now=GetTickCount();
      if (now > m_nud || r < sizeof(buf))
      {
        m_nud=now+100;
        int v = 0;
        unsigned int tm=now-m_stt;
        if (!tm) tm=1;
        if (m_filesize.QuadPart) v=(int) ((m_filepos * 10000) / m_filesize.QuadPart);
        SendDlgItemMessage(hwndParent,IDC_FILEPROGRESS,PBM_SETPOS,(WPARAM)v,0);
        {
          char text[512];
          char tmp1[128],tmp2[128],tmp3[128];
          format_size_string(m_filepos,tmp1);
          format_size_string(m_filesize.QuadPart,tmp2);
          format_size_string((m_filepos * 1000)/tm,tmp3);

          sprintf(text,"%d%% - %s/%s @ %s/s",v/100,tmp1,tmp2,tmp3);
          SetDlgItemText(hwndParent,IDC_FILEPOS,text);
        }
      }

      if (r < sizeof(buf)) // eof!
      {
        if (m_filesize.QuadPart < 16384) g_throttle_bytes+=16384;
        FILETIME ft;
        GetFileTime(m_srcFile,NULL,NULL,&ft);
        SetFileTime(m_dstFile,NULL,NULL,&ft);
        CloseHandle(m_srcFile); m_srcFile=INVALID_HANDLE_VALUE;
        CloseHandle(m_dstFile); m_dstFile=INVALID_HANDLE_VALUE;


        WDL_String destSave(m_fulldestfn.Get());
        destSave.Append(".PSYN_OLD");
        int err=0;

        HANDLE hFE = CreateFile(m_fulldestfn.Get(),0,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,0,NULL);
        if (hFE != INVALID_HANDLE_VALUE) CloseHandle(hFE);

        if (hFE == INVALID_HANDLE_VALUE || MoveFile(m_fulldestfn.Get(),destSave.Get()))
        {
          if (MoveFile(m_tmpdestfn.Get(),m_fulldestfn.Get()))
          {
            if (hFE != INVALID_HANDLE_VALUE) DeleteFile(destSave.Get());
          }
          else 
          {
            if (hFE != INVALID_HANDLE_VALUE) MoveFile(destSave.Get(),m_fulldestfn.Get()); // try and restore old
            err=2;
          }
        }
        else err=1;

        if (err)
        {
          WDL_String tmp("Error finalizing: ");
          tmp.Append(m_relfn.Get());
          LogMessage(tmp.Get());
          SendDlgItemMessage(hwndParent,IDC_LIST1,LB_ADDSTRING,0,(LPARAM)tmp.Get());
          return -1;
        }
        else
        {
          char text[2048] = "";
          unsigned int tm=max(now-m_stt, 1);
          char tmp2[128],tmp3[128];
            
          format_size_string(m_filesize.QuadPart,tmp2);
          format_size_string((m_filepos * 1000)/tm,tmp3);
          sprintf(text,"%s @ %s/s %s", tmp2, tmp3, m_fulldestfn.Get());
          LogMessage(text);
        }

        // close handles, delete/rename files
        m_copy_files++;
        return 1;
      }

      return 0;
    }

    ~fileCopier()
    {
      if (m_dstFile != INVALID_HANDLE_VALUE) 
      {
        CloseHandle(m_dstFile);
        DeleteFile(m_tmpdestfn.Get());
      }
      if (m_srcFile != INVALID_HANDLE_VALUE) CloseHandle(m_srcFile);
      
    }

    WDL_String m_tmpdestfn;

    WDL_String m_fullsrcfn, m_fulldestfn, m_relfn;
    __int64 m_filepos;
    ULARGE_INTEGER m_filesize;
    HANDLE m_srcFile, m_dstFile;
    unsigned int m_stt, m_nud;
};
fileCopier *m_copy_curcopy;
unsigned int m_next_statusupdate;

void updateXferStatus(HWND hwndDlg)
{
  char buf[512];
  char *p=buf;
  unsigned int t = GetTickCount() - m_copy_starttime;
  if (!t) t=1;
  if (!m_total_copy_size) m_total_copy_size=1;
  int v= (int) ((m_copy_bytestotalsofar * 10000) / m_total_copy_size);


  __int64 bytesleft = m_total_copy_size - m_copy_bytestotalsofar;
  int pred_t = 0;
  if (m_copy_bytestotalsofar) pred_t = (int) ((t/1000) * m_total_copy_size / m_copy_bytestotalsofar);

  SendDlgItemMessage(hwndDlg,IDC_TOTALPROGRESS,PBM_SETPOS,v,0);
  char tmp1[128],tmp2[128],tmp3[128];
  format_size_string(m_copy_bytestotalsofar,tmp1);
  format_size_string(m_total_copy_size,tmp2);
  format_size_string((m_copy_bytestotalsofar * 1000) / t,tmp3);

  sprintf(buf,"%d%% - %d file%s (%s/%s) copied at %s/s, %d file%s deleted.\r\nElapsed Time: %d:%02d, Time Remaining: %d:%02d",v/100,
    m_copy_files,m_copy_files==1?"":"s",
    tmp1,
    tmp2,
    tmp3,
    m_copy_deletes,m_copy_deletes==1?"":"s",
    t/60000,(t/1000)%60,
    (pred_t-t/1000)/60,(pred_t-t/1000)%60);
  SetDlgItemText(hwndDlg,IDC_TOTALPOS,p);
  if (g_intray)
  {
    if (v/100 != g_lasttraypercent)
    {
      sprintf(buf, "PathSync - Synchronizing - %d%%", v/100);
      systray_mod(g_dlg, 0, buf);
      g_lasttraypercent = v/100;
    }
  }
}

void LogEndSyncMessage()
{
    char buf[512];
    unsigned int t = GetTickCount() - m_copy_starttime;
    char tmp1[128], tmp2[128];

    format_size_string(m_total_copy_size, tmp1);
    format_size_string(t ? (m_total_copy_size * 1000) / t : 0, tmp2);

    sprintf(buf, "%d file%s copied (%s @ %s/s), %d file%s deleted. Elapsed Time: %d:%02d",
        m_copy_files,
        m_copy_files==1?"":"s",
        tmp1,
        tmp2,
        m_copy_deletes,
        m_copy_deletes==1?"":"s",
        t/60000,
        (t/1000)%60);

    LogMessage(buf);
}

BOOL WINAPI copyFilesProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
  switch (uMsg)
  {
    case WM_INITDIALOG:
      if (GetPrivateProfileInt("config","accopy",0,m_inifile)) CheckDlgButton(hwndDlg,IDC_CHECK1,BST_CHECKED);
      if (g_throttle) CheckDlgButton(hwndDlg,IDC_CHECK2,BST_CHECKED);     
    
      SetDlgItemInt(hwndDlg,IDC_EDIT1,g_throttlespd,FALSE);
      m_copy_starttime=GetTickCount();
      m_next_statusupdate=0;
      m_copy_deletes=m_copy_files=0;
      m_copy_curcopy=0;
      m_copy_entrypos=-1;
      m_copy_done=0;
      m_copy_bytestotalsofar=0;
      SendDlgItemMessage(hwndDlg,IDC_TOTALPROGRESS,PBM_SETRANGE,0,MAKELPARAM(0,10000));
      SendDlgItemMessage(hwndDlg,IDC_TOTALPROGRESS,PBM_SETPOS,0,0);
      SetTimer(hwndDlg,60,50,NULL);

      g_throttle_sttime=GetTickCount();     
      g_throttle_bytes=0;

    return 0;
    case WM_DESTROY:
      if (m_copy_curcopy)
      {
        delete m_copy_curcopy;
        // clean up after copy
        m_copy_curcopy=0;
      }
    return 0;
    case WM_TIMER:
      if (wParam == 60)
      {       
        unsigned int start_t=GetTickCount();
        unsigned int now;
        while ((now=GetTickCount()) - start_t < 200)
        {
          int docopy=1;
          if (g_throttle)
          {
            DWORD now=GetTickCount();
            if (now < g_throttle_sttime || now > g_throttle_sttime+30000)
            {
              g_throttle_sttime=now;
              g_throttle_bytes=0;
            }
            now -= g_throttle_sttime;
            if (!now) now=1;
            int kbytes_sec=(int)(g_throttle_bytes/now);
            if (kbytes_sec > g_throttlespd)
              docopy=0;
          }

          if (docopy && m_copy_curcopy && m_copy_curcopy->run(hwndDlg))
          {
            // if copy finishes, reset 
            delete m_copy_curcopy;
            m_copy_curcopy=0;
          }

          if (now > m_next_statusupdate)
          {
            updateXferStatus(hwndDlg);
            m_next_statusupdate=now+1000;
          }


          if (!m_copy_curcopy)
          {
            m_copy_entrypos++;
            m_next_statusupdate=0;

            if (m_copy_entrypos >= ListView_GetItemCount(m_listview))
            {
              updateXferStatus(hwndDlg);
              SetDlgItemText(hwndDlg,IDC_SRC,"");
              SetDlgItemText(hwndDlg,IDC_DEST,"");
              KillTimer(hwndDlg,60);
              m_copy_done=1;
              SetDlgItemText(hwndDlg,IDC_FILEPOS,"");
              SetDlgItemText(hwndDlg,IDCANCEL,"Close");
              if (IsDlgButtonChecked(hwndDlg,IDC_CHECK1) || g_autorun)
              {
                PostMessage(g_dlg, WM_COPYDIALOGEND, 1, 0);
              }
              LogEndSyncMessage();
              return 0;
            }
            else
            {
              char status[256];
              char action[256];
              char filename[2048];
              ListView_GetItemText(m_listview,m_copy_entrypos,0,filename,sizeof(filename));
              ListView_GetItemText(m_listview,m_copy_entrypos,1,status,sizeof(status));
              ListView_GetItemText(m_listview,m_copy_entrypos,2,action,sizeof(action));

              int isSend=!strcmp(action,ACTION_SEND);
              int isRecv=!strcmp(action,ACTION_RECV);

              if ((isRecv && !strcmp(status,LOCAL_ONLY_STR)) || 
                  (isSend && !strcmp(status,REMOTE_ONLY_STR)))
              {
                SetDlgItemText(hwndDlg,IDC_SRC,"<delete>");
                WDL_String gs;
                gs.Set(m_curscanner_basepath[!isRecv].Get());
                gs.Append("\\");
                gs.Append(filename);
                SetDlgItemText(hwndDlg,IDC_DEST,gs.Get());

                if (!DeleteFile(gs.Get()))
                {
                  WDL_String news("Error removing");
                  news.Append(isRecv ? " local file: " : " remote file: ");
                  news.Append(gs.Get());
                  LogMessage(news.Get());
                  SendDlgItemMessage(hwndDlg,IDC_LIST1,LB_ADDSTRING,0,(LPARAM)news.Get());
                }
                else
                {
                  m_copy_deletes++;
                  WDL_String news("Removed");
                  news.Append(isRecv ? " local file: " : " remote file: ");
                  news.Append(gs.Get());
                  LogMessage(news.Get());
                }
              }
              else if (isRecv || isSend)
              {
                WDL_String gs;
                gs.Set(m_curscanner_basepath[!!isRecv].Get());
                gs.Append("\\");
                gs.Append(filename);
                SetDlgItemText(hwndDlg,IDC_SRC,gs.Get());

                WDL_String outgs;
                outgs.Set(m_curscanner_basepath[!isRecv].Get());
                outgs.Append("\\");
                outgs.Append(filename);
                SetDlgItemText(hwndDlg,IDC_DEST,outgs.Get());

                m_copy_curcopy = new fileCopier;
                if (m_copy_curcopy->openFiles(gs.Get(),outgs.Get(),hwndDlg,filename))
                {
                  // add error string according to x.
                  delete m_copy_curcopy;
                  m_copy_curcopy=0;
                }
              }

              // start new copy
            }
          }
        } // while < 100ms
      } // if 60
    return 0;
    case WM_COMMAND:
      switch (LOWORD(wParam))
      {
        case IDC_CHECK1:
          WritePrivateProfileString("config","accopy", IsDlgButtonChecked(hwndDlg,IDC_CHECK1)?"1":"0",m_inifile);
        break;
        case IDC_CHECK2:
          g_throttle=!!IsDlgButtonChecked(hwndDlg,IDC_CHECK2);
          g_throttle_sttime=GetTickCount();     
          g_throttle_bytes=0;
        break;
        case IDC_EDIT1:
          if (HIWORD(wParam) == EN_CHANGE)
          {
            BOOL t=0;
            int a=GetDlgItemInt(hwndDlg,IDC_EDIT1,&t,FALSE);
            if (t)
            {
              g_throttlespd=a;
              g_throttle_sttime=GetTickCount();
              g_throttle_bytes=0;
            }
          }
        break;
        case IDCANCEL:
          if (m_copy_done || MessageBox(hwndDlg,"Cancel synchronization?","Question",MB_YESNO)==IDYES)
            PostMessage(g_dlg, WM_COPYDIALOGEND, 1, 0);
        break;
      }
    break;
    case WM_SIZE:
      if (g_systray)
      {
			  if (wParam == SIZE_MINIMIZED)
			  {
          g_intray = true;
				  ShowWindow(hwndDlg,SW_HIDE);
			  }
			  else if (wParam == SIZE_RESTORED)
			  {
          g_intray = false;
				  ShowWindow(hwndDlg,SW_SHOW);
			  }
      }
    return 0;
  }
  return 0;
}
