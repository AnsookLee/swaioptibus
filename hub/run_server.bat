@echo off
setlocal
title WS Hub (websockets)
cd /d "%~dp0"

where python >nul 2>nul
if errorlevel 1 (
  echo [ERROR] Python not found in PATH. Install Python and check "Add to PATH".
  pause
  exit /b 1
)

set VENV_DIR=%CD%\.venv
if not exist "%VENV_DIR%\Scripts\python.exe" (
  echo [INFO] Creating venv...
  python -m venv "%VENV_DIR%"
)

set PY="%VENV_DIR%\Scripts\python.exe"
if not exist %PY% (
  echo [WARN] venv missing, fallback to system Python
  set PY=python
)

echo [INFO] Installing requirements...
%PY% -m pip install --upgrade pip
if exist requirements.txt (
  %PY% -m pip install -r requirements.txt
) else (
  %PY% -m pip install websockets
)

set HOST=0.0.0.0
set PORT=81
echo [INFO] Starting hub: ws://%HOST%:%PORT%
%PY% -u hub.py --host %HOST% --port %PORT%
set ERR=%ERRORLEVEL%
if %ERR% NEQ 0 echo [ERROR] Exit code %ERR%
pause
