; Grouwdi-OBS-FrameGen installer (NSIS 3, Unicode)
; Installs the plugin into C:\ProgramData\obs-studio\plugins\Grouwdi-OBS-FrameGen,
; the per-machine plugin folder OBS Studio 28 and newer loads plugins from.
; Build: makensis -DSTAGE=<folder with the plugin files> -DOUTFILE=<setup.exe> installer.nsi

Unicode true
!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "x64.nsh"

!define PRODUCT     "Grouwdi-OBS-FrameGen"
!define VERSION     "1.0.0"
!define PUBLISHER   "Grouwdi"
!define UNINST_KEY  "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT}"
!ifndef STAGE
  !error "pass -DSTAGE=<folder>"
!endif
!ifndef OUTFILE
  !define OUTFILE "${PRODUCT}-${VERSION}-Setup.exe"
!endif

Name "${PRODUCT} ${VERSION}"
OutFile "${OUTFILE}"
RequestExecutionLevel admin
SetCompressor /SOLID lzma
ShowInstDetails show
ShowUninstDetails show
BrandingText "${PRODUCT} ${VERSION}"

VIProductVersion "1.0.0.0"
VIAddVersionKey "ProductName" "${PRODUCT}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "FileDescription" "${PRODUCT} installer"
VIAddVersionKey "CompanyName" "${PUBLISHER}"
VIAddVersionKey "LegalCopyright" "(C) 2026 ${PUBLISHER}. GPL-2.0-or-later."

!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TITLE "${PRODUCT} ${VERSION}"
!define MUI_WELCOMEPAGE_TEXT "This installs the ${PRODUCT} plugin for OBS Studio.$\r$\n$\r$\nIt adds two filters for capture card sources: Motion Smoother (NVIDIA), which fills repeated frames with real motion, and Capture Smoother, which reports capture timing.$\r$\n$\r$\nPlease close OBS Studio before continuing."
!define MUI_FINISHPAGE_TITLE "Installed"
!define MUI_FINISHPAGE_TEXT "Start OBS Studio, right-click your capture card source > Filters > Effect Filters > + > Motion Smoother (NVIDIA).$\r$\n$\r$\nRecommended: 'Fill missing frames only', Video delay 3, and +50 ms audio Sync Offset on the capture source."
!define MUI_FINISHPAGE_SHOWREADME "$INSTDIR\README.txt"
!define MUI_FINISHPAGE_SHOWREADME_TEXT "Open the README"
!define MUI_FINISHPAGE_SHOWREADME_NOTCHECKED

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${STAGE}\LICENSE.txt"
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

; Ask until OBS is closed (or the user cancels). $0 = 0 means obs64.exe is running.
!macro WAIT_FOR_OBS_CLOSED UN
Function ${UN}WaitForObs
  check:
    nsExec::ExecToStack '"$SYSDIR\cmd.exe" /c tasklist /FI "IMAGENAME eq obs64.exe" /NH | "$SYSDIR\find.exe" /I "obs64.exe"'
    Pop $0
    Pop $1
    ${If} $0 == 0
      MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "OBS Studio is running. Close OBS, then click Retry." IDRETRY check
      Abort
    ${EndIf}
FunctionEnd
!macroend
!insertmacro WAIT_FOR_OBS_CLOSED ""
!insertmacro WAIT_FOR_OBS_CLOSED "un."

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_OK|MB_ICONSTOP "${PRODUCT} needs 64-bit Windows."
    Abort
  ${EndIf}
  SetShellVarContext all
  SetRegView 64
  StrCpy $INSTDIR "$APPDATA\obs-studio\plugins\${PRODUCT}"
FunctionEnd

Section "Plugin" SecMain
  SectionIn RO
  Call WaitForObs

  ; Earlier builds used the folder names "capture-smoother" (test builds) and
  ; "Grouwdi-OBS-CaptureSmoother" (1.0.0 under its previous name) with the same filter IDs;
  ; OBS cannot load two of them, so the old ones are removed. Settings live in the scene
  ; files and are kept.
  ${If} ${FileExists} "$APPDATA\obs-studio\plugins\capture-smoother\*.*"
    DetailPrint "Removing the earlier capture-smoother test build"
    RMDir /r "$APPDATA\obs-studio\plugins\capture-smoother"
  ${EndIf}
  ${If} ${FileExists} "$APPDATA\obs-studio\plugins\Grouwdi-OBS-CaptureSmoother\*.*"
    DetailPrint "Removing Grouwdi-OBS-CaptureSmoother (this plugin's previous name)"
    RMDir /r "$APPDATA\obs-studio\plugins\Grouwdi-OBS-CaptureSmoother"
  ${EndIf}
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\Grouwdi-OBS-CaptureSmoother"

  ; replace any previous version of this plugin cleanly
  RMDir /r "$INSTDIR\bin"
  RMDir /r "$INSTDIR\data"

  SetOutPath "$INSTDIR\bin\64bit"
  File "${STAGE}\${PRODUCT}\bin\64bit\${PRODUCT}.dll"
  SetOutPath "$INSTDIR\data"
  File "${STAGE}\${PRODUCT}\data\motion-smoother.effect"
  SetOutPath "$INSTDIR\data\locale"
  File "${STAGE}\${PRODUCT}\data\locale\en-US.ini"
  SetOutPath "$INSTDIR"
  File "${STAGE}\README.txt"
  File "${STAGE}\LICENSE.txt"
  File "${STAGE}\THIRD-PARTY-NOTICES.txt"

  WriteUninstaller "$INSTDIR\uninstall.exe"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayName" "${PRODUCT} (OBS plugin)"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINST_KEY}" "Publisher" "${PUBLISHER}"
  WriteRegStr HKLM "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINST_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr HKLM "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoRepair" 1
  WriteRegDWORD HKLM "${UNINST_KEY}" "EstimatedSize" 200
SectionEnd

Function un.onInit
  SetShellVarContext all
  SetRegView 64
FunctionEnd

Section "Uninstall"
  Call un.WaitForObs
  Delete "$INSTDIR\uninstall.exe"
  Delete "$INSTDIR\README.txt"
  Delete "$INSTDIR\LICENSE.txt"
  Delete "$INSTDIR\THIRD-PARTY-NOTICES.txt"
  RMDir /r "$INSTDIR\bin"
  RMDir /r "$INSTDIR\data"
  RMDir "$INSTDIR"
  DeleteRegKey HKLM "${UNINST_KEY}"
SectionEnd
