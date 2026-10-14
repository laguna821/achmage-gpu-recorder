@echo off
"%~dp0AchmageRecorder.exe" uninstall
if errorlevel 1 (
 echo Uninstallation did not complete. Stop recording first.
 pause
 exit /b 1
)
echo Removed application and startup shortcut. Recordings were preserved.
