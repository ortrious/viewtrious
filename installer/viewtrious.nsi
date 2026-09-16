Unicode true
RequestExecutionLevel user
!include "MUI2.nsh"

!ifndef PRODUCT_VERSION
  !define PRODUCT_VERSION "0.9.40.0"
!endif
!ifndef RELEASE_DIR
  !error "RELEASE_DIR must name the Release artifact directory."
!endif
!ifndef SOURCE_DIR
  !error "SOURCE_DIR must name the repository root."
!endif

Name "viewtrious"
OutFile "${SOURCE_DIR}\out\installer\viewtrious-setup-${PRODUCT_VERSION}.exe"
InstallDir "$LOCALAPPDATA\viewtrious"
ShowInstDetails show
ShowUninstDetails show

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${SOURCE_DIR}\LICENSE"
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Section "Install"
  SetOutPath "$INSTDIR\app\shellextensions"
  ClearErrors
  File "${RELEASE_DIR}\ViewtriousStlThumbnail.dll"
  IfErrors 0 +3
    MessageBox MB_ICONSTOP "The Viewtrious Explorer thumbnail extension is in use. Close File Explorer windows and retry."
    Abort
  SetOutPath "$INSTDIR\app"
  ClearErrors
  File "${RELEASE_DIR}\Viewtrious.exe"
  IfErrors 0 +3
    MessageBox MB_ICONSTOP "Viewtrious.exe could not be updated. Close viewtrious and retry."
    Abort
  SetOutPath "$INSTDIR\app\addons"
  SetOutPath "$INSTDIR\app\licenses"
  File /oname=viewtrious.txt "${SOURCE_DIR}\LICENSE"
  File /oname=miniz.txt "${SOURCE_DIR}\app\licenses\miniz.txt"
  File /oname=notice.txt "${SOURCE_DIR}\NOTICE"
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  CreateShortcut "$SMPROGRAMS\viewtrious.lnk" "$INSTDIR\app\Viewtrious.exe" "" "$INSTDIR\app\Viewtrious.exe" 0 "$INSTDIR\app"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\viewtrious" "DisplayName" "viewtrious"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\viewtrious" "DisplayVersion" "${PRODUCT_VERSION}"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\viewtrious" "Publisher" "Ortrious"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\viewtrious" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\viewtrious" "DisplayIcon" "$INSTDIR\app\Viewtrious.exe"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\viewtrious" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\viewtrious" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\viewtrious" "NoModify" 1
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\viewtrious" "NoRepair" 1
  ExecWait '"$INSTDIR\app\Viewtrious.exe" --register-integration' $0
  IntCmp $0 0 +2
    Abort "Viewtrious could not register Windows integration (exit $0)."
SectionEnd

Section "Uninstall"
  ExecWait '"$INSTDIR\app\Viewtrious.exe" --unregister-integration' $0
  Delete "$SMPROGRAMS\viewtrious.lnk"
  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\viewtrious"
  Delete "$INSTDIR\app\shellextensions\ViewtriousStlThumbnail.dll"
  Delete "$INSTDIR\app\Viewtrious.exe"
  Delete "$INSTDIR\app\licenses\viewtrious.txt"
  Delete "$INSTDIR\app\licenses\miniz.txt"
  Delete "$INSTDIR\app\licenses\notice.txt"
  RMDir "$INSTDIR\app\licenses"
  RMDir "$INSTDIR\app\addons"
  RMDir "$INSTDIR\app\shellextensions"
  RMDir "$INSTDIR\app"
  ; Preserve a possibly active wallpaper rather than deleting Windows' live source file.
  IfFileExists "$INSTDIR\data\wallpaper\current.bmp" preserve_wallpaper
  RMDir /r "$INSTDIR\data"
  Goto remove_root
preserve_wallpaper:
  ; Keep the containing data directory so Windows can continue reading current.bmp.
remove_root:
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"
SectionEnd
