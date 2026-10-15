@echo off
rem Builds the Barrett M107A1 GML plugin and its in-game test harness.
rem   build.bat prep       (re)generate Assets\ from your export (tools\prep)
rem   build.bat            build into build\
rem   build.bat install    also copy the plugin into the game (refuses while the game runs)
rem
rem Personal use only: the model and textures come from your own export of the WTT-Armory Barrett M107A1
rem (BARRETT_EXPORT, default E:\TARKOVE EXPORTS\BarrettM107\BarrettM107 Files). They are encrypted into
rem Barrett.dll (tools\pack, a new key every build) and are never committed or shared.
rem Needs the GML SDK 2.2+ (..\ModLoader\include, or set GML_SDK), Visual Studio 2022 and the .NET 10 SDK.
setlocal
set ROOT=%~dp0
set OUT=%ROOT%build
set SDK=%ROOT%external\gml\include
if not exist "%SDK%\GML\GML.hpp" set SDK=%ROOT%..\ModLoader\include
if defined GML_SDK set SDK=%GML_SDK%
set GAME=C:\Program Files (x86)\Steam\steamapps\common\GERONIMO\Geronimo\Binaries\Win64
if defined GML_GAME_DIR set GAME=%GML_GAME_DIR%
if not defined BARRETT_EXPORT set BARRETT_EXPORT=E:\TARKOVE EXPORTS\BarrettM107\BarrettM107 Files
set TOOLS=%ROOT%..\Utils
set PAKS=%GAME%\..\..\Content\Paks

for %%D in ("%OUT%\obj" "%OUT%\gen" "%OUT%\Barrett" "%OUT%\tests\BarrettDev") do if not exist "%%~D" mkdir "%%~D"

if /i "%1"=="prep" (
  echo === the KAC M110's mesh, to fit the Barrett on
  if not exist "%OUT%\legacy\Geronimo\Content\Weapons\KAC_SR25\Materials\SR25_M110SASS.uexp" (
    "%TOOLS%\retoc\retoc.exe" to-legacy "%PAKS%" "%OUT%\legacy" --version UE5_7 -f "Content/Weapons/KAC_SR25/Materials/SR25_M110SASS." || exit /b 1
  )
  dotnet "%TOOLS%\UAssetParser\bin\Debug\net10.0\UAssetParser.dll" uexppos "%OUT%\legacy\Geronimo\Content\Weapons\KAC_SR25\Materials\SR25_M110SASS.uexp" "%OUT%\m110_static_pts.obj" || exit /b 1
  echo === Assets\ from "%BARRETT_EXPORT%"
  dotnet build "%ROOT%tools\prep\prep.csproj" -c Release -o "%OUT%\prep" --nologo -v q || exit /b 1
  dotnet "%OUT%\prep\prep.dll" "%BARRETT_EXPORT%" "%ROOT%Assets" "%OUT%\m110_static_pts.obj" "%OUT%\preview" || exit /b 1
  exit /b 0
)

if not exist "%SDK%\GML\GML.hpp" (
  echo GML SDK not found at %SDK% - set GML_SDK
  exit /b 1
)
findstr /c:"ImportDynamicMeshFromMemory" "%SDK%\GML\GML.h" >nul || (
  echo The GML SDK at %SDK% is older than 2.2 - set GML_SDK
  exit /b 1
)
if not exist "%ROOT%Assets\barrett.obj" (
  echo Assets\ is missing - run: build.bat prep
  exit /b 1
)
if not defined VCToolsInstallDir (
  call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
)
set CFLAGS=/nologo /std:c++20 /O2 /MT /EHsc /W3 /Zi /DUNICODE /D_UNICODE /I"%SDK%"
set LFLAGS=/DEBUG /INCREMENTAL:NO

echo === pack (encrypt Assets\ with a new key)
cl /nologo /std:c++20 /O2 /MT /EHsc /Fo"%OUT%\obj\\" "%ROOT%tools\pack\pack.cpp" /Fe"%OUT%\pack.exe" || exit /b 1
"%OUT%\pack.exe" "%ROOT%Assets" "%OUT%\gen\payload.bin" "%OUT%\gen\payload_key.h" || exit /b 1
echo === plugin (encrypted assets embedded)
rem rc looks for the .rc's build\gen\ files in the current directory first: run it from this plugin's folder
pushd "%ROOT%" || exit /b 1
rc /nologo /I "%ROOT%." /fo"%OUT%\obj\Barrett.res" "%ROOT%src\Barrett.rc" || (popd & exit /b 1)
popd
cl %CFLAGS% /I"%OUT%\gen" /LD /Fo"%OUT%\obj\\" /Fd"%OUT%\obj\\" "%ROOT%src\Barrett.cpp" "%OUT%\obj\Barrett.res" /Fe"%OUT%\Barrett\Barrett.dll" /link %LFLAGS% || exit /b 1
echo === test harness (never installed)
cl %CFLAGS% /LD /Fo"%OUT%\obj\\" /Fd"%OUT%\obj\\" "%ROOT%tests\BarrettDev\BarrettDev.cpp" /Fe"%OUT%\tests\BarrettDev\BarrettDev.dll" /link %LFLAGS% || exit /b 1
for /r "%OUT%" %%F in (*.exp *.lib *.ilk) do del /q "%%F"

if /i "%1"=="install" (
  tasklist /FI "IMAGENAME eq Geronimo-Win64-Shipping.exe" | find /I "Geronimo-Win64" >nul && (
    echo Geronimo is running - close it first.
    exit /b 1
  )
  echo === install to "%GAME%\GML\plugins\Barrett"
  if not exist "%GAME%\GML\plugins\Barrett" mkdir "%GAME%\GML\plugins\Barrett"
  copy /y "%OUT%\Barrett\Barrett.dll" "%GAME%\GML\plugins\Barrett\" >nul || exit /b 1
  copy /y "%ROOT%CREDITS.txt" "%GAME%\GML\plugins\Barrett\" >nul || exit /b 1
)
echo === done
exit /b 0
