@echo off
"%~dp0AchmageRecorder.exe" install
if errorlevel 1 (
 echo Installation failed. Existing recording may still be running.
 pause
 exit /b 1
)
echo Installed for this user. Press Win + Left Alt + R to record.
