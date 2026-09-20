param()

$ErrorActionPreference = "Continue"

$BaseDir = Join-Path $env:ProgramData "Lumitool\Printsever"
$ConfigPath = Join-Path $BaseDir "queues.json"
$LogPath = Join-Path $BaseDir "job-watcher.log"

New-Item -ItemType Directory -Path $BaseDir -Force | Out-Null

function Log {
    param([string]$Text)
    try {
        Add-Content -Path $LogPath -Value ("[{0}] {1}" -f (Get-Date -Format "yyyy-MM-dd HH:mm:ss.fff"), $Text) -Encoding UTF8
    } catch {}
}

function Get-LocalIPv4Set {
    $set = @{}
    try {
        foreach ($a in @(Get-NetIPAddress -AddressFamily IPv4 -ErrorAction Stop)) {
            $ip = [string]$a.IPAddress
            if ($ip -and $ip -notmatch "^127\." -and $ip -notmatch "^169\.254\.") {
                $set[$ip] = $true
            }
        }
    } catch {}
    return $set
}

function Get-EspJobs {
    param([string]$IP)
    try {
        return Invoke-RestMethod -Uri ("http://" + $IP + "/api/jobs") -Method Get -TimeoutSec 2 -ErrorAction Stop
    } catch {
        return $null
    }
}

function Get-PrinterApiState {
    param($Api,[string]$Letter)
    if (-not $Api -or -not $Api.printers) { return $null }
    try {
        $prop = $Api.printers.PSObject.Properties[$Letter]
        if ($prop) { return $prop.Value }
    } catch {}
    return $null
}

function Job-Key {
    param([string]$PrinterName,[uint32]$JobId)
    return ($PrinterName + "|" + [string]$JobId)
}

Import-Module PrintManagement -ErrorAction SilentlyContinue

$states = @{}
$lastConfigStamp = [DateTime]::MinValue
$config = @()
$localIps = Get-LocalIPv4Set
$lastLocalIpRefresh = Get-Date

Log "WATCHER START V1"

while ($true) {
    try {
        if ((Get-Date) - $lastLocalIpRefresh -gt [TimeSpan]::FromSeconds(30)) {
            $localIps = Get-LocalIPv4Set
            $lastLocalIpRefresh = Get-Date
        }

        if (Test-Path $ConfigPath) {
            $stamp = (Get-Item $ConfigPath).LastWriteTimeUtc
            if ($stamp -ne $lastConfigStamp) {
                try {
                    $raw = Get-Content $ConfigPath -Raw -ErrorAction Stop
                    $config = @()
                    if (-not [string]::IsNullOrWhiteSpace($raw)) {
                        $config = @($raw | ConvertFrom-Json)
                    }
                    $lastConfigStamp = $stamp
                    Log ("CONFIG loaded: " + $config.Count + " queue(s)")
                } catch {
                    Log ("CONFIG ERROR: " + $_.Exception.Message)
                }
            }
        }

        $apiCache = @{}
        $liveKeys = @{}

        foreach ($q in @($config)) {
            if (-not $q) { continue }

            $printerName = [string]$q.PrinterName
            $ip = [string]$q.IP
            $letter = [string]$q.Letter

            if ([string]::IsNullOrWhiteSpace($printerName) -or [string]::IsNullOrWhiteSpace($ip) -or [string]::IsNullOrWhiteSpace($letter)) {
                continue
            }

            if (-not $apiCache.ContainsKey($ip)) {
                $apiCache[$ip] = Get-EspJobs -IP $ip
            }

            $api = $apiCache[$ip]
            $esp = Get-PrinterApiState -Api $api -Letter $letter
            $jobs = @()

            try {
                $jobs = @(Get-PrintJob -PrinterName $printerName -ErrorAction Stop)
            } catch {
                continue
            }

            foreach ($job in $jobs) {
                $jobId = [uint32]$job.ID
                $key = Job-Key -PrinterName $printerName -JobId $jobId
                $liveKeys[$key] = $true

                if (-not $states.ContainsKey($key)) {
                    $baseline = 0
                    if ($esp -and $esp.last -and $esp.last.id) {
                        $baseline = [uint32]$esp.last.id
                    }

                    $states[$key] = [pscustomobject]@{
                        FirstSeen      = Get-Date
                        BaselineDoneId = $baseline
                        SawEspActive   = $false
                        CandidateSince = $null
                    }

                    Log ("WIN JOB seen printer='" + $printerName + "' id=" + $jobId + " baselineEsp=" + $baseline)
                }

                $st = $states[$key]

                if (-not $esp) {
                    $st.CandidateSince = $null
                    continue
                }

                if ([int]$esp.active -gt 0) {
                    $st.SawEspActive = $true
                    $st.CandidateSince = $null
                    continue
                }

                if ([bool]$esp.runtime_error) {
                    $st.CandidateSince = $null
                    continue
                }

                if (-not $esp.last -or -not [bool]$esp.last.success) {
                    $st.CandidateSince = $null
                    continue
                }

                $lastId = [uint32]$esp.last.id
                $sourceIp = [string]$esp.last.source_ip
                $sourceMatches = $false

                if ($sourceIp -and $localIps.ContainsKey($sourceIp)) {
                    $sourceMatches = $true
                }

                if (-not $sourceMatches) {
                    $st.CandidateSince = $null
                    continue
                }

                $newCompletion = $lastId -gt [uint32]$st.BaselineDoneId

                $recentCompletion = $false
                try {
                    $ageMs = [int64]$api.uptime_ms - [int64]$esp.last.finished_ms
                    $recentCompletion = ($ageMs -ge 0 -and $ageMs -le 30000)
                } catch {}

                $youngWindowsJob = ((Get-Date) - $st.FirstSeen) -le [TimeSpan]::FromMinutes(2)

                $confirmed = $st.SawEspActive -or $newCompletion -or ($recentCompletion -and $youngWindowsJob)

                if (-not $confirmed) {
                    $st.CandidateSince = $null
                    continue
                }

                if (-not $st.CandidateSince) {
                    $st.CandidateSince = Get-Date
                    Log ("ESP DONE candidate printer='" + $printerName + "' winJob=" + $jobId + " espJob=" + $lastId)
                    continue
                }

                if ((Get-Date) - $st.CandidateSince -lt [TimeSpan]::FromMilliseconds(1500)) {
                    continue
                }

                try {
                    Remove-PrintJob -PrinterName $printerName -ID $jobId -Confirm:$false -ErrorAction Stop
                    Log ("WINDOWS JOB CLEARED printer='" + $printerName + "' winJob=" + $jobId + " after ESP DONE #" + $lastId)
                    $states.Remove($key)
                } catch {
                    Log ("REMOVE ERROR printer='" + $printerName + "' winJob=" + $jobId + ": " + $_.Exception.Message)
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

    Start-Sleep -Milliseconds 750
}
