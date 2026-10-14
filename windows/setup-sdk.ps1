$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$deps=Join-Path $root '.build-deps'
New-Item -ItemType Directory -Path $deps -Force | Out-Null
Add-Type -AssemblyName System.IO.Compression.FileSystem
foreach($item in @(@('microsoft.windows.sdk.cpp','sdk'),@('microsoft.windows.sdk.cpp.x64','sdk-x64'))){
 $dest=Join-Path $deps $item[1]
 if(Test-Path -LiteralPath $dest){continue}
 $package=Join-Path $deps ($item[1]+'.nupkg')
 Invoke-WebRequest -Uri ("https://api.nuget.org/v3-flatcontainer/"+$item[0]+"/10.0.26100.9169/"+$item[0]+".10.0.26100.9169.nupkg") -OutFile $package
 [IO.Compression.ZipFile]::ExtractToDirectory($package,$dest)
}
