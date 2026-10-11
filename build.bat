@echo off
rem Builds the mods (release: out\) plus development builds and test tools (out\dev\).
rem Requires Visual Studio 2022 (x64 C++ tools).  Set KC_AUTHOR first, e.g.  set KC_AUTHOR=YourName
rem Another Visual Studio edition or the Build Tools: set KC_VCVARS to its vcvars64.bat (the CI build does).
setlocal
cd /d "%~dp0"
if "%KC_VCVARS%"=="" set "KC_VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
call "%KC_VCVARS%" >nul || exit /b 1
if "%KC_AUTHOR%"=="" set KC_AUTHOR=unknown
if not exist out\obj mkdir out\obj
if not exist out\dev mkdir out\dev
set CF=/nologo /std:c++17 /EHa /O2 /MT /W3 /DKC_AUTHOR=\"%KC_AUTHOR%\" /Isrc
set LF=/link /DLL user32.lib kernel32.lib
for %%m in (autoforge alchemy autotravel autosharpen) do (
  cl %CF% /LD src\%%m.cpp /Foout\obj\ /Feout\kcd2_%%m.dll %LF% || exit /b 1
  cl %CF% /DKC_DEVTOOLS /LD src\%%m.cpp /Foout\obj\ /Feout\dev\kcd2_%%m.dll %LF% || exit /b 1
)
cl %CF% /LD src\hardcore_markers.cpp /Foout\obj\ /Feout\kcd2_hardcore_markers.dll %LF% || exit /b 1
cl /nologo /O2 tools\selftest.cpp /Foout\obj\ /Feout\dev\selftest.exe || exit /b 1
cl /nologo /O2 tools\routetest.cpp /Foout\obj\ /Feout\dev\routetest.exe || exit /b 1
cl /nologo /O2 tools\markertest.cpp /Foout\obj\ /Feout\dev\kc_markertest.exe || exit /b 1
cl /nologo /O2 /EHsc tools\sharpsim.cpp /Foout\obj\ /Feout\dev\sharpsim.exe || exit /b 1
cl /nologo /O2 /EHsc tools\forgesim.cpp /Foout\obj\ /Feout\dev\forgesim.exe || exit /b 1
del /q out\*.exp out\*.lib out\dev\*.exp out\dev\*.lib 2>nul
echo Build OK.
