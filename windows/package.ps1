$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$stage=Join-Path $root 'dist\windows'
New-Item -ItemType Directory -Path $stage -Force | Out-Null
Copy-Item -LiteralPath "$root\build\windows\AchmageRecorder.exe" -Destination "$stage\AchmageRecorder.exe"
foreach($name in @('Install.cmd','Uninstall.cmd')){Copy-Item -LiteralPath "$PSScriptRoot\$name" -Destination "$stage\$name"}
foreach($name in @('LICENSE','README.md','VALIDATION.md','UPSTREAM.md')){Copy-Item -LiteralPath "$root\$name" -Destination "$stage\$name"}
$commit=(& git -C $root rev-parse HEAD).Trim()
@{sourceCommit=$commit;binarySha256=(Get-FileHash -LiteralPath "$stage\AchmageRecorder.exe" -Algorithm SHA256).Hash;windowsSourceSha256=(Get-FileHash -LiteralPath "$PSScriptRoot\recorder.cpp" -Algorithm SHA256).Hash;builtAt=(Get-Date).ToUniversalTime().ToString('o')}|ConvertTo-Json|Set-Content -LiteralPath "$stage\build-info.json" -Encoding utf8
Compress-Archive -LiteralPath "$stage\AchmageRecorder.exe","$stage\Install.cmd","$stage\Uninstall.cmd","$stage\LICENSE","$stage\README.md","$stage\VALIDATION.md","$stage\UPSTREAM.md","$stage\build-info.json" -DestinationPath "$root\dist\AchmageRecorder-windows-x64.zip" -Force
