#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <stdio.h>
#include <search.h>
#include <stdlib.h>

#include "resource.h"

#include "../gfc/ptrlist.h"
#include "../gfc/string.h"
#include "../gfc/dirscan.h"

HINSTANCE g_hInstance;


class dirItem {

public:
  dirItem() { }
  ~dirItem() { }

  GFC_String relativeFileName;
  DWORD fileSizeLow, fileSizeHigh;
  FILETIME lastWriteTime;

};

GFC_PtrList<GFC_String> m_dirscanlist[2];
GFC_DirScan m_curscanner[2];
GFC_String m_curscanner_relpath[2],m_curscanner_basepath[2];

GFC_PtrList<dirItem> m_files[2];
int m_comparing; // second and third bits mean done for each side
int m_comparing_pos;
HWND m_listview;
char m_inifile[2048];

#define CLEARPTRLIST(xxx) while (xxx.GetSize()>0) { int n=xxx.GetSize()-1; delete xxx.Get(n); xxx.Delete(n); }

int filenameCompareFunction(dirItem **a, dirItem **b)
{
  return stricmp((*a)->relativeFileName.Get(),(*b)->relativeFileName.Get());
}

void clearFileLists()
{
  CLEARPTRLIST(m_dirscanlist[0])
  CLEARPTRLIST(m_dirscanlist[1])
  CLEARPTRLIST(m_files[0])
  CLEARPTRLIST(m_files[1])
}

BOOL WINAPI mainDlgProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
  switch (uMsg)
  {
    case WM_INITDIALOG:
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
    case WM_DESTROY:
      {
        char path[2048];
        GetDlgItemText(hwndDlg,IDC_PATH1,path,sizeof(path));
        WritePrivateProfileString("config","path1",path,m_inifile);
        GetDlgItemText(hwndDlg,IDC_PATH2,path,sizeof(path));
        WritePrivateProfileString("config","path2",path,m_inifile);
      }
    return 0;
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
          }
          else
          {
            clearFileLists();
            ListView_DeleteAllItems(m_listview);

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
              SetTimer(hwndDlg,32,200,NULL);
              EnableWindow(GetDlgItem(hwndDlg,IDC_PATH1),0);
              EnableWindow(GetDlgItem(hwndDlg,IDC_PATH2),0);
              EnableWindow(GetDlgItem(hwndDlg,IDC_BROWSE1),0);
              EnableWindow(GetDlgItem(hwndDlg,IDC_BROWSE2),0);
            }

          }
        break;
        case IDOK:
        break;
        case IDCANCEL:
          EndDialog(hwndDlg,1);
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
          if (m_comparing < 7) do
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
                    GFC_String *s=new GFC_String(m_curscanner_relpath[x].Get());
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

                    di->fileSizeLow = m_curscanner[x].GetCurrentFileSize(&di->fileSizeHigh);
                    m_curscanner[x].GetCurrentLastWriteTime(&di->lastWriteTime);

                    m_files[x].Add(di);

                    GFC_String str2("Scanning file: ");
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
                  while (m_dirscanlist[x].GetSize()>0)
                  {
                    int i=m_dirscanlist[x].GetSize()-1;
                    GFC_String *str=m_dirscanlist[x].Get(i);
                    m_curscanner_relpath[x].Set(str->Get());
                    m_dirscanlist[x].Delete(i);
                    delete str;

                    GFC_String s(m_curscanner_basepath[x].Get());
                    s.Append("\\");
                    s.Append(m_curscanner_relpath[x].Get());
                    if (!m_curscanner[x].First(s.Get()))
                    {
                      success=1;
                      break;
                    }
                  }
                  if (!success) m_comparing |= 2<<x;                  
                }
              }
            } // each dir
          } while (m_comparing < 7 && GetTickCount() - start_t < 100);

          if (m_comparing == 7) // sort 1
          {
            if (m_files[0].GetSize()>1) 
            {
              qsort(m_files[0].GetList(),m_files[0].GetSize(),sizeof(dirItem *),(int (*)(const void*, const void*))filenameCompareFunction);
            }
            m_comparing++;
          }
          else if (m_comparing == 8) // sort 2!
          {
            if (m_files[1].GetSize()>1) qsort(m_files[1].GetList(),m_files[1].GetSize(),sizeof(dirItem *),(int (*)(const void*, const void*))filenameCompareFunction);
            m_comparing++;
            m_comparing_pos=0;
          }
          else if (m_comparing == 9) // search m_files[0] for every m_files[1], reporting missing and different files
          {
            if (!m_files[1].GetSize())
            {
              m_comparing++;
            }
            else while (GetTickCount() - start_t < 100)
            {
              dirItem **p=m_files[1].GetList()+m_comparing_pos;

              dirItem **res=0;
              if (m_files[0].GetSize()>0) res=(dirItem **)bsearch(p,m_files[0].GetList(),m_files[0].GetSize(),sizeof(dirItem *),(int (*)(const void*, const void*))filenameCompareFunction);

              if (!res)
              {
                int x=ListView_GetItemCount(m_listview);
                LVITEM lvi={LVIF_PARAM|LVIF_TEXT,x};
                lvi.pszText = (*p)->relativeFileName.Get();
                lvi.lParam = 0;
                ListView_InsertItem(m_listview,&lvi);
                ListView_SetItemText(m_listview,x,1,"#2 only");
                ListView_SetItemText(m_listview,x,2,"?");
              }
              else 
              {
                int dateMatch = !memcmp(&(*p)->lastWriteTime,&(*res)->lastWriteTime,sizeof(FILETIME));
                int sizeMatch = (*p)->fileSizeHigh == (*res)->fileSizeHigh && 
                    (*p)->fileSizeLow == (*res)->fileSizeLow;
                if (!sizeMatch || !dateMatch)
                {
                  int x=ListView_GetItemCount(m_listview);
                  LVITEM lvi={LVIF_PARAM|LVIF_TEXT,x};
                  lvi.pszText = (*p)->relativeFileName.Get();
                  lvi.lParam = 0;
                  ListView_InsertItem(m_listview,&lvi);
                  char *datedesc=0,*sizedesc=0;

                  if (!dateMatch)
                  {
                    ULARGE_INTEGER a=*(ULARGE_INTEGER *)&(*p)->lastWriteTime;
                    ULARGE_INTEGER b=*(ULARGE_INTEGER *)&(*res)->lastWriteTime;
                    if (a.QuadPart > b.QuadPart) 
                    {
                      datedesc="#2 newer";
                    }
                    else
                    {
                      datedesc="#1 newer";
                    }
                  }
                  if (!sizeMatch)
                  {
                    ULARGE_INTEGER a,b;
                    a.LowPart = (*p)->fileSizeLow;
                    a.HighPart = (*p)->fileSizeHigh;
                    b.LowPart = (*res)->fileSizeLow;
                    b.HighPart = (*res)->fileSizeHigh;
                    if (a.QuadPart > b.QuadPart) 
                    {
                      sizedesc="#2 larger";
                    }
                    else
                    {
                      sizedesc="#1 larger";
                    }
                  }
                  char buf[512];
                  if (sizedesc && datedesc) 
                    sprintf(buf,"%s, %s",datedesc,sizedesc);
                  else
                    strcpy(buf,datedesc?datedesc:sizedesc);
                  ListView_SetItemText(m_listview,x,1,buf);
                  ListView_SetItemText(m_listview,x,2,"?");
                }
              }

              m_comparing_pos++;
              if (m_comparing_pos >= m_files[1].GetSize())
              {
                m_comparing++;
                m_comparing_pos=0;
                break;
              }
            }
          }
          else if (m_comparing == 10) // search m_files[1] for every m_files[0], only reporting missing files
          {
            if (m_files[0].GetSize() < 1)
            {
              m_comparing++;
            }
            // at this point, we go through m_files[0] and search m_files[1] for files not 
            else while (GetTickCount() - start_t < 100)
            {
              dirItem **p=m_files[0].GetList()+m_comparing_pos;

              dirItem **res=0;
              if (m_files[1].GetSize()>0) res=(dirItem **)bsearch(p,m_files[1].GetList(),m_files[1].GetSize(),sizeof(dirItem *),(int (*)(const void*, const void*))filenameCompareFunction);

              if (!res)
              {
                  int x=ListView_GetItemCount(m_listview);
                  LVITEM lvi={LVIF_PARAM|LVIF_TEXT,x};
                  lvi.pszText = (*p)->relativeFileName.Get();
                  lvi.lParam = 0;
                  ListView_InsertItem(m_listview,&lvi);
                  ListView_SetItemText(m_listview,x,1,"#1 only");
                  ListView_SetItemText(m_listview,x,2,"?");
              }

              m_comparing_pos++;
              if (m_comparing_pos >= m_files[1].GetSize())
              {
                m_comparing++;
                m_comparing_pos=0;
                break;
              }
            }
          }
          else if (m_comparing == 11)
          {
            KillTimer(hwndDlg,32);
            SetDlgItemText(hwndDlg,IDC_ANALYZE,"Analyze");
            SetDlgItemText(hwndDlg,IDC_STATUS,"Status: Done");
            m_comparing=0;
            EnableWindow(GetDlgItem(hwndDlg,IDC_PATH1),1);
            EnableWindow(GetDlgItem(hwndDlg,IDC_PATH2),1);
            EnableWindow(GetDlgItem(hwndDlg,IDC_BROWSE1),1);
            EnableWindow(GetDlgItem(hwndDlg,IDC_BROWSE2),1);
          }
          in_timer=0;
        }
      }
    break;
  }

  return 0;
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