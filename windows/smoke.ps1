$ErrorActionPreference='Stop'
$exe=Join-Path (Split-Path $PSScriptRoot -Parent) 'build\windows\AchmageRecorder.exe'
& $exe toggle
Start-Sleep -Seconds 12
& $exe stop
for($i=0;$i -lt 40;$i++){
 Start-Sleep -Milliseconds 500
 $s=Get-Content -LiteralPath "$env:LOCALAPPDATA\AchmageRecorder\status.json" -Raw | ConvertFrom-Json
 if($s.status -in @('idle','error')){break}
}
Get-Content -LiteralPath "$env:LOCALAPPDATA\AchmageRecorder\last-recording.json" -Raw -Encoding utf8
& $exe exit
if(-not (Get-Content -LiteralPath "$env:LOCALAPPDATA\AchmageRecorder\last-recording.json" -Raw | ConvertFrom-Json).success){throw 'Capture failed'}
