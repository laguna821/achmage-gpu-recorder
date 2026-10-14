param([string]$SdkRoot = "$PSScriptRoot\..\.build-deps\sdk", [string]$SdkLibRoot = "$PSScriptRoot\..\.build-deps\sdk-x64", [switch]$Smoke)
$ErrorActionPreference='Stop'
$env:VSLANG='1033'
$root=Split-Path $PSScriptRoot -Parent
$vswhere="${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs=(& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)
if(-not $vs){throw 'MSVC C++ Build Tools are required.'}
$msvc=Get-ChildItem -LiteralPath "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name -Descending | Select-Object -First 1
$sdkVersion=(Get-ChildItem -LiteralPath "$SdkRoot\c\Include" -Directory | Sort-Object Name -Descending | Select-Object -First 1).Name
$inc="$SdkRoot\c\Include\$sdkVersion"
$lib="$SdkLibRoot\c"
$out=Join-Path $root 'build\windows'
New-Item -ItemType Directory -Path $out -Force | Out-Null
$cl=Join-Path $msvc.FullName 'bin\Hostx64\x64\cl.exe'
$argsList=@('/nologo','/std:c++17','/EHsc','/O2','/MT','/utf-8','/DUNICODE','/D_UNICODE','/DNOMINMAX',"/I$($msvc.FullName)\include")
foreach($d in @('um','shared','ucrt','winrt','cppwinrt')){$argsList+="/I$inc\$d"}
$argsList+=@("$PSScriptRoot\recorder.cpp","/Fo$out\recorder.obj","/Fe$out\AchmageRecorder.exe",'/link',"/LIBPATH:$($msvc.FullName)\lib\x64","/LIBPATH:$lib\um\x64","/LIBPATH:$lib\ucrt\x64",
'user32.lib','shell32.lib','ole32.lib','oleaut32.lib','runtimeobject.lib','d3d11.lib','dxgi.lib','mfplat.lib','mfreadwrite.lib','mfuuid.lib','mf.lib','wtsapi32.lib','shlwapi.lib','advapi32.lib','/SUBSYSTEM:WINDOWS','/ENTRY:wmainCRTStartup')
& $cl @argsList
if($LASTEXITCODE -ne 0){throw "Compilation failed: $LASTEXITCODE"}
if($Smoke){& "$PSScriptRoot\smoke.ps1"}
