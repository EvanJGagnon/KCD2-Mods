@echo off
rem Builds Theatrical AutoDice (release: out\kcd2_autodice.dll) plus the development build, the offline
rem self-test and the dice simulator (out\dev\).  Requires Visual Studio 2022 (x64 C++ tools).  Set KC_AUTHOR first.
rem Another Visual Studio edition or the Build Tools: set KC_VCVARS to its vcvars64.bat.
setlocal
cd /d "%~dp0"
if "%KC_VCVARS%"=="" set "KC_VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
call "%KC_VCVARS%" >nul || exit /b 1
if "%KC_AUTHOR%"=="" set KC_AUTHOR=unknown
if not exist out\obj mkdir out\obj
if not exist out\dev mkdir out\dev
set CF=/nologo /std:c++17 /EHa /O2 /MT /W3 /DKC_AUTHOR=\"%KC_AUTHOR%\" /Isrc
set LF=/link /DLL user32.lib kernel32.lib
cl %CF% /LD src\autodice.cpp /Foout\obj\ /Feout\kcd2_autodice.dll %LF% || exit /b 1
cl %CF% /DKC_DEVTOOLS /LD src\autodice.cpp /Foout\obj\ /Feout\dev\kcd2_autodice.dll %LF% || exit /b 1
cl /nologo /O2 tools\selftest.cpp /Foout\obj\ /Feout\dev\selftest.exe || exit /b 1
cl /nologo /O2 /EHsc /std:c++17 /Isrc tools\dicesim.cpp /Foout\obj\ /Feout\dev\dicesim.exe || exit /b 1
out\dev\dicesim.exe test || exit /b 1
del /q out\*.exp out\*.lib out\dev\*.exp out\dev\*.lib 2>nul
echo Build OK.
