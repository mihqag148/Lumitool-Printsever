param()

$ErrorActionPreference = "Continue"
$BaseDir = Join-Path $env:ProgramData "Lumitool\Printsever"
$LogPath = Join-Path $BaseDir "job-watcher.log"
New-Item -ItemType Directory -Path $BaseDir -Force | Out-Null

function Log {
    param([string]$Text)
    try { Add-Content -Path $LogPath -Value ("[{0}] {1}" -f (Get-Date -Format "yyyy-MM-dd HH:mm:ss.fff"), $Text) -Encoding UTF8 } catch {}
}

function Get-EspJobs {
    param([string]$IP)
    try { return Invoke-RestMethod -Uri ("http://" + $IP + "/api/jobs") -Method Get -TimeoutSec 1 -ErrorAction Stop } catch { return $null }
}

function Get-EspPrinterState {
    param($Api,[string]$Letter)
    if (-not $Api -or -not $Api.printers) { return $null }
    try {
        $p = $Api.printers.PSObject.Properties[$Letter]
        if ($p) { return $p.Value }
    } catch {}
    return $null
}

function Get-RouteLocalIP {
    param([string]$RemoteIP)
    $udp = $null
    try {
        $udp = New-Object System.Net.Sockets.UdpClient
        $udp.Connect($RemoteIP,80)
        $ep = [System.Net.IPEndPoint]$udp.Client.LocalEndPoint
        if ($ep) { return [string]$ep.Address.IPAddressToString }
    } catch {} finally { if ($udp) { try { $udp.Close() } catch {} } }
    return ""
}

function Get-LetterFromPort {
    param([int]$Port)
    if ($Port -eq 9101) { return "A" }
    if ($Port -eq 9102) { return "B" }
    if ($Port -eq 9103) { return "C" }
    return ""
}

function Clear-WindowsJob {
    param([string]$PrinterName,[uint32]$JobId)
    try {
        Remove-PrintJob -PrinterName $PrinterName -ID $JobId -Confirm:$false -ErrorAction Stop
        return "Remove-PrintJob"
    } catch {
        $firstError = $_.Exception.Message
        try {
            $jobs = @(Get-CimInstance -ClassName Win32_PrintJob -ErrorAction Stop | Where-Object { [uint32]$_.JobId -eq $JobId -and [string]$_.Name -like ($PrinterName + ",*") })
            foreach ($j in $jobs) {
                Invoke-CimMethod -InputObject $j -MethodName Delete -ErrorAction Stop | Out-Null
            }
            if ($jobs.Count -gt 0) { return "Win32_PrintJob.Delete" }
        } catch {
            throw ("Remove-PrintJob: " + $firstError + " | WMI: " + $_.Exception.Message)
        }
        throw $firstError
    }
}

Import-Module PrintManagement -ErrorAction SilentlyContinue
$states = @{}
$lastDiscoveryLog = [DateTime]::MinValue
Log "WATCHER START V2 AUTO-DISCOVERY"

while ($true) {
    try {
        $queues = @(
            Get-Printer -ErrorAction SilentlyContinue |
            Where-Object { ([string]$_.PortName).StartsWith("LUMITOOL_") }
        )

        if ((Get-Date) - $lastDiscoveryLog -gt [TimeSpan]::FromMinutes(2)) {
            Log ("AUTO-DISCOVERY: " + $queues.Count + " Lumitool queue(s)")
            $lastDiscoveryLog = Get-Date
        }

        $apiCache = @{}
        $routeIpCache = @{}
        $liveKeys = @{}

        foreach ($q in $queues) {
            $printerName = [string]$q.Name
            $portName = [string]$q.PortName
            $port = Get-PrinterPort -Name $portName -ErrorAction SilentlyContinue
            if (-not $port) { continue }

            $ip = [string]$port.PrinterHostAddress
            $tcp = [int]$port.PortNumber
            if ($tcp -eq 0 -and $portName -match "_(9101|9102|9103)$") { $tcp = [int]$Matches[1] }
            $letter = Get-LetterFromPort -Port $tcp
            if (-not $ip -or -not $letter) { continue }

            if (-not $apiCache.ContainsKey($ip)) { $apiCache[$ip] = Get-EspJobs -IP $ip }
            if (-not $routeIpCache.ContainsKey($ip)) { $routeIpCache[$ip] = Get-RouteLocalIP -RemoteIP $ip }
            $api = $apiCache[$ip]
            $esp = Get-EspPrinterState -Api $api -Letter $letter
            $localRouteIp = [string]$routeIpCache[$ip]

            $jobs = @()
            try { $jobs = @(Get-PrintJob -PrinterName $printerName -ErrorAction Stop) } catch { continue }

            foreach ($job in $jobs) {
                $jobId = [uint32]$job.ID
                $key = $printerName + "|" + $jobId
                $liveKeys[$key] = $true

                if (-not $states.ContainsKey($key)) {
                    $baseline = 0
                    if ($esp -and $esp.last -and $esp.last.id) { $baseline = [uint32]$esp.last.id }
                    $states[$key] = [pscustomobject]@{ FirstSeen=(Get-Date); BaselineDoneId=$baseline; SawActive=$false; CandidateSince=$null; LastEspId=0 }
                    Log ("WIN JOB seen printer=" + $printerName + " id=" + $jobId + " baselineEsp=" + $baseline + " routeIP=" + $localRouteIp)
                }

                $st = $states[$key]
                if (-not $esp) { $st.CandidateSince = $null; continue }

                if ([int]$esp.active -gt 0) {
                    if (-not $st.SawActive) { Log ("ESP ACTIVE printer=" + $letter + " for winJob=" + $jobId) }
                    $st.SawActive = $true
                    $st.CandidateSince = $null
                    continue
                }

                if ([bool]$esp.runtime_error -or -not $esp.last -or -not [bool]$esp.last.success) {
                    $st.CandidateSince = $null
                    continue
                }

                $lastId = [uint32]$esp.last.id
                $sourceIp = [string]$esp.last.source_ip
                $sourceMatch = ($sourceIp -and $localRouteIp -and $sourceIp -eq $localRouteIp)
                $newCompletion = ($lastId -gt [uint32]$st.BaselineDoneId)
                $recent = $false
                try {
                    $ageMs = [int64]$api.uptime_ms - [int64]$esp.last.finished_ms
                    $recent = ($ageMs -ge 0 -and $ageMs -le 20000)
                } catch {}
                $jobYoung = ((Get-Date) - $st.FirstSeen) -le [TimeSpan]::FromSeconds(90)

                $confirmed = $st.SawActive -or ($sourceMatch -and $newCompletion) -or ($sourceMatch -and $recent -and $jobYoung)
                if (-not $confirmed) { $st.CandidateSince = $null; continue }

                if (-not $st.CandidateSince -or $st.LastEspId -ne $lastId) {
                    $st.CandidateSince = Get-Date
                    $st.LastEspId = $lastId
                    Log ("ESP DONE candidate printer=" + $letter + " winJob=" + $jobId + " espJob=" + $lastId + " source=" + $sourceIp + " route=" + $localRouteIp)
                    continue
                }

                if ((Get-Date) - $st.CandidateSince -lt [TimeSpan]::FromMilliseconds(600)) { continue }

                try {
                    $method = Clear-WindowsJob -PrinterName $printerName -JobId $jobId
                    Log ("WINDOWS JOB CLEARED printer=" + $printerName + " winJob=" + $jobId + " after ESP DONE #" + $lastId + " via " + $method)
                    $states.Remove($key)
                } catch {
                    Log ("CLEAR ERROR printer=" + $printerName + " winJob=" + $jobId + ": " + $_.Exception.Message)
                    $st.CandidateSince = Get-Date
                }
            }
        }

        foreach ($key in @($states.Keys)) { if (-not $liveKeys.ContainsKey($key)) { $states.Remove($key) } }
    } catch {
        Log ("LOOP ERROR: " + $_.Exception.Message)
    }

    Start-Sleep -Milliseconds 400
}