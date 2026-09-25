param()

$ErrorActionPreference = "Continue"
$BaseDir = Join-Path $env:ProgramData "Lumitool\Printsever"
$LogPath = Join-Path $BaseDir "job-watcher.log"
New-Item -ItemType Directory -Path $BaseDir -Force | Out-Null

function Log {
    param([string]$Text)
    try {
        Add-Content -Path $LogPath -Value ("[{0}] {1}" -f (Get-Date -Format "yyyy-MM-dd HH:mm:ss.fff"), $Text) -Encoding UTF8
    } catch {}
}

function Get-DeviceInfo {
    param([string]$IP)
    try {
        $o = Invoke-RestMethod -Uri ("http://" + $IP + "/device-info") -Method Get -TimeoutSec 1 -ErrorAction Stop
        if ([string]$o.magic -ne "LUMITOOL_PRINTSEVER") { return $null }

        $id = ([string]$o.id).ToUpper()
        $uid = ([string]$o.uid).ToUpper() -replace '[^0-9A-F]', ''
        if (-not $uid) {
            $uid = ([string]$o.mac).ToUpper() -replace '[^0-9A-F]', ''
        }

        return [pscustomobject]@{
            IP = [string]$IP
            ID = $id
            UID = $uid
            Name = [string]$o.device_name
            Version = [string]$o.version
        }
    } catch {
        return $null
    }
}

function Get-ActiveIPv4Adapters {
    $out = @()
    try {
        foreach ($x in @(Get-NetIPAddress -AddressFamily IPv4 -ErrorAction Stop)) {
            $ip = [string]$x.IPAddress
            if (
                $ip -match '^127\.' -or
                $ip -match '^169\.254\.' -or
                [string]$x.AddressState -eq "Duplicate"
            ) {
                continue
            }

            if ($ip -match '^(\d+)\.(\d+)\.(\d+)\.\d+$') {
                $out += [pscustomobject]@{
                    IP = $ip
                    Broadcast = ($Matches[1] + "." + $Matches[2] + "." + $Matches[3] + ".255")
                }
            }
        }
    } catch {}
    return @($out)
}

function Find-LumitoolDevices {
    $found = @{}
    $payload = [System.Text.Encoding]::ASCII.GetBytes("LUMITOOL_DISCOVER_V1")

    foreach ($adapter in @(Get-ActiveIPv4Adapters)) {
        $udp = $null
        try {
            $udp = New-Object System.Net.Sockets.UdpClient
            $udp.EnableBroadcast = $true

            $local = New-Object System.Net.IPEndPoint(
                [System.Net.IPAddress]::Parse([string]$adapter.IP),
                0
            )
            $udp.Client.Bind($local)

            foreach ($target in @([string]$adapter.Broadcast, "255.255.255.255")) {
                try {
                    $ep = New-Object System.Net.IPEndPoint(
                        [System.Net.IPAddress]::Parse($target),
                        4210
                    )
                    [void]$udp.Send($payload, $payload.Length, $ep)
                } catch {}
            }

            $deadline = [DateTime]::UtcNow.AddMilliseconds(700)
            while ([DateTime]::UtcNow -lt $deadline) {
                if ($udp.Available -gt 0) {
                    $remote = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
                    $bytes = $udp.Receive([ref]$remote)
                    try {
                        $o = ([System.Text.Encoding]::UTF8.GetString($bytes)) | ConvertFrom-Json
                        if ([string]$o.magic -eq "LUMITOOL_PRINTSEVER") {
                            $ip = [string]$remote.Address
                            $id = ([string]$o.id).ToUpper()
                            $uid = ([string]$o.uid).ToUpper() -replace '[^0-9A-F]', ''
                            if (-not $uid) {
                                $uid = ([string]$o.mac).ToUpper() -replace '[^0-9A-F]', ''
                            }

                            $found[$ip] = [pscustomobject]@{
                                IP = $ip
                                ID = $id
                                UID = $uid
                                Name = [string]$o.device_name
                                Version = [string]$o.version
                            }
                        }
                    } catch {}
                } else {
                    Start-Sleep -Milliseconds 25
                }
            }
        } catch {
        } finally {
            if ($udp) {
                try { $udp.Close() } catch {}
            }
        }
    }

    return @($found.Values)
}

function Get-PortIdentity {
    param([string]$PortName)

    if ($PortName -match '^LUMITOOL_([0-9A-Fa-f]{4}|[0-9A-Fa-f]{12})_([ABC])_(9101|9102|9103)$') {
        return [pscustomobject]@{
            Key = $Matches[1].ToUpper()
            Letter = $Matches[2].ToUpper()
            TcpPort = [int]$Matches[3]
        }
    }

    return $null
}

function Find-DeviceForPort {
    param(
        [string]$Key,
        [object[]]$Devices
    )

    if ([string]::IsNullOrWhiteSpace($Key)) { return $null }

    if ($Key.Length -eq 12) {
        return @($Devices | Where-Object { ([string]$_.UID).ToUpper() -eq $Key }) | Select-Object -First 1
    }

    $matches = @($Devices | Where-Object { ([string]$_.ID).ToUpper() -eq $Key })
    if ($matches.Count -gt 1) {
        Log ("IDENTITY COLLISION short-id=" + $Key + " devices=" + $matches.Count + " - legacy port not auto-repaired")
        return $null
    }

    return $matches | Select-Object -First 1
}

function Repair-TcpPorts {
    param([object[]]$Changes)

    $items = @($Changes | Where-Object { $_ -and $_.PortName -and $_.IP } | Sort-Object PortName -Unique)
    if ($items.Count -eq 0) { return }

    Log ("PORT SELF-HEAL: " + $items.Count + " stale port(s); restarting spooler once")
    $stopped = $false

    try {
        Stop-Service Spooler -Force -ErrorAction Stop
        $stopped = $true
        Start-Sleep -Milliseconds 350

        foreach ($x in $items) {
            $path = "HKLM:\SYSTEM\CurrentControlSet\Control\Print\Monitors\Standard TCP/IP Port\Ports\" + [string]$x.PortName

            if (-not (Test-Path $path)) {
                Log ("PORT SELF-HEAL missing registry path: " + [string]$x.PortName)
                continue
            }

            Set-ItemProperty -Path $path -Name "HostName" -Value ([string]$x.IP) -ErrorAction Stop
            Set-ItemProperty -Path $path -Name "IPAddress" -Value ([string]$x.IP) -ErrorAction Stop

            try {
                Set-ItemProperty -Path $path -Name "PortNumber" -Value ([int]$x.TcpPort) -ErrorAction SilentlyContinue
            } catch {}

            try {
                if (Get-ItemProperty -Path $path -Name "SNMP Enabled" -ErrorAction SilentlyContinue) {
                    Set-ItemProperty -Path $path -Name "SNMP Enabled" -Value 0 -ErrorAction SilentlyContinue
                }
            } catch {}

            Log ("PORT SELF-HEAL " + [string]$x.PortName + " " + [string]$x.OldIP + " -> " + [string]$x.IP)
        }
    } catch {
        Log ("PORT SELF-HEAL ERROR: " + $_.Exception.Message)
    } finally {
        try {
            if ($stopped) {
                Start-Service Spooler -ErrorAction Stop
                Start-Sleep -Milliseconds 500
            }
        } catch {
            Log ("PORT SELF-HEAL spooler start ERROR: " + $_.Exception.Message)
        }
    }
}

function Get-EspJobs {
    param([string]$IP)
    try {
        return Invoke-RestMethod -Uri ("http://" + $IP + "/api/jobs") -Method Get -TimeoutSec 1 -ErrorAction Stop
    } catch {
        return $null
    }
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
    } catch {
    } finally {
        if ($udp) {
            try { $udp.Close() } catch {}
        }
    }
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
            $jobs = @(Get-CimInstance -ClassName Win32_PrintJob -ErrorAction Stop | Where-Object {
                [uint32]$_.JobId -eq $JobId -and [string]$_.Name -like ($PrinterName + ",*")
            })

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
$lastSummaryLog = [DateTime]::MinValue
$nextRepair = [DateTime]::MinValue

Log "WATCHER START V3 MULTI-DEVICE AUTO-IP SELF-HEAL"

while ($true) {
    try {
        $queues = @(
            Get-Printer -ErrorAction SilentlyContinue |
            Where-Object { ([string]$_.PortName).StartsWith("LUMITOOL_") }
        )

        if ((Get-Date) -ge $nextRepair) {
            $devices = @(Find-LumitoolDevices)
            $repairs = @()
            $seenPorts = @{}

            foreach ($q in $queues) {
                $portName = [string]$q.PortName
                if ($seenPorts.ContainsKey($portName)) { continue }
                $seenPorts[$portName] = $true

                $identity = Get-PortIdentity -PortName $portName
                if (-not $identity) { continue }

                $port = Get-PrinterPort -Name $portName -ErrorAction SilentlyContinue
                if (-not $port) { continue }

                $match = Find-DeviceForPort -Key ([string]$identity.Key) -Devices $devices
                if (-not $match) {
                    # Keep the old IP when the device is powered off or temporarily unreachable.
                    continue
                }

                $oldIp = [string]$port.PrinterHostAddress
                $newIp = [string]$match.IP

                if ($newIp -and $oldIp -ne $newIp) {
                    $repairs += [pscustomobject]@{
                        PortName = $portName
                        OldIP = $oldIp
                        IP = $newIp
                        TcpPort = [int]$identity.TcpPort
                    }
                }
            }

            Repair-TcpPorts -Changes $repairs
            $nextRepair = (Get-Date).AddSeconds(15)
        }

        if ((Get-Date) - $lastSummaryLog -gt [TimeSpan]::FromMinutes(2)) {
            Log ("AUTO-DISCOVERY: " + $queues.Count + " Lumitool queue(s); supports multiple print servers / PCs")
            $lastSummaryLog = Get-Date
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
            if ($tcp -eq 0 -and $portName -match "_(9101|9102|9103)$") {
                $tcp = [int]$Matches[1]
            }

            $letter = Get-LetterFromPort -Port $tcp
            if (-not $ip -or -not $letter) { continue }

            if (-not $apiCache.ContainsKey($ip)) { $apiCache[$ip] = Get-EspJobs -IP $ip }
            if (-not $routeIpCache.ContainsKey($ip)) { $routeIpCache[$ip] = Get-RouteLocalIP -RemoteIP $ip }

            $api = $apiCache[$ip]
            $esp = Get-EspPrinterState -Api $api -Letter $letter
            $localRouteIp = [string]$routeIpCache[$ip]

            $jobs = @()
            try {
                $jobs = @(Get-PrintJob -PrinterName $printerName -ErrorAction Stop)
            } catch {
                continue
            }

            foreach ($job in $jobs) {
                $jobId = [uint32]$job.ID
                $key = $printerName + "|" + $jobId
                $liveKeys[$key] = $true

                if (-not $states.ContainsKey($key)) {
                    $baseline = 0
                    if ($esp -and $esp.last -and $esp.last.id) {
                        $baseline = [uint32]$esp.last.id
                    }

                    $states[$key] = [pscustomobject]@{
                        FirstSeen = (Get-Date)
                        BaselineDoneId = $baseline
                        SawActive = $false
                        CandidateSince = $null
                        LastEspId = 0
                    }

                    Log ("WIN JOB seen printer=" + $printerName + " id=" + $jobId + " baselineEsp=" + $baseline + " routeIP=" + $localRouteIp)
                }

                $st = $states[$key]

                if (-not $esp) {
                    $st.CandidateSince = $null
                    continue
                }

                if ([int]$esp.active -gt 0) {
                    if (-not $st.SawActive) {
                        Log ("ESP ACTIVE printer=" + $letter + " for winJob=" + $jobId)
                    }
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

                if (-not $confirmed) {
                    $st.CandidateSince = $null
                    continue
                }

                if (-not $st.CandidateSince -or $st.LastEspId -ne $lastId) {
                    $st.CandidateSince = Get-Date
                    $st.LastEspId = $lastId
                    Log ("ESP DONE candidate printer=" + $letter + " winJob=" + $jobId + " espJob=" + $lastId + " source=" + $sourceIp + " route=" + $localRouteIp)
                    continue
                }

                if ((Get-Date) - $st.CandidateSince -lt [TimeSpan]::FromMilliseconds(600)) {
                    continue
                }

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

        foreach ($key in @($states.Keys)) {
            if (-not $liveKeys.ContainsKey($key)) {
                $states.Remove($key)
            }
        }
    } catch {
        Log ("LOOP ERROR: " + $_.Exception.Message)
    }

    Start-Sleep -Milliseconds 400
}
