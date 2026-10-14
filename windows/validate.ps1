param([switch]$Long)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$exe=Join-Path $root 'build\windows\AchmageRecorder.exe'
$dir=Join-Path $root 'validation\private'
New-Item -ItemType Directory -Path $dir -Force | Out-Null
$stateDir=Join-Path $env:LOCALAPPDATA 'AchmageRecorder'
$results=[Collections.Generic.List[object]]::new()
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class RecorderTestInput {
 [StructLayout(LayoutKind.Sequential)] public struct INPUT {public uint type; public UNION data;}
 [StructLayout(LayoutKind.Explicit)] public struct UNION {
  [FieldOffset(0)] public KEYBDINPUT ki;
  [FieldOffset(0)] public MOUSEINPUT mi;
 }
 [StructLayout(LayoutKind.Sequential)] public struct KEYBDINPUT {public ushort vk,scan;public uint flags,time;public UIntPtr extra;}
 [StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT {public int x,y;public uint data,flags,time;public UIntPtr extra;}
 [DllImport("user32.dll",SetLastError=true)] static extern uint SendInput(uint n,INPUT[] values,int size);
 public static void Chord(){
  ushort[] keys={0x5B,0xA4,0x52,0x52,0xA4,0x5B};var values=new INPUT[6];
  for(int i=0;i<6;i++){values[i].type=1;values[i].data.ki.vk=keys[i];values[i].data.ki.flags=(uint)(i>=3?2:0);}
  if(SendInput(6,values,Marshal.SizeOf(typeof(INPUT)))!=6)throw new Exception("SendInput failed");
 }
}
'@
function Read-State {Get-Content -LiteralPath "$stateDir\status.json" -Raw -Encoding utf8 | ConvertFrom-Json}
function Wait-Idle {
 for($i=0;$i -lt 120;$i++){
  Start-Sleep -Milliseconds 250
  $s=Read-State
  if($s.status -in @('idle','error')){return $s}
 }
 throw 'Normal stop did not complete in30 seconds. No forced termination.'
}
function Record-Case([int]$seconds,[string]$label){
 & $exe toggle
 Start-Sleep -Seconds 2
 $initial=Read-State
 if($initial.status -ne 'recording'){throw "Recorder did not start: $($initial.status)"}
 $maxRAM=0;$maxPrivate=0;$begin=Get-Date
 for($i=0;$i -lt $seconds;$i++){
  $s=Read-State
  if($s.status -ne 'recording'){throw "Recording stopped early: $($s.status)"}
  $proc=Get-Process -Id $s.workerPid -ErrorAction Stop
  $maxRAM=[math]::Max($maxRAM,$proc.WorkingSet64);$maxPrivate=[math]::Max($maxPrivate,$proc.PrivateMemorySize64)
  if($i%60 -eq 0){Write-Output "$label progress: $i / $seconds seconds"}
  Start-Sleep -Seconds 1
 }
 & $exe stop
 $end=Wait-Idle
 $record=Get-Content -LiteralPath "$stateDir\last-recording.json" -Raw -Encoding utf8 | ConvertFrom-Json
 if(-not $record.success){throw 'Recording result unsuccessful'}
 if(Get-Process -Id $initial.workerPid -ErrorAction SilentlyContinue){throw 'Worker survived normal stop'}
 $decoded=(& $exe verify $record.file | ConvertFrom-Json)
 if($LASTEXITCODE -ne 0 -or $decoded.decodedFrames -ne $record.frames){throw 'Full decode verification failed'}
 $row=[pscustomobject]@{Case=$label;RequestedSeconds=$seconds;ElapsedSeconds=((Get-Date)-$begin).TotalSeconds;EncodedFrames=$record.frames;DecodedFrames=$decoded.decodedFrames;VideoSeconds=$decoded.seconds;HardwareEncoder=$record.hardwareEncoder;MaxWorkerWS=$maxRAM;MaxWorkerPrivate=$maxPrivate;File=$record.file;Bytes=(Get-Item -LiteralPath $record.file).Length;WorkerReturned=$true}
 $results.Add($row)
 $results | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath "$dir\results.json" -Encoding utf8
 Write-Output "$label completed: $($decoded.decodedFrames) frames decoded."
}
try {
 Start-Process -FilePath $exe -ArgumentList '--broker' -WindowStyle Hidden
 Start-Sleep -Seconds 1
 $initial=Read-State
 if($initial.status -ne 'idle'){throw "Hotkey receiver unavailable: $($initial.error)"}
 $p=Get-Process -Id $initial.brokerPid
 $cpuStart=$p.TotalProcessorTime.TotalSeconds
 Start-Sleep -Seconds 30
 $p.Refresh()
 $idle=[pscustomobject]@{BrokerRAM=$p.WorkingSet64;PrivateBytes=$p.PrivateMemorySize64;CPUPercent=100*($p.TotalProcessorTime.TotalSeconds-$cpuStart)/30;HotkeyMode=$initial.hotkeyMode}
 $idle | ConvertTo-Json | Set-Content -LiteralPath "$dir\idle.json" -Encoding utf8
 if($idle.BrokerRAM -gt 20MB -or $idle.CPUPercent -gt 0.1){throw 'Idle resource target exceeded'}
 [RecorderTestInput]::Chord()
 Start-Sleep -Seconds 3
 if((Read-State).status -ne 'recording'){throw 'Recording hotkey did not start capture'}
 [RecorderTestInput]::Chord()
 $hotkeyEnd=Wait-Idle
 if($hotkeyEnd.status -ne 'idle'){throw 'Recording hotkey did not stop normally'}
 Write-Output 'Win+LeftAlt+R start/stop passed using native injected chord.'
 for($i=1;$i -le 10;$i++){Record-Case 3 "cycle-$i"}
 if($Long){Record-Case 600 '10-minute';Record-Case 3600 '60-minute'}
} finally {
 & $exe stop
 try{Wait-Idle | Out-Null}catch{Write-Warning 'Normal stop unconfirmed; no force kill.'}
 & $exe exit
 Start-Sleep -Seconds 1
}
