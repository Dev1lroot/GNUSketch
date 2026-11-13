; GNUSketch Windows installer (NSIS). Built by CI:
;   makensis -DVERSION=0.3.0 -DSRCDIR=<unpacked package> -DOUTFILE=<setup.exe> installer.nsi
Unicode true
!include "MUI2.nsh"

Name "GNUSketch ${VERSION}"
OutFile "${OUTFILE}"
InstallDir "$PROGRAMFILES64\GNUSketch"
InstallDirRegKey HKLM "Software\GNUSketch" "InstallDir"
RequestExecutionLevel admin
SetCompressor /SOLID lzma

!define MUI_ICON "${SRCDIR}\gnusketch.ico"
!define MUI_UNICON "${SRCDIR}\gnusketch.ico"
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${SRCDIR}\LICENSE"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN "$INSTDIR\bin\gnusketch.exe"
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Section "GNUSketch" SecMain
  SectionIn RO
  SetOutPath "$INSTDIR"
  File /r "${SRCDIR}\*.*"
  WriteRegStr HKLM "Software\GNUSketch" "InstallDir" "$INSTDIR"
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  CreateDirectory "$SMPROGRAMS\GNUSketch"
  CreateShortcut "$SMPROGRAMS\GNUSketch\GNUSketch.lnk" "$INSTDIR\bin\gnusketch.exe" "" "$INSTDIR\gnusketch.ico"
  CreateShortcut "$SMPROGRAMS\GNUSketch\Uninstall GNUSketch.lnk" "$INSTDIR\Uninstall.exe"
  CreateShortcut "$DESKTOP\GNUSketch.lnk" "$INSTDIR\bin\gnusketch.exe" "" "$INSTDIR\gnusketch.ico"
  ; .gsketch files open in GNUSketch
  WriteRegStr HKCR ".gsketch" "" "GNUSketch.Model"
  WriteRegStr HKCR "GNUSketch.Model" "" "GNUSketch Model"
  WriteRegStr HKCR "GNUSketch.Model\DefaultIcon" "" "$INSTDIR\gnusketch.ico"
  WriteRegStr HKCR "GNUSketch.Model\shell\open\command" "" '"$INSTDIR\bin\gnusketch.exe" "%1"'
  ; Apps & features
  !define UNINST "Software\Microsoft\Windows\CurrentVersion\Uninstall\GNUSketch"
  WriteRegStr HKLM "${UNINST}" "DisplayName" "GNUSketch"
  WriteRegStr HKLM "${UNINST}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINST}" "Publisher" "GNUSketch contributors"
  WriteRegStr HKLM "${UNINST}" "DisplayIcon" "$INSTDIR\gnusketch.ico"
  WriteRegStr HKLM "${UNINST}" "URLInfoAbout" "https://github.com/Dev1lroot/GNUSketch"
  WriteRegStr HKLM "${UNINST}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegDWORD HKLM "${UNINST}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINST}" "NoRepair" 1
SectionEnd

Section "Uninstall"
  Delete "$DESKTOP\GNUSketch.lnk"
  RMDir /r "$SMPROGRAMS\GNUSketch"
  DeleteRegKey HKCR ".gsketch"
  DeleteRegKey HKCR "GNUSketch.Model"
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\GNUSketch"
  DeleteRegKey HKLM "Software\GNUSketch"
  RMDir /r "$INSTDIR"
SectionEnd
