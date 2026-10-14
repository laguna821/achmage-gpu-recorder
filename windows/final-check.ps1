param([Parameter(Mandatory)][string]$Python)
$ErrorActionPreference='Stop'
& "$PSScriptRoot\build.ps1" -Smoke
& $Python -B -X utf8 "$PSScriptRoot\test_parent_lifetime.py" --driver
if($LASTEXITCODE -ne 0){throw 'Parent lifecycle test failed'}
& "$PSScriptRoot\validate.ps1"
$exe=Join-Path (Split-Path $PSScriptRoot -Parent) 'build\windows\AchmageRecorder.exe'
& $exe install
if($LASTEXITCODE -ne 0){throw 'Per-user installation failed'}
Start-Sleep -Seconds 2
$installed=Join-Path $env:LOCALAPPDATA 'AchmageRecorder\bin\AchmageRecorder.exe'
$s=Get-Content -LiteralPath "$env:LOCALAPPDATA\AchmageRecorder\status.json" -Raw -Encoding utf8|ConvertFrom-Json
if($s.status -ne 'idle'){throw 'Installed broker not ready'}
if((Get-Process -Id $s.brokerPid).Path -ne $installed){throw 'Installed binary path mismatch'}
& $installed toggle
Start-Sleep -Seconds 6
& $installed stop
for($i=0;$i -lt 120;$i++){Start-Sleep -Milliseconds 250;$s=Get-Content -LiteralPath "$env:LOCALAPPDATA\AchmageRecorder\status.json" -Raw -Encoding utf8|ConvertFrom-Json;if($s.status -in @('idle','error')){break}}
$r=Get-Content -LiteralPath "$env:LOCALAPPDATA\AchmageRecorder\last-recording.json" -Raw -Encoding utf8|ConvertFrom-Json
if(-not $r.success){throw 'Installed recording failed'}
& $installed verify $r.file
if($LASTEXITCODE -ne 0){throw 'Installed playback verification failed'}
Write-Output 'Installed receiver ready; recording worker returned normally.'
