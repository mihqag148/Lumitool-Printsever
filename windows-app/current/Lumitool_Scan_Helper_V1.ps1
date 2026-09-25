param(
    [Parameter(Mandatory=$true)][string]$ResultPath,
    [Parameter(Mandatory=$true)][string]$ProgressPath,
    [string]$CachePath = ""
)

$ErrorActionPreference = "SilentlyContinue"

function Write-State {
    param([int]$Percent, [string]$Text)

    try {
        [pscustomobject]@{
            percent = $Percent
            text = $Text
        } | ConvertTo-Json -Compress | Set-Content -Path $ProgressPath -Encoding UTF8
    } catch {}
}

function Test-Lumitool {
    param([string]$IP)

    try {
        $r = Invoke-WebRequest -Uri ("http://" + $IP + "/device-info") -UseBasicParsing -TimeoutSec 1 -ErrorAction Stop
        if ($r.StatusCode -eq 200) {
            $o = $r.Content | ConvertFrom-Json
            if ([string]$o.magic -eq "LUMITOOL_PRINTSEVER") {
                $id = ([string]$o.id).ToUpper()
                $uid = ([string]$o.uid).ToUpper() -replace '[^0-9A-F]', ''
                if (-not $uid) {
                    $uid = ([string]$o.mac).ToUpper() -replace '[^0-9A-F]', ''
                }

                return [pscustomobject]@{
                    IP = [string]$IP
                    Name = ("Lumitool-Printsever-" + $id)
                    ID = $id
                    UID = $uid
                    Strong = $true
                }
            }
        }
    } catch {}

    try {
        $r = Invoke-WebRequest -Uri ("http://" + $IP + "/") -UseBasicParsing -TimeoutSec 1 -ErrorAction Stop
        if (
            $r.StatusCode -eq 200 -and
            (
                $r.Content -match "Lumitool Printsever" -or
                $r.Content -match "Lumitool-Printsever" -or
                $r.Content -match "ESP Print Server"
            )
        ) {
            $name = "Lumitool-Printsever @ " + $IP

            if ($r.Content -match '(?:Lumitool Printsever|Lumitool-Printsever|ESP Print Server)[-\s]+([0-9A-Fa-f]{4})') {
                $name = "Lumitool-Printsever-" + $Matches[1].ToUpper()
            }

            $id = ""
            if ($name -match '([0-9A-Fa-f]{4})$') {
                $id = $Matches[1].ToUpper()
            }

            return [pscustomobject]@{
                IP = [string]$IP
                Name = $name
                ID = $id
                UID = ""
                Strong = $false
            }
        }
    } catch {}

    return $null
}

$found = @()
$foundSeen = @{}

function Add-Found {
    param($Device)

    if (-not $Device) { return }

    $ip = [string]$Device.IP
    if ([string]::IsNullOrWhiteSpace($ip)) { return }

    if (-not $foundSeen.ContainsKey($ip)) {
        $foundSeen[$ip] = $true
        $script:found += $Device
    }
}

Write-State 2 "Kiểm tra mạng setup và IP đã lưu..."

Add-Found (Test-Lumitool -IP "192.168.10.1")

if ($CachePath -and (Test-Path $CachePath)) {
    try {
        $cacheRaw = Get-Content $CachePath -Raw
        if (-not [string]::IsNullOrWhiteSpace($cacheRaw)) {
            foreach ($x in @($cacheRaw | ConvertFrom-Json)) {
                if ($x.IP) {
                    Add-Found (Test-Lumitool -IP ([string]$x.IP))
                }
            }
        }
    } catch {}
}

$adapters = @()
try {
    $adapters = @(
        Get-NetIPAddress -AddressFamily IPv4 |
        Where-Object {
            $_.IPAddress -notmatch "^127\." -and
            $_.IPAddress -notmatch "^169\.254\." -and
            $_.AddressState -ne "Duplicate"
        }
    )
} catch {}

$bases = @{}
foreach ($a in $adapters) {
    $ip = [string]$a.IPAddress
    if ($ip -match '^(\d+)\.(\d+)\.(\d+)\.\d+$') {
        $bases[($Matches[1] + "." + $Matches[2] + "." + $Matches[3])] = $true
    }
}
$bases["192.168.10"] = $true

Write-State 10 "UDP discovery trên các card mạng..."

$udpSeen = @{}
foreach ($a in $adapters) {
    $ip = [string]$a.IPAddress
    if ($ip -notmatch '^(\d+)\.(\d+)\.(\d+)\.\d+$') { continue }

    $broadcast = $Matches[1] + "." + $Matches[2] + "." + $Matches[3] + ".255"
    $udp = $null

    try {
        $udp = New-Object System.Net.Sockets.UdpClient
        $udp.EnableBroadcast = $true
        $udp.Client.Bind((New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Parse($ip), 0)))
        $payload = [System.Text.Encoding]::ASCII.GetBytes("LUMITOOL_DISCOVER_V1")

        foreach ($target in @($broadcast, "255.255.255.255")) {
            try {
                $ep = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Parse($target), 4210)
                [void]$udp.Send($payload, $payload.Length, $ep)
            } catch {}
        }

        $until = [DateTime]::UtcNow.AddMilliseconds(400)
        while ([DateTime]::UtcNow -lt $until) {
            if ($udp.Available -gt 0) {
                $remote = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
                $bytes = $udp.Receive([ref]$remote)
                $o = ([System.Text.Encoding]::UTF8.GetString($bytes)) | ConvertFrom-Json

                if ([string]$o.magic -eq "LUMITOOL_PRINTSEVER") {
                    $foundIp = [string]$remote.Address
                    if (-not $udpSeen.ContainsKey($foundIp)) {
                        $udpSeen[$foundIp] = $true
                        $id = ([string]$o.id).ToUpper()
                        $uid = ([string]$o.uid).ToUpper() -replace '[^0-9A-F]', ''
                        if (-not $uid) {
                            $uid = ([string]$o.mac).ToUpper() -replace '[^0-9A-F]', ''
                        }

                        Add-Found ([pscustomobject]@{
                            IP = $foundIp
                            Name = ("Lumitool-Printsever-" + $id)
                            ID = $id
                            UID = $uid
                            Strong = $true
                        })
                    }
                }
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

$candidates = New-Object System.Collections.ArrayList
$candidateSeen = @{}

function Add-Candidate {
    param([string]$IP)

    if ([string]::IsNullOrWhiteSpace($IP)) { return }

    if (-not $candidateSeen.ContainsKey($IP)) {
        $candidateSeen[$IP] = $true
        [void]$candidates.Add($IP)
    }
}

foreach ($base in @($bases.Keys)) {
    for ($i = 1; $i -le 254; $i++) {
        Add-Candidate ($base + "." + $i)
    }
}

try {
    foreach ($n in @(Get-NetNeighbor -AddressFamily IPv4)) {
        if ($n.IPAddress -and $n.State -ne "Unreachable") {
            Add-Candidate ([string]$n.IPAddress)
        }
    }
} catch {}

$total = [Math]::Max(1, $candidates.Count)
$batchSize = 64
$checked = 0

for ($offset = 0; $offset -lt $candidates.Count; $offset += $batchSize) {
    $pending = @()
    $last = [Math]::Min($offset + $batchSize - 1, $candidates.Count - 1)

    for ($idx = $offset; $idx -le $last; $idx++) {
        $targetIp = [string]$candidates[$idx]
        $client = New-Object System.Net.Sockets.TcpClient

        try {
            $async = $client.BeginConnect($targetIp, 80, $null, $null)
            $pending += [pscustomobject]@{
                IP = $targetIp
                Client = $client
                Async = $async
            }
        } catch {
            try { $client.Close() } catch {}
        }
    }

    Start-Sleep -Milliseconds 250

    foreach ($p in $pending) {
        try {
            if ($p.Async.IsCompleted) {
                $p.Client.EndConnect($p.Async)

                if ($p.Client.Connected) {
                    Add-Found (Test-Lumitool -IP ([string]$p.IP))
                }
            }
        } catch {
        } finally {
            try { $p.Client.Close() } catch {}
        }

        $checked++
    }

    $pct = 20 + [Math]::Min(78, [int](78 * $checked / $total))
    Write-State $pct ("Quét IP " + [Math]::Min($checked, $total) + "/" + $total)
}

try {
    @($found) | ConvertTo-Json -Depth 4 | Set-Content -Path $ResultPath -Encoding UTF8
} catch {
    "[]" | Set-Content -Path $ResultPath -Encoding UTF8
}

Write-State 100 ("Hoàn tất - tìm thấy " + @($found).Count + " thiết bị")
) {
                $id = $Matches[1].ToUpper()
            }

            return [pscustomobject]@{
                IP = [string]$IP
                Name = $name
                ID = $id
                UID = ""
                Strong = $false
            }
        }
    } catch {}

    return $null
}

$found = @()
$foundSeen = @{}

function Add-Found {
    param($Device)

    if (-not $Device) { return }

    $ip = [string]$Device.IP
    if ([string]::IsNullOrWhiteSpace($ip)) { return }

    if (-not $foundSeen.ContainsKey($ip)) {
        $foundSeen[$ip] = $true
        $script:found += $Device
    }
}

Write-State 2 "Kiểm tra mạng setup và IP đã lưu..."

Add-Found (Test-Lumitool -IP "192.168.10.1")

if ($CachePath -and (Test-Path $CachePath)) {
    try {
        $cacheRaw = Get-Content $CachePath -Raw
        if (-not [string]::IsNullOrWhiteSpace($cacheRaw)) {
            foreach ($x in @($cacheRaw | ConvertFrom-Json)) {
                if ($x.IP) {
                    Add-Found (Test-Lumitool -IP ([string]$x.IP))
                }
            }
        }
    } catch {}
}

$adapters = @()
try {
    $adapters = @(
        Get-NetIPAddress -AddressFamily IPv4 |
        Where-Object {
            $_.IPAddress -notmatch "^127\." -and
            $_.IPAddress -notmatch "^169\.254\." -and
            $_.AddressState -ne "Duplicate"
        }
    )
} catch {}

$bases = @{}
foreach ($a in $adapters) {
    $ip = [string]$a.IPAddress
    if ($ip -match '^(\d+)\.(\d+)\.(\d+)\.\d+$') {
        $bases[($Matches[1] + "." + $Matches[2] + "." + $Matches[3])] = $true
    }
}
$bases["192.168.10"] = $true

Write-State 10 "UDP discovery trên các card mạng..."

$udpSeen = @{}
foreach ($a in $adapters) {
    $ip = [string]$a.IPAddress
    if ($ip -notmatch '^(\d+)\.(\d+)\.(\d+)\.\d+$') { continue }

    $broadcast = $Matches[1] + "." + $Matches[2] + "." + $Matches[3] + ".255"
    $udp = $null

    try {
        $udp = New-Object System.Net.Sockets.UdpClient
        $udp.EnableBroadcast = $true
        $udp.Client.Bind((New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Parse($ip), 0)))
        $payload = [System.Text.Encoding]::ASCII.GetBytes("LUMITOOL_DISCOVER_V1")

        foreach ($target in @($broadcast, "255.255.255.255")) {
            try {
                $ep = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Parse($target), 4210)
                [void]$udp.Send($payload, $payload.Length, $ep)
            } catch {}
        }

        $until = [DateTime]::UtcNow.AddMilliseconds(400)
        while ([DateTime]::UtcNow -lt $until) {
            if ($udp.Available -gt 0) {
                $remote = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
                $bytes = $udp.Receive([ref]$remote)
                $o = ([System.Text.Encoding]::UTF8.GetString($bytes)) | ConvertFrom-Json

                if ([string]$o.magic -eq "LUMITOOL_PRINTSEVER") {
                    $foundIp = [string]$remote.Address
                    if (-not $udpSeen.ContainsKey($foundIp)) {
                        $udpSeen[$foundIp] = $true
                        Add-Found ([pscustomobject]@{
                            IP = $foundIp
                            Name = ("Lumitool-Printsever-" + [string]$o.id)
                            Strong = $true
                        })
                    }
                }
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

$candidates = New-Object System.Collections.ArrayList
$candidateSeen = @{}

function Add-Candidate {
    param([string]$IP)

    if ([string]::IsNullOrWhiteSpace($IP)) { return }

    if (-not $candidateSeen.ContainsKey($IP)) {
        $candidateSeen[$IP] = $true
        [void]$candidates.Add($IP)
    }
}

foreach ($base in @($bases.Keys)) {
    for ($i = 1; $i -le 254; $i++) {
        Add-Candidate ($base + "." + $i)
    }
}

try {
    foreach ($n in @(Get-NetNeighbor -AddressFamily IPv4)) {
        if ($n.IPAddress -and $n.State -ne "Unreachable") {
            Add-Candidate ([string]$n.IPAddress)
        }
    }
} catch {}

$total = [Math]::Max(1, $candidates.Count)
$batchSize = 64
$checked = 0

for ($offset = 0; $offset -lt $candidates.Count; $offset += $batchSize) {
    $pending = @()
    $last = [Math]::Min($offset + $batchSize - 1, $candidates.Count - 1)

    for ($idx = $offset; $idx -le $last; $idx++) {
        $targetIp = [string]$candidates[$idx]
        $client = New-Object System.Net.Sockets.TcpClient

        try {
            $async = $client.BeginConnect($targetIp, 80, $null, $null)
            $pending += [pscustomobject]@{
                IP = $targetIp
                Client = $client
                Async = $async
            }
        } catch {
            try { $client.Close() } catch {}
        }
    }

    Start-Sleep -Milliseconds 250

    foreach ($p in $pending) {
        try {
            if ($p.Async.IsCompleted) {
                $p.Client.EndConnect($p.Async)

                if ($p.Client.Connected) {
                    Add-Found (Test-Lumitool -IP ([string]$p.IP))
                }
            }
        } catch {
        } finally {
            try { $p.Client.Close() } catch {}
        }

        $checked++
    }

    $pct = 20 + [Math]::Min(78, [int](78 * $checked / $total))
    Write-State $pct ("Quét IP " + [Math]::Min($checked, $total) + "/" + $total)
}

try {
    @($found) | ConvertTo-Json -Depth 4 | Set-Content -Path $ResultPath -Encoding UTF8
} catch {
    "[]" | Set-Content -Path $ResultPath -Encoding UTF8
}

Write-State 100 ("Hoàn tất - tìm thấy " + @($found).Count + " thiết bị")
