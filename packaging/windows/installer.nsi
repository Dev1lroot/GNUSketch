; Forma Windows installer (NSIS). Built by CI:
;   makensis -DVERSION=0.3.0 -DSRCDIR=<unpacked package> -DOUTFILE=<setup.exe> installer.nsi
Unicode true
!include "MUI2.nsh"

Name "Forma ${VERSION}"
OutFile "${OUTFILE}"
InstallDir "$PROGRAMFILES64\Forma"
InstallDirRegKey HKLM "Software\Forma" "InstallDir"
RequestExecutionLevel admin
SetCompressor /SOLID lzma

!define MUI_ICON "${SRCDIR}\forma.ico"
!define MUI_UNICON "${SRCDIR}\forma.ico"
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${SRCDIR}\LICENSE"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN "$INSTDIR\bin\forma.exe"
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Section "Forma" SecMain
  SectionIn RO
  SetOutPath "$INSTDIR"
  File /r "${SRCDIR}\*.*"
  WriteRegStr HKLM "Software\Forma" "InstallDir" "$INSTDIR"
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  CreateDirectory "$SMPROGRAMS\Forma"
  CreateShortcut "$SMPROGRAMS\Forma\Forma.lnk" "$INSTDIR\bin\forma.exe" "" "$INSTDIR\forma.ico"
  CreateShortcut "$SMPROGRAMS\Forma\Uninstall Forma.lnk" "$INSTDIR\Uninstall.exe"
  CreateShortcut "$DESKTOP\Forma.lnk" "$INSTDIR\bin\forma.exe" "" "$INSTDIR\forma.ico"
  ; .forma files (and .gsketch, from before the rename) open in Forma
  WriteRegStr HKCR ".forma" "" "Forma.Model"
  WriteRegStr HKCR ".gsketch" "" "Forma.Model"
  WriteRegStr HKCR "Forma.Model" "" "Forma Model"
  WriteRegStr HKCR "Forma.Model\DefaultIcon" "" "$INSTDIR\forma.ico"
  WriteRegStr HKCR "Forma.Model\shell\open\command" "" '"$INSTDIR\bin\forma.exe" "%1"'
  ; Apps & features
  !define UNINST "Software\Microsoft\Windows\CurrentVersion\Uninstall\Forma"
  WriteRegStr HKLM "${UNINST}" "DisplayName" "Dev1lroot Software: Forma"
  WriteRegStr HKLM "${UNINST}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINST}" "Publisher" "Dev1lroot Software"
  WriteRegStr HKLM "${UNINST}" "DisplayIcon" "$INSTDIR\forma.ico"
  WriteRegStr HKLM "${UNINST}" "URLInfoAbout" "https://github.com/Dev1lroot/Forma"
  WriteRegStr HKLM "${UNINST}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegDWORD HKLM "${UNINST}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINST}" "NoRepair" 1
SectionEnd

Section "Uninstall"
  Delete "$DESKTOP\Forma.lnk"
  RMDir /r "$SMPROGRAMS\Forma"
  DeleteRegKey HKCR ".forma"
  DeleteRegKey HKCR ".gsketch"
  DeleteRegKey HKCR "Forma.Model"
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Forma"
  DeleteRegKey HKLM "Software\Forma"
  RMDir /r "$INSTDIR"
SectionEnd
