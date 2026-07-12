$ErrorActionPreference = 'Stop'
$MSVC = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207'
$SDK = 'C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0'
$SDKLIB = 'C:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0'
$HOSTX64 = "$MSVC\bin\Hostx64\x64"
$env:PATH = "$HOSTX64;$env:PATH"
$env:INCLUDE = "$MSVC\include;$SDK\ucrt;$SDK\um;$SDK\shared"
$env:LIB = "$MSVC\lib\x64;$SDKLIB\ucrt\x64;$SDKLIB\um\x64"

Write-Host '=== Building SonicCustomBGM.dll ===' -ForegroundColor Cyan
& "$HOSTX64\cl.exe" /EHsc /LD /std:c++17 /MD /O2 /I include /DUSE_DEFAULT_STDLIB 'src\dll\dllmain.cpp' /link /OUT:SonicCustomBGM.dll user32.lib psapi.lib advapi32.lib 'lib\libMinHook-x64-v141-md.lib'
if ($LASTEXITCODE -ne 0) { Write-Host 'DLL build FAILED' -ForegroundColor Red; exit 1 }

Write-Host '=== Building SonicCustomBGM.exe ===' -ForegroundColor Cyan
$aacFiles = Get-ChildItem 'libhelix-aac\*.c' | ForEach-Object { $_.FullName }
$imguiFiles = @(
    'lib\imgui\imgui.cpp',
    'lib\imgui\imgui_draw.cpp',
    'lib\imgui\imgui_tables.cpp',
    'lib\imgui\imgui_widgets.cpp',
    'lib\imgui\backends\imgui_impl_win32.cpp',
    'lib\imgui\backends\imgui_impl_dx9.cpp'
)
$rc = 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\rc.exe'
& $rc /nologo 'app.rc'
if ($LASTEXITCODE -ne 0) { Write-Host 'RC build FAILED' -ForegroundColor Red; exit 1 }
& "$HOSTX64\cl.exe" /EHsc /std:c++17 /MD /O2 /DUSE_DEFAULT_STDLIB /I include /I libhelix-aac /I lib\imgui /I lib\imgui\backends /I lib\SDL2 /I src\common /I shared 'src\exe\main.cpp' $imguiFiles $aacFiles 'app.res' /link /OUT:SonicCustomBGM.exe user32.lib psapi.lib advapi32.lib gdi32.lib comdlg32.lib comctl32.lib xaudio2.lib ole32.lib d3d9.lib 'lib\SDL2\lib\x64\SDL2.lib'
if ($LASTEXITCODE -ne 0) { Write-Host 'EXE build FAILED' -ForegroundColor Red; exit 1 }

Write-Host '=== Build complete ===' -ForegroundColor Green
Get-ChildItem 'SonicCustomBGM.dll','SonicCustomBGM.exe' | Select-Object Name, @{N='KB';E={[math]::Round($_.Length/1024,1)}}
