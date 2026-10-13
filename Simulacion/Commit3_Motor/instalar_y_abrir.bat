@echo off
setlocal
cd /d "%~dp0"
if exist ".venv\Scripts\python.exe" goto dependencies
where py >nul 2>&1
if errorlevel 1 goto no_python
echo Preparando Python para el panel...
py -3 -m venv ".venv"
if errorlevel 1 goto failed
:dependencies
".venv\Scripts\python.exe" -c "import tkinter, serial" >nul 2>&1
if not errorlevel 1 goto run
echo Instalando pyserial. Solo la primera vez necesita internet...
".venv\Scripts\python.exe" -m pip install -r requirements.txt
if errorlevel 1 goto failed
:run
".venv\Scripts\python.exe" panel_motor.py
if errorlevel 1 goto failed
exit /b 0
:no_python
echo Instala Python 3 para Windows desde https://www.python.org/downloads/windows/
echo Incluye el lanzador py y Tcl/Tk. Despues abre este archivo otra vez.
pause
exit /b 1
:failed
echo No se pudo abrir el panel. Revisa el mensaje de arriba.
pause
exit /b 1
