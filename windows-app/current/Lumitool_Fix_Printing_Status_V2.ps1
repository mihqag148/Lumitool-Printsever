param()

$ErrorActionPreference = "Stop"

$BaseDir = Join-Path $env:ProgramData "Lumitool\Printsever"
$ConfigPath = Join-Path $BaseDir "queues.json"
$WatcherSource = Join-Path $PSScriptRoot "Lumitool_Job_Watcher_V1.ps1"
$WatcherTarget = Join-Path $BaseDir "Lumitool_Job_Watcher_V1.ps1"

New-Item -ItemType Directory -Path $BaseDir -Force | Out-Null

if (-not (Test-Path $WatcherSource)) {
    throw "Thiếu Lumitool_Job_Watcher_V1.ps1 trong thư mục bộ cài."
}

Copy-Item -Path $WatcherSource -Destination $WatcherTarget -Force

function Get-LetterFromPort {
    param([int]$Port)
    if ($Port -eq 9101) { return "A" }
    if ($Port -eq 9102) { return "B" }
    if ($Port -eq 9103) { return "C" }
    return ""
}

Write-Host ""
Write-Host "Lumitool Printsever - FIX WINDOWS JOB STATUS V2" -ForegroundColor Cyan
Write-Host "Dang tim cac queue Lumitool..." -ForegroundColor Gray

$printers = @(Get-Printer -ErrorAction Stop | Where-Object {
    ([string]$_.PortName).StartsWith("LUMITOOL_") -or
    ([string]$_.Name) -like "Lumitool Printsever*"
})

if ($printers.Count -eq 0) {
    Write-Host "Khong tim thay printer Lumitool da cai." -ForegroundColor Yellow
    Read-Host "Nhan Enter de dong"
    exit 2
}

$config = @()

foreach ($p in $printers) {
    $name = [string]$p.Name
    $portName = [string]$p.PortName
    $port = Get-PrinterPort -Name $portName -ErrorAction SilentlyContinue

    if (-not $port) {
        Write-Host ("Bo qua " + $name + ": khong doc duoc port " + $portName) -ForegroundColor Yellow
        continue
    }

    $ip = [string]$port.PrinterHostAddress
    $tcp = [int]$port.PortNumber

    if ($tcp -eq 0 -and $portName -match '_(9101|9102|9103)$') {
        $tcp = [int]$Matches[1]
    }

    $letter = Get-LetterFromPort -Port $tcp

    if (-not $ip -or -not $letter) {
        Write-Host ("Bo qua " + $name + ": IP/port khong hop le") -ForegroundColor Yellow
        continue
    }

    try {
        Set-Printer -Name $name -KeepPrintedJobs $false -Datatype "RAW" -ErrorAction Stop
    } catch {}

    $config += [pscustomobject]@{
        PrinterName = $name
        IP = $ip
        Letter = $letter
        TcpPort = $tcp
    }

    Write-Host ("OK " + $name + " -> " + $ip + ":" + $tcp + " / " + $letter) -ForegroundColor Green
}

@($config) | ConvertTo-Json -Depth 4 | Set-Content -Path $ConfigPath -Encoding UTF8

$action = New-ScheduledTaskAction -Execute "powershell.exe" -Argument ('-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "' + $WatcherTarget + '"')
$trigger = New-ScheduledTaskTrigger -AtStartup
$principal = New-ScheduledTaskPrincipal -UserId "SYSTEM" -LogonType ServiceAccount -RunLevel Highest
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -RestartCount 10 -RestartInterval (New-TimeSpan -Minutes 1) -ExecutionTimeLimit ([TimeSpan]::Zero)

Register-ScheduledTask -TaskName "Lumitool Printsever Job Watcher" -Action $action -Trigger $trigger -Principal $principal -Settings $settings -Force | Out-Null

try {
    Stop-ScheduledTask -TaskName "Lumitool Printsever Job Watcher" -ErrorAction SilentlyContinue
} catch {}

Start-ScheduledTask -TaskName "Lumitool Printsever Job Watcher"

Write-Host ""
Write-Host ("Da cai watcher cho " + $config.Count + " queue.") -ForegroundColor Green
Write-Host "Can firmware V7.7 tro len de co /api/jobs."
Write-Host ("Log: " + (Join-Path $BaseDir "job-watcher.log"))
Read-Host "Nhan Enter de dong"
