@echo off
rem Installs the free VB-CABLE virtual audio device (run as administrator).
rem Download: https://vb-audio.com/Cable/  (VBCABLE_Driver_Pack45.zip) - unzip into the "vbcable" folder next to this file.
cd /d "%~dp0vbcable"
if not exist VBCABLE_Setup_x64.exe (
  echo VBCABLE_Setup_x64.exe not found. Download the zip from https://vb-audio.com/Cable/ and unzip it into %~dp0vbcable
  pause
  exit /b 1
)
VBCABLE_Setup_x64.exe -i -h
echo VB-CABLE installed. If "CABLE Input/Output" do not show up in Windows sound devices, reboot once.
