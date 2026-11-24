@echo off
setlocal
pushd "%~dp0"
rem Cerca Visual Studio 2017-2022 (qualsiasi edizione, anche Build Tools) con vswhere.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VCVARS="
if exist "%VSWHERE%" (
  for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
)
if not defined VCVARS set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
  echo Visual Studio con il carico di lavoro "Sviluppo di applicazioni desktop C++" non trovato.
  echo Installare Visual Studio 2022 Community o Build Tools for Visual Studio.
  popd
  exit /b 1
)
call "%VCVARS%" >nul
rem I moduli sono descritti in osmer_console.h. L'icona e' in osmer_console.rc.
echo Compilazione avviata %DATE% %TIME% > build_log.txt
rc /nologo /fo osmer_console.res osmer_console.rc >> build_log.txt 2>&1
if errorlevel 1 goto errore
cl /nologo /EHsc /O2 /std:c++17 /utf-8 /W3 /Fe:osmer_console.exe osmer_console.cpp dati.cpp grafica.cpp esportazione.cpp aggiornamento.cpp osmer_console.res user32.lib gdi32.lib gdiplus.lib ole32.lib comdlg32.lib shell32.lib /link /SUBSYSTEM:WINDOWS >> build_log.txt 2>&1
if errorlevel 1 goto errore
del /q *.obj osmer_console.res 2>nul
echo ESITO: OK %DATE% %TIME% >> build_log.txt
echo Compilato: %~dp0osmer_console.exe
popd
exit /b 0
:errore
echo ESITO: ERRORE %DATE% %TIME% >> build_log.txt
echo Compilazione NON riuscita: vedere build_log.txt
popd
exit /b 1
