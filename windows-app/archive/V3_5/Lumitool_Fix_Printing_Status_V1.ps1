param()

$ErrorActionPreference = "Stop"

function Find-ScriptFile {
    param([string]$Name)

    $root = Join-Path $env:WINDIR "System32\Printing_Admin_Scripts"
    if (-not (Test-Path $root)) { return $null }

    $f = Get-ChildItem -Path $root -Filter $Name -File -Recurse -ErrorAction SilentlyContinue |
        Select-Object -First 1

    if ($f) { return [string]$f.FullName }
    return $null
}

Write-Host ""
Write-Host "Lumitool Printsever - FIX PRINTING STATUS V1" -ForegroundColor Cyan
Write-Host "Dang tim cac queue Lumitool..." -ForegroundColor Gray

$printers = @(
    Get-CimInstance -ClassName Win32_Printer -ErrorAction Stop |
    Where-Object {
        ([string]$_.PortName).StartsWith("LUMITOOL_") -or
        ([string]$_.Name) -like "Lumitool Printsever*"
    }
)

if ($printers.Count -eq 0) {
    Write-Host "Khong tim thay printer Lumitool da cai." -ForegroundColor Yellow
    Write-Host "Hay cai printer bang Setup V3.5 truoc."
    Read-Host "Nhan Enter de dong"
    exit 2
}

$prnport = Find-ScriptFile "prnport.vbs"
$prncnfg = Find-ScriptFile "prncnfg.vbs"

foreach ($p in $printers) {
    $name = [string]$p.Name
    $port = [string]$p.PortName

    Write-Host ""
    Write-Host ("FIX: " + $name) -ForegroundColor White
    Write-Host ("PORT: " + $port)

    if ($prnport -and $port) {
        & cscript.exe //NoLogo $prnport -t -r $port -md 2>&1 |
            ForEach-Object { Write-Host ("  SNMP: " + $_) }
    }

    if ($prncnfg) {
        & cscript.exe //NoLogo $prncnfg -t -P $name +direct -queued -enablebidi -keepprintedjobs -enabledevq -workoffline 2>&1 |
            ForEach-Object { Write-Host ("  QUEUE: " + $_) }
    }

    $fresh = Get-CimInstance -ClassName Win32_Printer -ErrorAction Stop |
        Where-Object { [string]$_.Name -eq $name } |
        Select-Object -First 1

    if ($fresh) {
        Set-CimInstance -InputObject $fresh -Property @{
            EnableBIDI          = $false
            KeepPrintedJobs     = $false
            Direct              = $true
            Queued              = $false
            EnableDevQueryPrint = $false
            WorkOffline         = $false
        } -ErrorAction Stop | Out-Null
    }

    try {
        Set-Printer -Name $name -KeepPrintedJobs $false -Datatype "RAW" -ErrorAction Stop
    } catch {
        Write-Host ("  RAW warning: " + $_.Exception.Message) -ForegroundColor Yellow
    }
}

Write-Host ""
Write-Host "Restart Print Spooler..." -ForegroundColor Gray
Restart-Service -Name Spooler -Force -ErrorAction Stop
(Get-Service -Name Spooler).WaitForStatus(
    [System.ServiceProcess.ServiceControllerStatus]::Running,
    [TimeSpan]::FromSeconds(10)
)
Start-Sleep -Milliseconds 800

Write-Host ""
Write-Host "VERIFY" -ForegroundColor Cyan
foreach ($p in $printers) {
    $name = [string]$p.Name
    $v = Get-CimInstance -ClassName Win32_Printer -ErrorAction Stop |
        Where-Object { [string]$_.Name -eq $name } |
        Select-Object -First 1

    if ($v) {
        Write-Host (
            $name +
            " | Direct=" + [string]$v.Direct +
            " Queued=" + [string]$v.Queued +
            " BIDI=" + [string]$v.EnableBIDI +
            " Keep=" + [string]$v.KeepPrintedJobs +
            " DevQuery=" + [string]$v.EnableDevQueryPrint
        ) -ForegroundColor Green
    }
}

Write-Host ""
Write-Host "Da ap dung Fast Completion." -ForegroundColor Green
Write-Host "In lai 1 tem de test. Job Windows se ket thuc khi Windows gui xong RAW data."
Read-Host "Nhan Enter de dong"
