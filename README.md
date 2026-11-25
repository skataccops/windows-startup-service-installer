# JioPC Installer

NSIS installer plus a Windows service that opens the JioPC PWA in Chrome.

## Files
- `JioPC.nsi` - installer script
- `JioPCSvc.cpp` - launch service source
- `eula.txt` - license text (placeholder generated if missing)

## Prerequisites (to compile)
- **NSIS 3.x** - install from https://nsis.sourceforge.io/Download (default options; the build uses the bundled MUI2, System and UserInfo plugins).
- **A C++ compiler for the service** - Visual Studio Build Tools (MSVC) or MinGW-w64.

## Change before you build: hardcoded browser target
The shortcut and the service launch a **hardcoded** Chrome command. The sample values (`Profile 1` and the app id `hddihfeenjkjdfhhidapmhkhnhmapanm`) must be replaced with your real ones, or the app will not open.

| Value | Where to change it |

| Chrome path (`C:\Program Files\Google\Chrome\Application\chrome_proxy.exe`) | `LAUNCH_COMMAND` in `JioPCSvc.cpp`; `CHROME_REL` in `JioPC.nsi` |
| Profile (`--profile-directory="Profile 1"`) | `LAUNCH_COMMAND` in `JioPCSvc.cpp`; `CHROME_ARGS` in `JioPC.nsi` |
| App id (`--app-id=...`) | `LAUNCH_COMMAND` in `JioPCSvc.cpp`; `CHROME_ARGS` in `JioPC.nsi` |

Change **both files** so the shortcut and the service match, then rebuild.

How to find the values:
- **App id:** install the PWA once in Chrome, open `chrome://apps`, right-click the app > *App info*, or open `chrome://web-app-internals`.
- **Profile folder:** open `chrome://version` in that profile and read *Profile Path*; use its last folder name (e.g. `Default`, `Profile 1`).
- **Chrome path:** the install folder of `chrome_proxy.exe` (`Program Files` or `Program Files (x86)`).

## Build
1. Compile the service (MSVC Developer Command Prompt):
   `cl /nologo /EHsc /O2 /DUNICODE /D_UNICODE JioPCSvc.cpp /link /SUBSYSTEM:WINDOWS advapi32.lib wtsapi32.lib userenv.lib`
2. Keep `JioPCSvc.exe` next to `JioPC.nsi`.
3. Compile the installer: right-click `JioPC.nsi` > *Compile NSIS Script*, or run `makensis JioPC.nsi`.

Output: `JioPC-Setup.exe`.

## Behavior
- Installs to `C:\Program Files (x86)\JioPC` for all users (needs administrator).
- **Admin:** installs and starts the `JioPCLaunchSvc` service. It starts the app at user logon with a non-elevated token. Setup does not launch the app.
- **Non-admin:** creates a Startup shortcut only.
- The launched command is set in `LAUNCH_COMMAND` (`JioPCSvc.cpp`) and `CHROME_ARGS` (`JioPC.nsi`). Edit both and rebuild to change it.

## Requirements on the target PC
Windows with Chrome, and the PWA already installed in the profile named in the command.

## Logs and uninstall
- Log: `C:\ProgramData\JioPC\service.log`
- Uninstall: Settings > Apps > JioPC, or run `Uninstall.exe` in the install folder as administrator.
