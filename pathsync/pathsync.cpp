#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <stdio.h>
#include <search.h>
#include <stdlib.h>

#include "resource.h"

#include "../WDL/ptrlist.h"
#include "../WDL/string.h"
#include "../WDL/dirscan.h"

#include "../WDL/wingui/wndsize.h"

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

WDL_PtrList<WDL_String> m_dirscanlist[2];
WDL_DirScan m_curscanner[2];
WDL_String m_curscanner_relpath[2],m_curscanner_basepath[2];

WDL_PtrList<dirItem> m_files[2];
WDL_PtrList<dirItem> m_listview_recs;

int m_comparing; // second and third bits mean done for each side
int m_comparing_pos,m_comparing_pos2;
HWND m_listview;
char m_inifile[2048];

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


__int64 m_total_copy_size;

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
  char buf[512];
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

  SetDlgItemText(hwndDlg,IDC_STATS,buf);
  m_total_copy_size=totalbytescopy.QuadPart;
}

BOOL WINAPI mainDlgProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
  static WDL_WndSizer resizer;
  switch (uMsg)
  {
    case WM_INITDIALOG:
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
      

      SetClassLong(hwndDlg,GCL_HICON,(long)LoadIcon(g_hInstance,MAKEINTRESOURCE(IDI_ICON1)));
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
        {
          char path[2048];
          GetPrivateProfileString("config","path1","",path,sizeof(path),m_inifile);
          SetDlgItemText(hwndDlg,IDC_PATH1,path);
          GetPrivateProfileString("config","path2","",path,sizeof(path),m_inifile);
          SetDlgItemText(hwndDlg,IDC_PATH2,path);
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
    return 0;

    case WM_DESTROY:
      {
        char path[2048];
        GetDlgItemText(hwndDlg,IDC_PATH1,path,sizeof(path));
        WritePrivateProfileString("config","path1",path,m_inifile);
        GetDlgItemText(hwndDlg,IDC_PATH2,path,sizeof(path));
        WritePrivateProfileString("config","path2",path,m_inifile);
      }
    return 0;
    case WM_CLOSE:
      EndDialog(hwndDlg,1);
    break;
    case WM_COMMAND:
      switch (LOWORD(wParam))
      {
        case IDC_ANALYZE:
          if (m_comparing)
          {
            KillTimer(hwndDlg,32);
            SetDlgItemText(hwndDlg,IDC_ANALYZE,"Analyze");
            SetDlgItemText(hwndDlg,IDC_STATUS,"Status: Stopped");
            m_comparing=0;
            EnableWindow(GetDlgItem(hwndDlg,IDC_PATH1),1);
            EnableWindow(GetDlgItem(hwndDlg,IDC_PATH2),1);
            EnableWindow(GetDlgItem(hwndDlg,IDC_BROWSE1),1);
            EnableWindow(GetDlgItem(hwndDlg,IDC_BROWSE2),1);
            EnableWindow(GetDlgItem(hwndDlg,IDC_STATS),1);
          }
          else
          {
            clearFileLists(hwndDlg);
            SetDlgItemText(hwndDlg,IDC_STATS,"");

            char buf[2048];
            GetDlgItemText(hwndDlg,IDC_PATH1,buf,sizeof(buf));
            m_curscanner_basepath[0].Set(buf);
            GetDlgItemText(hwndDlg,IDC_PATH2,buf,sizeof(buf));
            m_curscanner_basepath[1].Set(buf);

            m_curscanner_relpath[0].Set("");
            m_curscanner_relpath[1].Set("");

            if (m_curscanner[0].First(m_curscanner_basepath[0].Get()) || 
                m_curscanner[1].First(m_curscanner_basepath[1].Get()))
            {
              MessageBox(hwndDlg,"Error reading a path","Error",MB_OK);
            }
            else
            {
              // start new scan
              SetDlgItemText(hwndDlg,IDC_ANALYZE,"Stop...");
              m_comparing=1;
              SetTimer(hwndDlg,32,50,NULL);
              EnableWindow(GetDlgItem(hwndDlg,IDC_STATS),0);
              EnableWindow(GetDlgItem(hwndDlg,IDC_PATH1),0);
              EnableWindow(GetDlgItem(hwndDlg,IDC_PATH2),0);
              EnableWindow(GetDlgItem(hwndDlg,IDC_BROWSE1),0);
              EnableWindow(GetDlgItem(hwndDlg,IDC_BROWSE2),0);
              EnableWindow(GetDlgItem(hwndDlg,IDC_GO),0);              
            }

          }
        break;
        case IDOK:
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
            }
          }
        break;
        case IDC_GO:
          if (ListView_GetItemCount(m_listview))
          {
            ShowWindow(hwndDlg,SW_HIDE);
            DialogBox(g_hInstance,MAKEINTRESOURCE(IDD_DIALOG2),NULL,copyFilesProc);
            clearFileLists(hwndDlg);
            SetDlgItemText(hwndDlg,IDC_STATS,"");
            SetDlgItemText(hwndDlg,IDC_STATUS,"");
            EnableWindow(GetDlgItem(hwndDlg,IDC_GO),0);
            ShowWindow(hwndDlg,SW_SHOW);
          }
        break;
      }
    break;
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
                    sprintf(buf,"Setting the action to Remote->Local will result in %d local file%s being removed.\r\n"
                        "If this is acceptable, select Yes. Otherwise, select No.",localonly,localonly==1?"":"s");
                    if (MessageBox(hwndDlg,buf,"PathSync Warning",MB_YESNO|MB_ICONQUESTION) == IDYES) do_action_change=1;
                  }
                  else do_action_change=1;
                break;
                case IDM_1TO2:
                  if (remoteonly)
                  { 
                    char buf[512];
                    sprintf(buf,"Setting the action to Local->Remote will result in %d remote file%s being removed.\r\n"
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
                    if (m_curscanner[x].GetCurrentIsDirectory())
                    {
                      WDL_String *s=new WDL_String(m_curscanner_relpath[x].Get());
                      if (m_curscanner_relpath[x].Get()[0]) s->Append("\\");
                      s->Append(ptr);
                      m_dirscanlist[x].Add(s);
                    }
                    else
                    {
                      dirItem *di=new dirItem;
                      di->relativeFileName.Set(m_curscanner_relpath[x].Get());
                      if (m_curscanner_relpath[x].Get()[0]) di->relativeFileName.Append("\\");
                      di->relativeFileName.Append(ptr);

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
                  int x=ListView_GetItemCount(m_listview);
                  LVITEM lvi={LVIF_PARAM|LVIF_TEXT,x};
                  lvi.pszText = (*p)->relativeFileName.Get();
                  lvi.lParam = m_listview_recs.GetSize();
                  m_listview_recs.Add(NULL);
                  m_listview_recs.Add(*p);
                  ListView_InsertItem(m_listview,&lvi);
                  ListView_SetItemText(m_listview,x,1,REMOTE_ONLY_STR);
                  ListView_SetItemText(m_listview,x,2,"Remote->Local");
                }
                else 
                {
                  (*res)->refcnt++;
                  ULARGE_INTEGER fta=*(ULARGE_INTEGER *)&(*p)->lastWriteTime;
                  ULARGE_INTEGER ftb=*(ULARGE_INTEGER *)&(*res)->lastWriteTime;
                  __int64 datediff = fta.QuadPart - ftb.QuadPart;
                  if (datediff < 0) datediff=-datediff;
                  int dateMatch = datediff < 10000000; // if difference is less than 1s, than they are equal
                  int sizeMatch = (*p)->fileSize.QuadPart == (*res)->fileSize.QuadPart;
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
                    int x=ListView_GetItemCount(m_listview);
                    LVITEM lvi={LVIF_PARAM|LVIF_TEXT,x};
                    lvi.pszText = (*p)->relativeFileName.Get();
                    lvi.lParam = m_listview_recs.GetSize();
                    m_listview_recs.Add(*p);
                    m_listview_recs.Add(NULL);
                    ListView_InsertItem(m_listview,&lvi);
                    ListView_SetItemText(m_listview,x,1,LOCAL_ONLY_STR);
                    ListView_SetItemText(m_listview,x,2,"Local->Remote");
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
              EnableWindow(GetDlgItem(hwndDlg,IDC_STATS),1);
              EnableWindow(GetDlgItem(hwndDlg,IDC_PATH1),1);
              EnableWindow(GetDlgItem(hwndDlg,IDC_PATH2),1);
              EnableWindow(GetDlgItem(hwndDlg,IDC_BROWSE1),1);
              EnableWindow(GetDlgItem(hwndDlg,IDC_BROWSE2),1);
              EnableWindow(GetDlgItem(hwndDlg,IDC_GO),1);     
              calcStats(hwndDlg);
              break; // exit loop
            }
          } // while
          in_timer=0;
        } // if (!in_timer)
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

  DialogBox(hInstance,MAKEINTRESOURCE(IDD_DIALOG1),NULL,mainDlgProc);

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
        SendDlgItemMessage(hwndParent,IDC_LIST1,LB_ADDSTRING,0,(LPARAM)tmp.Get());
        return -1;
      }

      if (r)
      {
        m_copy_bytestotalsofar+=r;
        m_filepos += r;

        DWORD or;
        if (!WriteFile(m_dstFile,buf,r,&or,NULL) || or != r)
        {
          WDL_String tmp("Error writing to: ");
          tmp.Append(m_relfn.Get());
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
          SendDlgItemMessage(hwndParent,IDC_LIST1,LB_ADDSTRING,0,(LPARAM)tmp.Get());
          return -1;
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
}

BOOL WINAPI copyFilesProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
  switch (uMsg)
  {
    case WM_INITDIALOG:
      if (GetPrivateProfileInt("config","accopy",0,m_inifile)) CheckDlgButton(hwndDlg,IDC_CHECK1,BST_CHECKED);
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
          if (m_copy_curcopy && m_copy_curcopy->run(hwndDlg))
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
              if (IsDlgButtonChecked(hwndDlg,IDC_CHECK1)) EndDialog(hwndDlg,1);
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
                  news.Append(filename);
                  SendDlgItemMessage(hwndDlg,IDC_LIST1,LB_ADDSTRING,0,(LPARAM)news.Get());
                }
                else
                  m_copy_deletes++;
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
        case IDCANCEL:
          if (m_copy_done || MessageBox(hwndDlg,"Cancel synchronization?","Question",MB_YESNO)==IDYES)
            EndDialog(hwndDlg,1);
        break;
      }
    break;
  }
  return 0;
}
