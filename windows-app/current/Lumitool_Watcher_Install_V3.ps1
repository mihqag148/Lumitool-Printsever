param()

$ErrorActionPreference = "Stop"
$BaseDir = Join-Path $env:ProgramData "Lumitool\Printsever"
$Source = Join-Path $PSScriptRoot "Lumitool_Job_Watcher_V3.ps1"
$Target = Join-Path $BaseDir "Lumitool_Job_Watcher_V3.ps1"
$TaskName = "Lumitool Printsever Job Watcher"

New-Item -ItemType Directory -Path $BaseDir -Force | Out-Null
if (-not (Test-Path $Source)) { throw "Missing Lumitool_Job_Watcher_V3.ps1" }

Copy-Item -Path $Source -Destination $Target -Force
foreach ($old in @("Lumitool_Job_Watcher_V1.ps1","Lumitool_Job_Watcher_V2.ps1")) {
    try { Remove-Item (Join-Path $BaseDir $old) -Force -ErrorAction SilentlyContinue } catch {}
}

try { Stop-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue } catch {}

$action = New-ScheduledTaskAction -Execute "powershell.exe" -Argument ('-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "' + $Target + '"')
$trigger = New-ScheduledTaskTrigger -AtStartup
$principal = New-ScheduledTaskPrincipal -UserId "SYSTEM" -LogonType ServiceAccount -RunLevel Highest
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -RestartCount 10 -RestartInterval (New-TimeSpan -Minutes 1) -ExecutionTimeLimit ([TimeSpan]::Zero)

Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger $trigger -Principal $principal -Settings $settings -Force | Out-Null
Start-ScheduledTask -TaskName $TaskName

Start-Sleep -Milliseconds 500
$task = Get-ScheduledTask -TaskName $TaskName -ErrorAction Stop
$info = Get-ScheduledTaskInfo -TaskName $TaskName -ErrorAction Stop

$logPath = Join-Path $BaseDir "watcher-install.log"
Add-Content -Path $logPath -Value ("[{0}] Installed V3 state={1} lastResult={2}" -f (Get-Date -Format "yyyy-MM-dd HH:mm:ss"), $task.State, $info.LastTaskResult) -Encoding UTF8

exit 0
