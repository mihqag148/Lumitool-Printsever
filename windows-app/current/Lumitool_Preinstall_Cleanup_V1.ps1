param()

$ErrorActionPreference = "Continue"
$LogPath = Join-Path $env:TEMP "Lumitool_Preinstall_Cleanup.log"

function Log {
    param([string]$Text)
    try { Add-Content -Path $LogPath -Value ("[{0}] {1}" -f (Get-Date -Format "yyyy-MM-dd HH:mm:ss.fff"), $Text) -Encoding UTF8 } catch {}
}

Log "=== CLEAN INSTALL START ==="

# 1) Stop old watcher task before touching files.
try {
    $task = Get-ScheduledTask -TaskName "Lumitool Printsever Job Watcher" -ErrorAction SilentlyContinue
    if ($task) {
        Log ("Found old watcher task state=" + [string]$task.State)
        try { Stop-ScheduledTask -TaskName "Lumitool Printsever Job Watcher" -ErrorAction SilentlyContinue } catch {}
        Unregister-ScheduledTask -TaskName "Lumitool Printsever Job Watcher" -Confirm:$false -ErrorAction SilentlyContinue
        Log "Old watcher task removed"
    }
} catch { Log ("Watcher task cleanup warning: " + $_.Exception.Message) }

# 2) Force-close old Lumitool PowerShell/CMD processes so files are not locked.
try {
    $procs = @(Get-CimInstance Win32_Process -ErrorAction SilentlyContinue | Where-Object {
        $name = ([string]$_.Name).ToLowerInvariant()
        $cmd = [string]$_.CommandLine
        ([uint32]$_.ProcessId -ne [uint32]$PID) -and
        ($name -in @("powershell.exe","pwsh.exe","cmd.exe","lumitool_printsever.exe")) -and
        (($name -eq "lumitool_printsever.exe") -or ($cmd -match "Lumitool_Printsever|Lumitool_Job_Watcher|Lumitool_OTA_Helper|Lumitool_Scan_Helper|Lumitool_Fix_Printing_Status"))
    })

    foreach ($p in $procs) {
        try {
            Log ("Stopping old process PID=" + $p.ProcessId + " " + [string]$p.Name)
            Invoke-CimMethod -InputObject $p -MethodName Terminate -ErrorAction SilentlyContinue | Out-Null
        } catch {}
    }
} catch { Log ("Process cleanup warning: " + $_.Exception.Message) }

Start-Sleep -Milliseconds 500

# 3) Remove only Lumitool-created Windows printer queues and jobs.
# Manufacturer drivers (SP46, etc.) are intentionally preserved.
try { Import-Module PrintManagement -ErrorAction SilentlyContinue } catch {}

try {
    $queues = @(Get-Printer -ErrorAction SilentlyContinue | Where-Object {
        ([string]$_.PortName).StartsWith("LUMITOOL_") -or
        ([string]$_.Name) -like "Lumitool Printsever*"
    })

    foreach ($q in $queues) {
        $name = [string]$q.Name
        $port = [string]$q.PortName
        Log ("Removing old printer queue: " + $name + " port=" + $port)

        try {
            @(Get-PrintJob -PrinterName $name -ErrorAction SilentlyContinue) | ForEach-Object {
                try { Remove-PrintJob -PrinterName $name -ID $_.ID -Confirm:$false -ErrorAction SilentlyContinue } catch {}
            }
        } catch {}

        try { Remove-Printer -Name $name -ErrorAction SilentlyContinue } catch {}
    }
} catch { Log ("Printer queue cleanup warning: " + $_.Exception.Message) }

# 4) Remove only Lumitool-created TCP/IP ports.
try {
    $ports = @(Get-PrinterPort -ErrorAction SilentlyContinue | Where-Object { ([string]$_.Name).StartsWith("LUMITOOL_") })
    foreach ($p in $ports) {
        $pn = [string]$p.Name
        Log ("Removing old Lumitool port: " + $pn)
        try { Remove-PrinterPort -Name $pn -ErrorAction SilentlyContinue } catch {}
    }
} catch { Log ("Port cleanup warning: " + $_.Exception.Message) }

# 5) Remove old Lumitool runtime state. This is deliberately a clean install.
foreach ($dir in @(
    (Join-Path $env:ProgramData "Lumitool\Printsever"),
    (Join-Path $env:LOCALAPPDATA "Lumitool\Printsever")
)) {
    try {
        if (Test-Path $dir) {
            Log ("Removing old data: " + $dir)
            Remove-Item -Path $dir -Recurse -Force -ErrorAction SilentlyContinue
        }
    } catch { Log ("Data cleanup warning " + $dir + ": " + $_.Exception.Message) }
}

Log "Vendor printer drivers preserved."
Log "=== CLEAN INSTALL COMPLETE ==="
exit 0