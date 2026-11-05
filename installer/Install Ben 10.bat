@echo off
chcp 65001 >nul
title Ben 10: Protector of Earth - installer
cd /d "%~dp0"
python\python.exe -X utf8 installer\install.py %*
echo.
pause
