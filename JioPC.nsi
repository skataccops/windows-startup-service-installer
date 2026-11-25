; =============================================================================
;  JioPC PWA installer  -  NSIS (MUI2), no Launcher.exe
;  Build:  makensis JioPC.nsi   (needs JioPCSvc.exe next to this file; eula.txt optional)
;
;  Admin privileges   : installs the Windows service (JioPCSvc.exe). The service runs
;                       the LaunchTarget/LaunchArgs command below in each user's session
;                       (non-elevated) at boot, logon, user switch and resume.
;  No admin privileges: only a Startup shortcut to the same command.
; =============================================================================

; ------------------------------- DEFINES -------------------------------------
!define APP_NAME      "JioPC"
!define APP_VERSION   "1.0.0"
!define APP_PUBLISHER "Reliance Jio Infocomm Ltd"
!define APP_URL       "https://my.jiopc.in/"      ; force-installed through browser policy (admin path)
!define SVC_EXE       "JioPCSvc.exe"              ; built from JioPCSvc.cpp, must sit next to this file
!define CHROME_REL    "Google\Chrome\Application\chrome_proxy.exe"
!define CHROME_ARGS   '--profile-directory="Profile 1" --app-id=hddihfeenjkjdfhhidapmhkhnhmapanm'

Unicode true
!include "MUI2.nsh"
!include "x64.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"

Name "${APP_NAME}"
OutFile "${APP_NAME}-Setup.exe"
InstallDir "$PROGRAMFILES32\${APP_NAME}"          ; hardcoded Program Files (x86)\JioPC
InstallDirRegKey HKLM "Software\${APP_NAME}" "InstallDir"
RequestExecutionLevel admin
BrandingText "${APP_NAME} ${APP_VERSION}"
SetCompressor /SOLID lzma

; ---- Build-time prerequisites
; eula.txt is generated with placeholder text if you did not supply one.
!if /FileExists "eula.txt"
!else
  !appendfile "eula.txt" "END USER LICENSE AGREEMENT$\r$\n"
  !appendfile "eula.txt" "Replace this text with your own license terms.$\r$\n"
  !appendfile "eula.txt" "By clicking I Agree you accept these terms and consent to installation of ${APP_NAME} and the browser policy entries it requires.$\r$\n"
  !warning "eula.txt was missing - a placeholder was generated. Replace it with your real EULA."
!endif
; The service binary cannot be generated here: build it from JioPCSvc.cpp first.
!if /FileExists "${SVC_EXE}"
!else
  !error "${SVC_EXE} not found next to this script. Build it from JioPCSvc.cpp first (see the header of that file)."
!endif

; ---- Only four pages: Welcome, EULA, Progress, Finish
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "eula.txt"
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

; ---------------------------------------------------------------------------
Function .onInit
  SetRegView 64
  SetShellVarContext all                          ; install for ALL users
FunctionEnd

; ---------------------------------------------------------------------------
; DetectInvokerType -> $0 = "Admin" or "User"
; Setup is always elevated, so the plugin's account type is always "Admin" even if a
; standard user typed an admin password. Instead, look at who is signed in at the
; console (LogonUI LastLoggedOnSAMUser) and check whether THAT user is a member of the
; local Administrators group (found by SID S-1-5-32-544, so it works in any language).
; Any failure or doubt falls back to "Admin" (the service install path).
Function DetectInvokerType
  Push $1
  Push $2
  Push $R0
  Push $R1
  Push $R2
  Push $R3
  Push $R6
  Push $R7
  Push $R9
  StrCpy $0 "Admin"

  ReadRegStr $R0 HKLM "SOFTWARE\Microsoft\Windows\CurrentVersion\Authentication\LogonUI" "LastLoggedOnSAMUser"
  StrCmp $R0 "" detect_done
  StrCpy $R1 $R0 2
  StrCmp $R1 ".\" 0 +2
    StrCpy $R0 $R0 "" 2                        ; ".\bob" -> "bob"

  ; Name of the Administrators group from its well-known SID
  System::Call 'advapi32::ConvertStringSidToSidW(w "S-1-5-32-544", *p .R1) i .R2'
  StrCmp $R2 0 detect_done
  System::Call 'advapi32::LookupAccountSidW(p 0, p R1, w .R3, *i ${NSIS_MAX_STRLEN}, w .r1, *i ${NSIS_MAX_STRLEN}, *i .r2) i .R2'
  System::Call 'kernel32::LocalFree(p R1)'
  StrCmp $R2 0 detect_done

  ; Local groups of that user, including nested/indirect membership
  System::Call 'netapi32::NetUserGetLocalGroups(p 0, w R0, i 0, i 1, *p .R6, i -1, *i .R7, *i .r2) i .R2'
  StrCmp $R2 0 +2
    Goto detect_done                           ; lookup failed -> keep "Admin"

  StrCpy $0 "User"                             ; lookup worked: user unless Administrators is found
  StrCpy $R9 0
detect_loop:
  IntCmp $R9 $R7 detect_free 0 detect_free
  IntOp $1 $R9 * ${NSIS_PTR_SIZE}
  IntOp $1 $R6 + $1
  System::Call '*$1(w .r2)'
  StrCmp $2 $R3 0 +3
    StrCpy $0 "Admin"
    Goto detect_free
  IntOp $R9 $R9 + 1
  Goto detect_loop
detect_free:
  System::Call 'netapi32::NetApiBufferFree(p R6)'
detect_done:
  Pop $R9
  Pop $R7
  Pop $R6
  Pop $R3
  Pop $R2
  Pop $R1
  Pop $R0
  Pop $2
  Pop $1
FunctionEnd

; ---------------------------------------------------------------------------
; ChromeTarget -> $0 = full path of chrome_proxy.exe (64-bit Program Files first,
; then Program Files (x86); defaults to the 64-bit path if neither exists)
Function ChromeTarget
  StrCpy $0 "$PROGRAMFILES64\${CHROME_REL}"
  IfFileExists "$0" +4 0
    StrCpy $0 "$PROGRAMFILES32\${CHROME_REL}"
    IfFileExists "$0" +2 0
      StrCpy $0 "$PROGRAMFILES64\${CHROME_REL}"
FunctionEnd

; ---------------------------------------------------------------------------
Section "Install"
  SetShellVarContext all
  SetOutPath "$INSTDIR"
  Call DetectInvokerType                          ; $0 = "Admin" or "User"
  StrCpy $2 $0                                    ; keep the result
  Call ChromeTarget                               ; $0 = chrome_proxy.exe path

  ${If} $2 == "Admin"
    ; ===== Admin privileges: install the Windows service =====
    ; Re-install/upgrade: remove a running copy first so its exe is not locked.
    IfFileExists "$INSTDIR\${SVC_EXE}" 0 +2
      ExecWait '"$INSTDIR\${SVC_EXE}" --uninstall'

    File "${SVC_EXE}"

    ; What the service launches in each user's session
    SetRegView 64
    WriteRegStr HKLM "Software\${APP_NAME}" "LaunchTarget" "$0"
    WriteRegStr HKLM "Software\${APP_NAME}" "LaunchArgs" '${CHROME_ARGS}'

    ; Silent PWA provisioning via browser enterprise policy (all users)
    ;WriteRegStr HKLM "SOFTWARE\Policies\Google\Chrome\WebAppInstallForceList" "1" \
      '{"url":"${APP_URL}","default_launch_container":"window","create_desktop_shortcut":false}'
    ;WriteRegStr HKLM "SOFTWARE\Policies\Microsoft\Edge\WebAppInstallForceList" "1" \
      '{"url":"${APP_URL}","default_launch_container":"window","create_desktop_shortcut":false}'

    ; Start Menu entry for all users
    CreateDirectory "$SMPROGRAMS\${APP_NAME}"
    CreateShortCut "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk" "$0" '${CHROME_ARGS}'

    ; Install + start the service. Setup starts it with --no-initial, so finishing the
    ; install does not launch the app; it opens on the next logon/switch/resume/boot.
    ExecWait '"$INSTDIR\${SVC_EXE}" --install' $1
    ${If} $1 != 0
      MessageBox MB_ICONSTOP|MB_OK "The ${APP_NAME} service could not be installed (code $1)." /SD IDOK
      Abort
    ${EndIf}
  ${Else}
    ; ===== No admin privileges: only a Startup shortcut =====
    ; All Users Startup folder, because this elevated process cannot reach the
    ; standard user's own profile. No service, no policy.
    CreateShortCut "$SMSTARTUP\${APP_NAME}.lnk" "$0" '${CHROME_ARGS}'
  ${EndIf}

  ; Uninstaller + Add/Remove Programs entry for both paths
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  WriteRegStr   HKLM "Software\${APP_NAME}" "InstallDir" "$INSTDIR"
  WriteRegStr   HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_NAME}" "DisplayName" "${APP_NAME}"
  WriteRegStr   HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_NAME}" "DisplayVersion" "${APP_VERSION}"
  WriteRegStr   HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_NAME}" "Publisher" "${APP_PUBLISHER}"
  WriteRegStr   HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_NAME}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_NAME}" "NoModify" 1
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_NAME}" "NoRepair" 1
SectionEnd

; ---------------------------------------------------------------------------
Section "Uninstall"
  SetShellVarContext all
  SetRegView 64
  IfFileExists "$INSTDIR\${SVC_EXE}" 0 +2
    ExecWait '"$INSTDIR\${SVC_EXE}" --uninstall'
  Delete "$INSTDIR\${SVC_EXE}"
  Delete "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk"
  RMDir  "$SMPROGRAMS\${APP_NAME}"
  Delete "$SMSTARTUP\${APP_NAME}.lnk"
  SetShellVarContext current
  Delete "$SMSTARTUP\${APP_NAME}.lnk"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir  "$INSTDIR"
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_NAME}"
  DeleteRegKey HKLM "Software\${APP_NAME}"
SectionEnd
