param()

$ErrorActionPreference = "Continue"
$LogPath = Join-Path $env:TEMP "Lumitool_Preinstall_Cleanup_V2.log"

function Log {
    param([string]$Text)
    try {
        Add-Content -Path $LogPath -Value ("[{0}] {1}" -f (Get-Date -Format "yyyy-MM-dd HH:mm:ss.fff"), $Text) -Encoding UTF8
    } catch {}
}

Log "=== SAFE UPGRADE START ==="

# Stop the old watcher before replacing program files.
try {
    $task = Get-ScheduledTask -TaskName "Lumitool Printsever Job Watcher" -ErrorAction SilentlyContinue
    if ($task) {
        Log ("Found watcher task state=" + [string]$task.State)
        try { Stop-ScheduledTask -TaskName "Lumitool Printsever Job Watcher" -ErrorAction SilentlyContinue } catch {}
        Unregister-ScheduledTask -TaskName "Lumitool Printsever Job Watcher" -Confirm:$false -ErrorAction SilentlyContinue
        Log "Old watcher task removed"
    }
} catch {
    Log ("Watcher cleanup warning: " + $_.Exception.Message)
}

# Close only Lumitool app/helper processes so installed files can be replaced.
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
} catch {
    Log ("Process cleanup warning: " + $_.Exception.Message)
}

# IMPORTANT FOR RETAIL:
# Do not remove LUMITOOL printer queues, TCP ports, print jobs, or the device cache.
# Customers may have several print servers and several PCs already configured.
# V5.0 upgrades in-place and Watcher V3 repairs stale DHCP addresses automatically.
Log "Existing Lumitool printer queues preserved."
Log "Existing Lumitool TCP/IP ports preserved."
Log "Existing device cache preserved."
Log "Vendor printer drivers preserved."

Start-Sleep -Milliseconds 400
Log "=== SAFE UPGRADE COMPLETE ==="
exit 0
