# FTG Borderless Fix

Borderless window fix for For the Glory: A Europa Universalis Game on Windows.
Current version: **1.2.0**. Build the x86 ddraw.dll from the sources below;
this repository does not include the game, its executable or compiled binaries.

Experimental x86 DirectDraw proxy for two verified FTG builds with original SHA-256:

```text
Newer / Steam installation (4225536 bytes):
4ddc08b92e21b53fdf4f96375417ef5f95785092554c2ec96cb1555480429fa1
Older / GOG installation (4200960 bytes):
8b889c71810ed8f072a990d8f7b0bbae2c0357efbcef37dc88b8319f4affa35c
```

Both report file/product version 1.3.0.0. Compatibility is selected by checked
window-code, mode-flag and six patch signatures, not the version label or file
size alone. Each profile has its own import, resolution and window-mode addresses;
the older cursor correction uses EDX rather than the newer build's ECX.
The selected profile is recorded in FTGWindow.log. Other builds remain unsupported.

The executable on disk stays original. The proxy hooks the game's CreateWindowExA import and applies six drawing/cursor offset patches in process memory only for borderless windowed mode. The original system DirectDraw library is loaded by its full system path.

## Changes

- 2026-11-02 / 1.2.0: add the older GOG executable profile, bounds-checked signature
	selection and tests of both profiles; retain newer-build support and rejection
	of unknown or already patched code. Both actual EXEs passed offline signature
	checks; the user confirmed the older GOG version works on 2026-11-02.
	Compatibility with other executables or systems is not guaranteed.

## Startup Log (Proxy 1.2.0)

The proxy appends UTF-8 records to FTGWindow.log beside ddraw.dll, independently
of the game's current working directory. Each launch starts with a dated,
PID-tagged DLL_PROCESS_ATTACH record containing the proxy version/build,
DLL/EXE paths, executable size, and file/product versions from the EXE's PE
resource. Missing version resources are reported explicitly. Reading the
resource does not load version.dll inside DllMain.

Further records show whether executable signatures matched and the window hook
was installed or rejected, the effective INI setting, created window geometry,
memory-patch results, system DirectDraw path/loading failures, DirectDrawCreate
HRESULT, and DLL unload/shutdown. A fresh DLL_PROCESS_ATTACH proves the proxy
was loaded, even if its window hook was rejected. Old sessions are retained;
check the newest timestamp/PID. Absence of a log is not conclusive if the game
directory is unwritable; file-write failures also go to OutputDebugString.

No new INI settings or changes to the executable are required. Logging errors
do not block the game, and each write closes its file handle.

## Configuration

Place ddraw.dll and FTGWindow.ini beside FTG.exe. The INI is read once per game launch:

```ini
[Window]
Borderless=1
```

- `Borderless=1`: centered borderless window on the primary screen, retaining the selected game resolution.
- `Borderless=0`: original framed window, with no drawing/cursor patches applied.
- `fullscreen = yes` in the game settings bypasses the borderless changes.

Restart the game after changing the INI. Missing INI defaults to borderless; values other than 1 disable it. No scaling, secondary-monitor selection, or black background outside a smaller window is provided.

## Build

Requires Visual Studio Build Tools with MSVC C++ and the Windows SDK.
From an x86 Visual Studio developer command prompt in this directory:

```bat
cl /nologo /std:c++17 /O2 /MT /LD /W4 /WX ddraw_proxy.cpp /link user32.lib /DEF:ddraw_proxy.def /OUT:ddraw.dll
```

The static runtime avoids adding a separate Visual C++ runtime dependency.

One focused test covers both profiles' framed mode, fullscreen bypass, borderless centering, reversible memory patches, mismatched-byte and truncated-image rejection, PE version extraction, forwarding through the built DLL, and new log-session contents:

```bat
cl /nologo /std:c++17 /O2 /MT /W4 /WX /DFTG_WINDOW_PROXY_TEST ddraw_proxy.cpp /Fe:proxy_test.exe /link user32.lib
proxy_test.exe
```

Pass an optional executable path to check its actual version resource without
running it: `proxy_test.exe "path\to\FTG.exe"`.

## Install or Remove

1. Close the game and build ddraw.dll using the command above.
2. Verify that your original, unpatched FTG.exe matches a supported build.
3. If ddraw.dll already exists in the game directory, preserve it and identify
	its purpose before installing. Do not combine this fix with another
	DirectDraw wrapper or overwrite one blindly.
4. Copy the built ddraw.dll beside FTG.exe. Copy FTGWindow.ini only if absent;
	otherwise preserve the existing configuration and check Borderless=1.
5. Enable windowed mode (fullscreen = no in the game's settings.txt), preserving
	your resolution and other settings. Restart the game.
6. Check the newest session in FTGWindow.log for the matched executable profile.

No executable patching or EXE replacement is required. Use a game directory
writable by your user.

To remove the proxy, close FTG and remove only the installed ddraw.dll and FTGWindow.ini from the game directory. The EXE is already original. With fullscreen still set to no, the game returns to its original framed window.

Do not combine this DLL with an on-disk EXE patch or another DirectDraw wrapper.
Unsupported builds or already patched executables leave the window hook disabled.
On your system, verify mouse alignment, map scrolling and Alt+Tab in FTG.

## License

This fix is licensed under the GNU General Public License, version 3 only
(SPDX: GPL-3.0-only). See [LICENSE](LICENSE) for the full terms.
Redistributed modified versions must comply with GPLv3, including its applicable
source-code obligations. The software is provided without warranty.

For the Glory and its game assets are not included or licensed by this project.