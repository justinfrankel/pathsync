;Include Modern UI


!define MUI_COMPONENTSPAGE_NODESC

  !include "MUI.nsh"

;--------------------------------
;General

!define VER_MAJOR 0
!define VER_MINOR 2

SetCompressor lzma



  ;Name and file
  Name "PathSync ${VER_MAJOR}.${VER_MINOR}"
  OutFile "pathsync${VER_MAJOR}${VER_MINOR}-install.exe"

  ;Default installation folder
  InstallDir "$PROGRAMFILES\PathSync"
  
  ;Get installation folder from registry if available
  InstallDirRegKey HKLM "Software\PathSync" ""

;--------------------------------
;Interface Settings

  !define MUI_ABORTWARNING

;--------------------------------
;Pages

  !insertmacro MUI_PAGE_LICENSE "license.txt"
  !insertmacro MUI_PAGE_COMPONENTS
  !insertmacro MUI_PAGE_DIRECTORY
  !insertmacro MUI_PAGE_INSTFILES
  
  !insertmacro MUI_UNPAGE_CONFIRM
  !insertmacro MUI_UNPAGE_INSTFILES
  
;--------------------------------
;Languages
 
  !insertmacro MUI_LANGUAGE "English"




;--------------------------------
;Installer Sections


Section "Required files"

  SectionIn RO
  SetOutPath "$INSTDIR"


  File release\pathsync.exe
    
  ;Store installation folder
  WriteRegStr HKLM "Software\pathsync" "" $INSTDIR
  
  ;Create uninstaller
  WriteUninstaller "$INSTDIR\Uninstall.exe"

  File license.txt
  File whatsnew.txt

SectionEnd

Section "Associate with PSS files"
  WriteRegStr HKCR ".pss" "" "pssfile"
  WriteRegStr HKCR "pssfile" "" "PathSync Settings file"
  WriteRegStr HKCR "pssfile\DefaultIcon" "" "$INSTDIR\pathsync.exe,0"
  WriteRegStr HKCR "pssfile\shell\open\command" "" '"$INSTDIR\pathsync.exe" -loadpss "%1"' 
SectionEnd

Section "Desktop Icon"
  CreateShortcut "$DESKTOP\PathSync.lnk" "$INSTDIR\pathsync.exe"
SectionEnd

Section "Start Menu Shortcuts"

  SetOutPath $SMPROGRAMS\Pathsync
  CreateShortcut "$OUTDIR\PathSync.lnk" "$INSTDIR\pathsync.exe"
  CreateShortcut "$OUTDIR\PathSync License.lnk" "$INSTDIR\license.txt"
  CreateShortcut "$OUTDIR\Whatsnew.txt.lnk" "$INSTDIR\whatsnew.txt"
  CreateShortcut "$OUTDIR\Uninstall PathSync.lnk" "$INSTDIR\uninstall.exe"

  SetOutPath $INSTDIR

SectionEnd



;--------------------------------
;Uninstaller Section

Section "Uninstall"

  DeleteRegKey HKLM "Software\PathSync"
  DeleteRegKey HKCR ".pss"
  DeleteRegKey HKCR "pssfile"

  Delete "$INSTDIR\pathsync.exe"

  Delete "$INSTDIR\pathsync.ini"
  Delete "$INSTDIR\license.txt"
  Delete "$INSTDIR\whatsnew.txt"
  Delete "$INSTDIR\Uninstall.exe"
  Delete "$SMPROGRAMS\PathSync\*.lnk"
  RMDir $SMPROGRAMS\PathSync
  Delete "$DESKTOP\PathSync.lnk"

  RMDir "$INSTDIR"

SectionEnd
