#include <windows.h>
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
          }
          else
          {
            clearFileLists();

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
            }

          }
        break;
        case IDOK:
        case IDCANCEL:
          EndDialog(hwndDlg,1);
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
                    OutputDebugString(str2.Get());
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
                GFC_String str2("File is only in path #2: ");
                str2.Append((*p)->relativeFileName.Get());
                str2.Append("\n");
                OutputDebugString(str2.Get());
              }
              else 
              {
                if ((*p)->fileSizeHigh == (*res)->fileSizeHigh && (*p)->fileSizeLow == (*res)->fileSizeLow && !memcmp(&(*p)->lastWriteTime,&(*res)->lastWriteTime,sizeof(FILETIME)))
                {
                  // match, woo! do nothing
                }
                else
                {
                  GFC_String str2("Files differ in date or size: ");
                  str2.Append((*p)->relativeFileName.Get());
                  str2.Append("\n");
                  OutputDebugString(str2.Get());
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
                GFC_String str2("File is only in path #1: ");
                str2.Append((*p)->relativeFileName.Get());
                str2.Append("\n");
                OutputDebugString(str2.Get());
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

  DialogBox(hInstance,MAKEINTRESOURCE(IDD_DIALOG1),NULL,mainDlgProc);

  return 0;
}