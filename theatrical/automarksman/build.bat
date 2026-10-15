@echo off
rem Theatrical AutoMarksman: builds out\kcd2_automarksman.dll (release) and out\dev\ (development build with the
rem self-test export, selftest.exe, marksim.exe), then runs the offline ballistics tests.  The release archives are
rem made by the repository's tools\package.py.
rem Requires Visual Studio 2022 (x64 C++ tools).  Another edition or the Build Tools: set KC_VCVARS first.
setlocal
cd /d "%~dp0"
if "%KC_VCVARS%"=="" set "KC_VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
call "%KC_VCVARS%" >nul || exit /b 1
if "%KC_AUTHOR%"=="" set KC_AUTHOR=Flubbermunchkin
if not exist out\obj mkdir out\obj
if not exist out\dev mkdir out\dev
set CF=/nologo /std:c++17 /EHa /O2 /MT /W3 /DKC_AUTHOR=\"%KC_AUTHOR%\" /Isrc
set LF=/link /DLL user32.lib kernel32.lib
cl %CF% /LD src\automarksman.cpp /Foout\obj\ /Feout\kcd2_automarksman.dll %LF% || exit /b 1
cl %CF% /DKC_DEVTOOLS /LD src\automarksman.cpp /Foout\obj\ /Feout\dev\kcd2_automarksman.dll %LF% || exit /b 1
cl /nologo /O2 ..\..\tools\selftest.cpp /Foout\obj\ /Feout\dev\selftest.exe || exit /b 1
cl /nologo /std:c++17 /O2 /EHsc tools\marksim.cpp /Foout\obj\ /Feout\dev\marksim.exe || exit /b 1
del /q out\*.exp out\*.lib out\dev\*.exp out\dev\*.lib 2>nul
out\dev\marksim.exe >out\dev\marksim.txt || (type out\dev\marksim.txt & echo marksim FAILED & exit /b 1)
echo Build OK.
