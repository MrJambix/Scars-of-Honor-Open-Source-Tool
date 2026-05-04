# ScarsTool

Unity IL2CPP injector + payload DLL targeting **Scars of Honor.exe**.

Modeled after EthyTool's injector. Two projects:

| Project | Output | Role |
|---|---|---|
| `ScarsToolInjector` | `injector_exe/ScarsToolInjector.exe` | Console app. Scans for the game process, waits for `GameAssembly.dll` (IL2CPP runtime), then `LoadLibraryW`-injects the payload via `CreateRemoteThread`. Requires admin (manifested). |
| `ScarsTool` | `injector_exe/ScarsTool.dll` | Payload. Spawns a debug console inside the game, locates `GameAssembly.dll` / `UnityPlayer.dll`, resolves common `il2cpp_*` exports, then idles. Add your hooks in `PayloadThread()`. |

Game install (reference):
`C:\Program Files (x86)\Steam\steamapps\common\Scars of Honor Playtest\Scars of Honor.exe`

## Build

Open `ScarsTool.sln` in Visual Studio 2022 (v143 / Win10 SDK) and build `Release | x64`.
The DLL drops next to the EXE automatically. The injector depends on the DLL project, so building the injector builds both.

Or from a Developer PowerShell:

```powershell
msbuild ScarsTool.sln /p:Configuration=Release /p:Platform=x64
```

## Run

1. Build Release|x64.
2. Launch `Scars of Honor.exe` (or run the injector first; it will wait up to 2 minutes for the process).
3. Run `injector_exe\ScarsToolInjector.exe` as admin.
4. A second console titled "ScarsTool - Payload Console" will appear once the DLL loads inside the game.

## Customizing for another game

All target-specific strings live at the top of `injector_exe/main.cpp`:

```cpp
static const wchar_t* kProcessSubstr = L"scars of honor";
static const wchar_t* kFriendlyName  = L"Scars of Honor";
static const wchar_t* kPayloadDll    = L"ScarsTool.dll";
static const wchar_t* kPayloadModule = L"scarstool";
static const wchar_t* kIl2cppModule  = L"gameassembly";
```
