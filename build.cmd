@echo off
rem Builds MusicToMic.exe with Visual Studio 2022 Build Tools / Community (MSVC).
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
cl /nologo /O2 /EHsc /W3 /DUNICODE /D_UNICODE /std:c++17 main.cpp /Fe:MusicToMic.exe /link /SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup
if exist main.obj del main.obj
