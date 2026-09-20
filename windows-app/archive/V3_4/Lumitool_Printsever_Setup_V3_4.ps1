$ErrorActionPreference = "Stop"

$LogPath = Join-Path $env:TEMP "Lumitool_Printsever_Setup_V3_4.log"

function Write-DebugLog {
    param([string]$Text)
    try {
        Add-Content -Path $LogPath -Value ("[{0}] {1}" -f (Get-Date -Format "yyyy-MM-dd HH:mm:ss"), $Text) -Encoding UTF8
    } catch {}
}

try {
    Add-Type -AssemblyName System.Windows.Forms
    Add-Type -AssemblyName System.Drawing
    [System.Windows.Forms.Application]::EnableVisualStyles()

    Write-DebugLog "=== START V3.4 ==="
    Write-DebugLog ("PowerShell: " + $PSVersionTable.PSVersion.ToString())

    Import-Module PrintManagement -ErrorAction Stop
    Write-DebugLog "PrintManagement loaded"

    $script:CurrentIP = ""
    $script:CurrentName = ""
    $script:CurrentHtml = ""
    $script:DriverNames = @()

    $script:CacheDir = Join-Path $env:LOCALAPPDATA "Lumitool\Printsever"
    $script:CachePath = Join-Path $script:CacheDir "devices.json"
    $script:ScanResults = @()
    $script:ScanSeen = @{}
    $script:ScanProcess = $null
    $script:ScanResultPath = ""
    $script:ScanProgressPath = ""
    $script:ScanStartedAt = $null

    function UiLog {
        param([string]$Text)
        Write-DebugLog $Text
        if ($script:txtLog) {
            $script:txtLog.AppendText(("[" + (Get-Date -Format "HH:mm:ss") + "] " + $Text + "`r`n"))
            $script:txtLog.SelectionStart = $script:txtLog.TextLength
            $script:txtLog.ScrollToCaret()
            [System.Windows.Forms.Application]::DoEvents()
        }
    }

    function Get-EspHtml {
        param([string]$IP)

        try {
            $r = Invoke-WebRequest `
                -Uri ("http://" + $IP + "/") `
                -UseBasicParsing `
                -TimeoutSec 2 `
                -ErrorAction Stop

            if (
                $r.StatusCode -eq 200 -and
                (
                    $r.Content -match "Lumitool Printsever" -or
                    $r.Content -match "ESP Print Server"
                )
            ) {
                return [string]$r.Content
            }
        } catch {}

        return $null
    }

    function Get-NameFromHtml {
        param(
            [string]$Html,
            [string]$IP
        )

        if (
            $Html -match '(?:Lumitool Printsever|ESP Print Server)\s+([0-9A-Fa-f]{4})'
        ) {
            return (
                "Lumitool-Printsever-" +
                $Matches[1].ToUpper()
            )
        }

        return (
            "Lumitool-Printsever @ " +
            $IP
        )
    }

    function Get-LumitoolIdentity {
        param([string]$IP)

        # Firmware V7.4+: exact identity endpoint.
        try {
            $r = Invoke-WebRequest `
                -Uri ("http://" + $IP + "/device-info") `
                -UseBasicParsing `
                -TimeoutSec 2 `
                -ErrorAction Stop

            if ($r.StatusCode -eq 200) {
                $o = $r.Content | ConvertFrom-Json

                if (
                    [string]$o.magic -eq "LUMITOOL_PRINTSEVER" -and
                    [string]$o.product -eq "Lumitool Printsever"
                ) {
                    return [pscustomobject]@{
                        IP = [string]$IP
                        Name = (
                            "Lumitool-Printsever-" +
                            [string]$o.id
                        )
                        Strong = $true
                    }
                }
            }
        } catch {}

        # Firmware cũ: nhận diện bằng chữ ký web.
        $html = Get-EspHtml -IP $IP

        if ($html) {
            return [pscustomobject]@{
                IP = [string]$IP
                Name = Get-NameFromHtml -Html $html -IP $IP
                Strong = $false
            }
        }

        return $null
    }


    function Get-StatusFromHtml {
        param([string]$Html, [string]$Letter, [int]$TcpPort)

        if ([string]::IsNullOrWhiteSpace($Html)) {
            return "Không đọc được"
        }

        $rx1 = "(?s)PORT\s*" + [regex]::Escape($Letter) + ".*?" + $TcpPort + ".*?<div class='(?:ok|bad|warn)'[^>]*>([^<]+)</div>"
        $m1 = [regex]::Match($Html, $rx1, [Text.RegularExpressions.RegexOptions]::IgnoreCase)
        if ($m1.Success) {
            return [System.Net.WebUtility]::HtmlDecode($m1.Groups[1].Value.Trim())
        }

        $rx2 = "(?s)PORT\s*" + [regex]::Escape($Letter) + ".*?(ONLINE|OFFLINE|SẴN SÀNG|HẾT GIẤY|LỖI|CHƯA SẴN SÀNG)"
        $m2 = [regex]::Match($Html, $rx2, [Text.RegularExpressions.RegexOptions]::IgnoreCase)
        if ($m2.Success) {
            return $m2.Groups[1].Value.Trim()
        }

        return "Không xác định"
    }

    function Paint-Status {
        param([System.Windows.Forms.Label]$Label, [string]$Status)

        $Label.Text = $Status

        if ($Status -match "OFFLINE|LỖI|HẾT GIẤY|Không") {
            $Label.ForeColor = [Drawing.Color]::FromArgb(183,28,28)
        } elseif ($Status -match "ONLINE|SẴN SÀNG|READY") {
            $Label.ForeColor = [Drawing.Color]::FromArgb(20,125,55)
        } else {
            $Label.ForeColor = [Drawing.Color]::FromArgb(178,106,0)
        }
    }

    function Update-Device {
        param([string]$IP, [string]$Html)

        $script:CurrentIP = $IP
        $script:CurrentHtml = $Html
        $script:CurrentName = Get-NameFromHtml -Html $Html -IP $IP

        $txtIP.Text = $IP
        $lblDevice.Text = $script:CurrentName + "  ·  " + $IP

        Paint-Status $lblA (Get-StatusFromHtml $Html "A" 9101)
        Paint-Status $lblB (Get-StatusFromHtml $Html "B" 9102)
        Paint-Status $lblC (Get-StatusFromHtml $Html "C" 9103)

        UiLog ("Đã kết nối " + $script:CurrentName + " - " + $IP)
    }

    function Connect-IP {
        $ip = $txtIP.Text.Trim()

        $tmp = $null

        if (
            -not
            [System.Net.IPAddress]::TryParse(
                $ip,
                [ref]$tmp
            )
        ) {
            [System.Windows.Forms.MessageBox]::Show(
                "IP không hợp lệ.",
                "Lumitool Printsever",
                "OK",
                "Warning"
            ) | Out-Null
            return
        }

        $lblTop.Text =
            "Đang kết nối " +
            $ip +
            "..."

        [System.Windows.Forms.Application]::DoEvents()

        $dev =
            Get-LumitoolIdentity `
            -IP $ip

        if (-not $dev) {
            $lblTop.Text =
                "Không kết nối được"

            UiLog(
                "Không tìm thấy Lumitool Printsever tại " +
                $ip
            )

            [System.Windows.Forms.MessageBox]::Show(
                "Không tìm thấy Lumitool Printsever tại " +
                $ip +
                ".",
                "Không kết nối được",
                "OK",
                "Warning"
            ) | Out-Null
            return
        }

        $html =
            Get-EspHtml `
            -IP $ip

        if (-not $html) {
            $html = ""
        }

        Update-Device `
            -IP $ip `
            -Html $html

        $script:CurrentName =
            $dev.Name

        $lblDevice.Text =
            $dev.Name +
            "  ·  " +
            $ip

        $lblTop.Text =
            "Đã kết nối"
    }

    function Get-ActiveIPv4Adapters {
        $items = @()

        try {
            $ips =
                Get-NetIPAddress `
                    -AddressFamily IPv4 `
                    -ErrorAction Stop |
                Where-Object {
                    $_.IPAddress -notmatch "^127\." -and
                    $_.IPAddress -notmatch "^169\.254\." -and
                    $_.AddressState -ne "Duplicate"
                }

            foreach ($x in $ips) {
                $ip =
                    [string]$x.IPAddress

                if (
                    $ip -match
                    '^(\d+)\.(\d+)\.(\d+)\.\d+$'
                ) {
                    $base =
                        $Matches[1] +
                        "." +
                        $Matches[2] +
                        "." +
                        $Matches[3]

                    $broadcast =
                        $base +
                        ".255"

                    $exists =
                        $false

                    foreach ($old in $items) {
                        if (
                            $old.IP -eq
                            $ip
                        ) {
                            $exists =
                                $true
                            break
                        }
                    }

                    if (-not $exists) {
                        $items +=
                            [pscustomobject]@{
                                IP = $ip
                                Prefix = [int]$x.PrefixLength
                                Base = $base
                                Broadcast = $broadcast
                                InterfaceIndex = $x.InterfaceIndex
                            }
                    }
                }
            }
        } catch {
            UiLog(
                "Đọc card mạng lỗi: " +
                $_.Exception.Message
            )
        }

        return $items
    }

    function Get-LanBases {
        $bases = @()

        foreach (
            $x in @(Get-ActiveIPv4Adapters)
        ) {
            if (
                $bases -notcontains
                $x.Base
            ) {
                $bases +=
                    $x.Base
            }
        }

        # Setup AP network is always checked too.
        if (
            $bases -notcontains
            "192.168.10"
        ) {
            $bases +=
                "192.168.10"
        }

        return $bases
    }

    function Load-CachedDevices {
        if (
            -not
            (Test-Path $script:CachePath)
        ) {
            return @()
        }

        try {
            $raw =
                Get-Content `
                    -Path $script:CachePath `
                    -Raw `
                    -ErrorAction Stop

            if (
                [string]::IsNullOrWhiteSpace(
                    $raw
                )
            ) {
                return @()
            }

            return @(
                $raw |
                ConvertFrom-Json
            )
        } catch {
            return @()
        }
    }

    function Save-DeviceCache {
        param($Device)

        if (-not $Device) {
            return
        }

        try {
            if (
                -not
                (Test-Path $script:CacheDir)
            ) {
                New-Item `
                    -ItemType Directory `
                    -Path $script:CacheDir `
                    -Force |
                    Out-Null
            }

            $id = ""

            if (
                [string]$Device.Name -match
                '([0-9A-Fa-f]{4})$'
            ) {
                $id =
                    $Matches[1].ToUpper()
            }

            $keep = @()

            foreach (
                $x in @(Load-CachedDevices)
            ) {
                if (
                    [string]$x.IP -eq
                    [string]$Device.IP
                ) {
                    continue
                }

                if (
                    $id -and
                    [string]$x.ID -eq
                    $id
                ) {
                    continue
                }

                $keep +=
                    $x
            }

            $keep +=
                [pscustomobject]@{
                    ID = $id
                    IP = [string]$Device.IP
                    Name = [string]$Device.Name
                    LastSeen = (
                        Get-Date
                    ).ToString("s")
                }

            $keep |
                ConvertTo-Json -Depth 3 |
                Set-Content `
                    -Path $script:CachePath `
                    -Encoding UTF8
        } catch {
            UiLog(
                "Lưu IP thiết bị lỗi: " +
                $_.Exception.Message
            )
        }
    }

    function Register-ScanDevice {
        param($Device)

        if (-not $Device) {
            return
        }

        $ip =
            [string]$Device.IP

        if (
            [string]::IsNullOrWhiteSpace(
                $ip
            )
        ) {
            return
        }

        if (
            $script:ScanSeen.ContainsKey(
                $ip
            )
        ) {
            return
        }

        $script:ScanSeen[$ip] =
            $true

        $script:ScanResults +=
            $Device

        Save-DeviceCache `
            -Device $Device
    }

    function Get-NeighborCandidates {
        $out = @()

        try {
            $items =
                Get-NetNeighbor `
                    -AddressFamily IPv4 `
                    -ErrorAction Stop |
                Where-Object {
                    $_.IPAddress -notmatch "^224\." -and
                    $_.IPAddress -notmatch "^255\." -and
                    $_.State -ne "Unreachable"
                }

            foreach ($x in $items) {
                $ip =
                    [string]$x.IPAddress

                if (
                    $out -notcontains
                    $ip
                ) {
                    $out +=
                        $ip
                }
            }
        } catch {}

        return $out
    }

    function Find-LumitoolUdp {
        $results = @()
        $seen = @{}

        $payload =
            [System.Text.Encoding]::ASCII.GetBytes(
                "LUMITOOL_DISCOVER_V1"
            )

        # IMPORTANT:
        # One UDP socket per active adapter.
        # This lets discovery work on Wi-Fi + Ethernet instead of
        # Windows picking only one default interface.
        foreach (
            $adapter in @(Get-ActiveIPv4Adapters)
        ) {
            $udp = $null

            try {
                UiLog(
                    "UDP " +
                    $adapter.IP +
                    " -> " +
                    $adapter.Broadcast
                )

                $localIp =
                    [System.Net.IPAddress]::Parse(
                        [string]$adapter.IP
                    )

                $localEp =
                    New-Object System.Net.IPEndPoint(
                        $localIp,
                        0
                    )

                $udp =
                    New-Object System.Net.Sockets.UdpClient

                $udp.EnableBroadcast =
                    $true

                $udp.Client.Bind(
                    $localEp
                )

                foreach (
                    $targetText in @(
                        [string]$adapter.Broadcast,
                        "255.255.255.255"
                    )
                ) {
                    try {
                        $targetIp =
                            [System.Net.IPAddress]::Parse(
                                $targetText
                            )

                        $targetEp =
                            New-Object System.Net.IPEndPoint(
                                $targetIp,
                                4210
                            )

                        [void]$udp.Send(
                            $payload,
                            [int]$payload.Length,
                            $targetEp
                        )
                    } catch {}
                }

                $deadline =
                    [DateTime]::UtcNow.AddMilliseconds(900)

                while (
                    [DateTime]::UtcNow -lt
                    $deadline
                ) {
                    try {
                        if (
                            $udp.Available -gt
                            0
                        ) {
                            $remote =
                                New-Object System.Net.IPEndPoint(
                                    [System.Net.IPAddress]::Any,
                                    0
                                )

                            $bytes =
                                $udp.Receive(
                                    [ref]$remote
                                )

                            $json =
                                [System.Text.Encoding]::UTF8.GetString($bytes)

                            $o =
                                $json |
                                ConvertFrom-Json

                            if (
                                [string]$o.magic -eq
                                "LUMITOOL_PRINTSEVER"
                            ) {
                                $ip =
                                    [string]$remote.Address

                                if (
                                    -not
                                    $seen.ContainsKey($ip)
                                ) {
                                    $seen[$ip] =
                                        $true

                                    $name =
                                        "Lumitool-Printsever-" +
                                        [string]$o.id

                                    $results +=
                                        [pscustomobject]@{
                                            IP = $ip
                                            Name = $name
                                            Strong = $true
                                        }

                                    UiLog(
                                        "UDP tìm thấy " +
                                        $name +
                                        " @ " +
                                        $ip
                                    )
                                }
                            }
                        } else {
                            Start-Sleep `
                                -Milliseconds 35
                        }
                    } catch {
                        Start-Sleep `
                            -Milliseconds 20
                    }
                }

            } catch {
                UiLog(
                    "UDP trên card " +
                    $adapter.IP +
                    " lỗi: " +
                    $_.Exception.Message
                )
            } finally {
                if ($udp) {
                    try {
                        $udp.Close()
                    } catch {}
                }
            }
        }

        return $results
    }


    function Find-Devices {
        $script:ScanResults = @()
        $script:ScanSeen = @{}

        # Show exactly what networks are being searched.
        foreach (
            $adapter in @(Get-ActiveIPv4Adapters)
        ) {
            UiLog(
                "Mạng đang dùng: " +
                $adapter.IP +
                "/" +
                $adapter.Prefix +
                " -> quét " +
                $adapter.Base +
                ".1-254"
            )
        }

        # 0) Direct setup IP.
        UiLog(
            "Kiểm tra setup IP 192.168.10.1"
        )

        Register-ScanDevice (
            Get-LumitoolIdentity `
                -IP "192.168.10.1"
        )

        # 1) Previously saved IPs.
        foreach (
            $cached in @(Load-CachedDevices)
        ) {
            $ip =
                [string]$cached.IP

            if (
                [string]::IsNullOrWhiteSpace(
                    $ip
                )
            ) {
                continue
            }

            UiLog(
                "Kiểm tra IP đã lưu: " +
                $ip
            )

            Register-ScanDevice (
                Get-LumitoolIdentity `
                    -IP $ip
            )
        }

        # 2) UDP on every active adapter.
        UiLog(
            "UDP discovery trên tất cả card mạng..."
        )

        foreach (
            $d in @(Find-LumitoolUdp)
        ) {
            Register-ScanDevice $d
        }

        # 3) Windows ARP / neighbor table.
        $neighbors =
            @(Get-NeighborCandidates)

        if (
            $neighbors.Count -gt
            0
        ) {
            UiLog(
                "Kiểm tra ARP/Neighbor: " +
                $neighbors.Count +
                " IP"
            )
        }

        foreach ($ip in $neighbors) {
            if (
                $script:ScanSeen.ContainsKey(
                    [string]$ip
                )
            ) {
                continue
            }

            Register-ScanDevice (
                Get-LumitoolIdentity `
                    -IP ([string]$ip)
            )
        }

        # 4) Full /24 fallback for every active network.
        # Also keeps 192.168.10.x in the list for setup.
        foreach (
            $base in @(Get-LanBases)
        ) {
            UiLog(
                "TCP fallback: " +
                $base +
                ".1-254"
            )

            $pending = @()

            for (
                $i = 1;
                $i -le 254;
                $i++
            ) {
                $ip =
                    $base +
                    "." +
                    $i

                if (
                    $script:ScanSeen.ContainsKey(
                        $ip
                    )
                ) {
                    continue
                }

                $c =
                    New-Object System.Net.Sockets.TcpClient

                try {
                    $a =
                        $c.BeginConnect(
                            [string]$ip,
                            [int]80,
                            $null,
                            $null
                        )

                    $pending +=
                        [pscustomobject]@{
                            IP = [string]$ip
                            Client = $c
                            Async = $a
                        }
                } catch {
                    try {
                        $c.Close()
                    } catch {}
                }
            }

            Start-Sleep `
                -Milliseconds 800

            foreach ($p in $pending) {
                try {
                    if (
                        $p.Async.IsCompleted
                    ) {
                        $p.Client.EndConnect(
                            $p.Async
                        )

                        if (
                            $p.Client.Connected
                        ) {
                            $ipKey =
                                [string]$p.IP

                            if (
                                -not
                                $script:ScanSeen.ContainsKey(
                                    $ipKey
                                )
                            ) {
                                $dev =
                                    Get-LumitoolIdentity `
                                        -IP $ipKey

                                if ($dev) {
                                    Register-ScanDevice $dev

                                    UiLog(
                                        "TCP tìm thấy " +
                                        $dev.Name +
                                        " @ " +
                                        $ipKey
                                    )
                                }
                            }
                        }
                    }
                } catch {
                } finally {
                    try {
                        $p.Client.Close()
                    } catch {}
                }
            }

            [System.Windows.Forms.Application]::DoEvents()
        }

        return @(
            $script:ScanResults
        )
    }

    function Show-ScanResults {
        param([object[]]$Devices)

        $devices = @(
            $Devices |
            Where-Object { $_ -and $_.IP } |
            Group-Object IP |
            ForEach-Object { $_.Group[0] }
        )

        if ($devices.Count -eq 0) {
            $lblTop.Text = "Không tìm thấy Lumitool Printsever"
            UiLog "Quét xong: không tìm thấy thiết bị"

            [System.Windows.Forms.MessageBox]::Show(
                "Không tìm thấy Lumitool Printsever.\r\n\r\nĐã kiểm tra mạng setup 192.168.10.x và tất cả mạng IPv4 đang kết nối.",
                "Không tìm thấy",
                "OK",
                "Information"
            ) | Out-Null
            return
        }

        foreach ($d in $devices) {
            Save-DeviceCache -Device $d
        }

        if ($devices.Count -eq 1) {
            $d = $devices[0]
            $html = Get-EspHtml -IP ([string]$d.IP)
            if (-not $html) { $html = "" }

            Update-Device -IP ([string]$d.IP) -Html $html
            $script:CurrentName = [string]$d.Name
            $lblDevice.Text = ([string]$d.Name) + "  ·  " + ([string]$d.IP)
            $lblTop.Text = "Đã tìm thấy"
            return
        }

        $dlg = New-Object Windows.Forms.Form
        $dlg.Text = "Chọn Lumitool Printsever"
        $dlg.Size = New-Object Drawing.Size(500,330)
        $dlg.StartPosition = "CenterParent"

        $list = New-Object Windows.Forms.ListBox
        $list.Location = New-Object Drawing.Point(12,12)
        $list.Size = New-Object Drawing.Size(460,235)

        foreach ($d in $devices) {
            [void]$list.Items.Add(([string]$d.Name) + "    " + ([string]$d.IP))
        }

        if ($list.Items.Count -gt 0) { $list.SelectedIndex = 0 }
        $dlg.Controls.Add($list)

        $ok = New-Object Windows.Forms.Button
        $ok.Text = "Chọn"
        $ok.Location = New-Object Drawing.Point(370,255)
        $ok.Size = New-Object Drawing.Size(100,32)
        $ok.DialogResult = [Windows.Forms.DialogResult]::OK
        $dlg.Controls.Add($ok)
        $dlg.AcceptButton = $ok

        if ($dlg.ShowDialog($form) -eq [Windows.Forms.DialogResult]::OK) {
            $d = $devices[$list.SelectedIndex]
            $html = Get-EspHtml -IP ([string]$d.IP)
            if (-not $html) { $html = "" }

            Update-Device -IP ([string]$d.IP) -Html $html
            $script:CurrentName = [string]$d.Name
            $lblDevice.Text = ([string]$d.Name) + "  ·  " + ([string]$d.IP)
            $lblTop.Text = "Đã chọn thiết bị"
        }
    }

    function Start-AsyncScan {
        if ($script:ScanProcess -and -not $script:ScanProcess.HasExited) {
            return
        }

        $helper = Join-Path $PSScriptRoot "Lumitool_Scan_Helper_V1.ps1"

        if (-not (Test-Path $helper)) {
            [System.Windows.Forms.MessageBox]::Show(
                "Thiếu file Lumitool_Scan_Helper_V1.ps1. Hãy giải nén đầy đủ bộ cài.",
                "Thiếu file",
                "OK",
                "Error"
            ) | Out-Null
            return
        }

        $token = [Guid]::NewGuid().ToString("N")
        $script:ScanResultPath = Join-Path $env:TEMP ("Lumitool_Scan_Result_" + $token + ".json")
        $script:ScanProgressPath = Join-Path $env:TEMP ("Lumitool_Scan_Progress_" + $token + ".json")
        $script:ScanStartedAt = Get-Date

        $btnFind.Enabled = $false
        $btnCancelScan.Enabled = $true
        $scanProgress.Style = "Marquee"
        $scanProgress.MarqueeAnimationSpeed = 20
        $lblTop.Text = "Đang quét nền - giao diện vẫn dùng được..."
        UiLog "Bắt đầu quét nền"

        $args = @(
            "-NoProfile",
            "-ExecutionPolicy", "Bypass",
            "-File", ('"' + $helper + '"'),
            "-ResultPath", ('"' + $script:ScanResultPath + '"'),
            "-ProgressPath", ('"' + $script:ScanProgressPath + '"'),
            "-CachePath", ('"' + $script:CachePath + '"')
        )

        try {
            $script:ScanProcess = Start-Process powershell.exe -ArgumentList $args -WindowStyle Hidden -PassThru
            $scanTimer.Start()
        } catch {
            $btnFind.Enabled = $true
            $btnCancelScan.Enabled = $false
            $scanProgress.Style = "Blocks"
            $scanProgress.Value = 0
            UiLog ("Không chạy được quét nền: " + $_.Exception.Message)
        }
    }

    function Stop-AsyncScan {
        if ($script:ScanProcess -and -not $script:ScanProcess.HasExited) {
            try { $script:ScanProcess.Kill() } catch {}
            $lblTop.Text = "Đã hủy quét"
            UiLog "Đã hủy quét"
        }
    }

    function Load-Drivers {
        $cmbDriver.Items.Clear()

        try {
            $all = @(Get-PrinterDriver -ErrorAction Stop | Sort-Object Name)
            $script:DriverNames = @($all | ForEach-Object { $_.Name })

            foreach ($name in $script:DriverNames) {
                [void]$cmbDriver.Items.Add($name)
            }

            if ($cmbDriver.Items.Count -gt 0) {
                $cmbDriver.SelectedIndex = 0
                $lblDriver.Text = "Đã tìm thấy " + $cmbDriver.Items.Count + " driver đã cài trên Windows"
            } else {
                $lblDriver.Text = "Chưa có driver máy in nào"
            }

            UiLog ("Đã nạp " + $cmbDriver.Items.Count + " driver")
        } catch {
            $lblDriver.Text = "Không đọc được driver"
            UiLog ("Đọc driver lỗi: " + $_.Exception.Message)
        }
    }

    function Filter-Drivers {
        $q = $txtDriverSearch.Text.Trim()

        $cmbDriver.Items.Clear()

        $matches = $script:DriverNames
        if ($q) {
            $matches = @($script:DriverNames | Where-Object { $_ -like ("*" + $q + "*") })
        }

        foreach ($name in $matches) {
            [void]$cmbDriver.Items.Add($name)
        }

        if ($cmbDriver.Items.Count -gt 0) {
            $cmbDriver.SelectedIndex = 0
        }

        $lblDriver.Text = $cmbDriver.Items.Count.ToString() + " driver phù hợp"
    }

    function Find-PrnPortVbs {
        $root =
            Join-Path `
                $env:WINDIR `
                "System32\Printing_Admin_Scripts"

        if (
            -not
            (Test-Path $root)
        ) {
            return $null
        }

        try {
            $f =
                Get-ChildItem `
                    -Path $root `
                    -Filter "prnport.vbs" `
                    -File `
                    -Recurse `
                    -ErrorAction SilentlyContinue |
                Select-Object -First 1

            if ($f) {
                return [string]$f.FullName
            }
        } catch {}

        return $null
    }

    function Find-PrnCfgVbs {
        $root = Join-Path $env:WINDIR "System32\Printing_Admin_Scripts"

        if (-not (Test-Path $root)) {
            return $null
        }

        try {
            $f = Get-ChildItem -Path $root -Filter "prncnfg.vbs" -File -Recurse -ErrorAction SilentlyContinue |
                Select-Object -First 1

            if ($f) {
                return [string]$f.FullName
            }
        } catch {}

        return $null
    }

    function Disable-PortSnmp {
        param([string]$PortName)

        $scriptPath = Find-PrnPortVbs

        if ([string]::IsNullOrWhiteSpace($scriptPath)) {
            UiLog "PORT: không tìm thấy prnport.vbs để tắt SNMP"
            return
        }

        try {
            $args = @(
                "//NoLogo",
                $scriptPath,
                "-t",
                "-r",
                $PortName,
                "-md"
            )

            $output = & cscript.exe @args 2>&1
            $exit = $LASTEXITCODE

            foreach ($line in @($output)) {
                if (-not [string]::IsNullOrWhiteSpace([string]$line)) {
                    UiLog ("PORT/SNMP: " + [string]$line)
                }
            }

            if ($exit -eq 0) {
                UiLog "PORT: SNMP Status = OFF"
            } else {
                UiLog ("PORT: tắt SNMP cảnh báo, exit=" + $exit)
            }
        } catch {
            UiLog ("PORT: tắt SNMP cảnh báo: " + $_.Exception.Message)
        }
    }

    function Disable-PrinterBidi {
        param([string]$PrinterName)

        $scriptPath = Find-PrnCfgVbs

        if ([string]::IsNullOrWhiteSpace($scriptPath)) {
            UiLog "QUEUE: không tìm thấy prncnfg.vbs để tắt BIDI"
            return
        }

        try {
            # Microsoft prncnfg syntax: -enablebidi disables bidirectional printing.
            $args = @(
                "//NoLogo",
                $scriptPath,
                "-t",
                "-P",
                $PrinterName,
                "-enablebidi"
            )

            $output = & cscript.exe @args 2>&1
            $exit = $LASTEXITCODE

            foreach ($line in @($output)) {
                if (-not [string]::IsNullOrWhiteSpace([string]$line)) {
                    UiLog ("QUEUE/BIDI: " + [string]$line)
                }
            }

            if ($exit -eq 0) {
                UiLog "QUEUE: Bidirectional support = OFF"
            } else {
                UiLog ("QUEUE: tắt BIDI cảnh báo, exit=" + $exit)
            }
        } catch {
            UiLog ("QUEUE: tắt BIDI cảnh báo: " + $_.Exception.Message)
        }
    }

    function Configure-RawCompletionMode {
        param(
            [string]$PrinterName,
            [string]$PortName
        )

        # We use TCP socket close as the generic job-complete signal.
        # Disable SNMP/BIDI polling so vendor status monitors do not keep
        # a completed RAW job stuck in "Printing/Waiting".
        Disable-PortSnmp -PortName $PortName
        Disable-PrinterBidi -PrinterName $PrinterName

        try {
            Set-Printer -Name $PrinterName -KeepPrintedJobs $false -Datatype "RAW" -ErrorAction Stop
            UiLog "QUEUE: RAW datatype + KeepPrintedJobs=OFF"
        } catch {
            UiLog ("QUEUE: RAW/KeepPrintedJobs cảnh báo: " + $_.Exception.Message)
        }
    }

    function Ensure-TcpPort {
        param(
            [string]$PortName,
            [string]$IP,
            [int]$TcpPort
        )

        $existing =
            Get-PrinterPort `
                -Name $PortName `
                -ErrorAction SilentlyContinue

        if ($existing) {
            UiLog(
                "PORT: đã tồn tại " +
                $PortName
            )
            return
        }

        UiLog(
            "PORT: tạo Standard TCP/IP " +
            $IP +
            ":" +
            $TcpPort
        )

        # Không truyền tham số SNMP khi tạo bằng Add-PrinterPort.
        # Một số Windows từ chối index SNMP = 0.
        try {
            Add-PrinterPort `
                -Name ([string]$PortName) `
                -PrinterHostAddress ([string]$IP) `
                -PortNumber ([uint32]$TcpPort) `
                -ErrorAction Stop

            UiLog(
                "PORT: Add-PrinterPort OK"
            )

            return

        } catch {
            UiLog(
                "PORT: Add-PrinterPort lỗi: " +
                $_.Exception.Message
            )
        }

        # Fallback: Windows built-in prnport.vbs.
        # RAW custom port + SNMP disabled.
        $scriptPath =
            Find-PrnPortVbs

        if (
            [string]::IsNullOrWhiteSpace(
                $scriptPath
            )
        ) {
            throw (
                "Không tạo được TCP port bằng Add-PrinterPort và " +
                "không tìm thấy prnport.vbs."
            )
        }

        UiLog(
            "PORT: thử fallback prnport.vbs"
        )

        $args = @(
            "//NoLogo",
            $scriptPath,
            "-a",
            "-r",
            $PortName,
            "-h",
            $IP,
            "-o",
            "raw",
            "-n",
            [string]$TcpPort,
            "-md"
        )

        $output =
            & cscript.exe @args 2>&1

        $exit =
            $LASTEXITCODE

        foreach ($line in @($output)) {
            if (
                -not
                [string]::IsNullOrWhiteSpace(
                    [string]$line
                )
            ) {
                UiLog(
                    "PORT/VBS: " +
                    [string]$line
                )
            }
        }

        if ($exit -ne 0) {
            throw (
                "prnport.vbs thất bại, exit code " +
                $exit
            )
        }

        $check =
            Get-PrinterPort `
                -Name $PortName `
                -ErrorAction SilentlyContinue

        if (-not $check) {
            throw (
                "Windows báo tạo port xong nhưng không tìm thấy port " +
                $PortName
            )
        }

        UiLog(
            "PORT: prnport.vbs OK"
        )
    }

    function Install-PrinterFor {
        param(
            [string]$Letter,
            [int]$TcpPort
        )

        if (
            -not
            $script:CurrentIP
        ) {
            [System.Windows.Forms.MessageBox]::Show(
                "Hãy kết nối Lumitool Printsever trước.",
                "Lumitool Printsever",
                "OK",
                "Warning"
            ) | Out-Null
            return
        }

        if (
            -not
            $cmbDriver.SelectedItem
        ) {
            [System.Windows.Forms.MessageBox]::Show(
                "Hãy chọn đúng driver của máy in.`r`n" +
                "Nếu chưa có, cài driver của hãng rồi bấm 'Nạp lại driver'.",
                "Chưa chọn driver",
                "OK",
                "Warning"
            ) | Out-Null
            return
        }

        $driver =
            [string]$cmbDriver.SelectedItem

        $driverObj =
            Get-PrinterDriver `
                -Name $driver `
                -ErrorAction SilentlyContinue

        if (-not $driverObj) {
            [System.Windows.Forms.MessageBox]::Show(
                "Driver này không còn tồn tại trong Windows:`r`n" +
                $driver +
                "`r`n`r`nBấm 'Nạp lại driver' rồi chọn lại.",
                "Driver không hợp lệ",
                "OK",
                "Warning"
            ) | Out-Null
            return
        }

        $suffix =
            "LOCAL"

        if (
            $script:CurrentName -match
            '([0-9A-Fa-f]{4})$'
        ) {
            $suffix =
                $Matches[1].ToUpper()
        }

        $defaultName =
            "Lumitool Printsever " +
            $Letter +
            " - " +
            $driver +
            " (" +
            $suffix +
            ")"

        $name =
            [Microsoft.VisualBasic.Interaction]::InputBox(
                "Tên máy in trên Windows:",
                "Cài Printer " + $Letter,
                $defaultName
            )

        if (
            [string]::IsNullOrWhiteSpace(
                $name
            )
        ) {
            return
        }

        $portName =
            "LUMITOOL_" +
            $suffix +
            "_" +
            $Letter +
            "_" +
            $TcpPort

        try {
            $lblTop.Text =
                "Đang cài Printer " +
                $Letter +
                "..."

            [System.Windows.Forms.Application]::DoEvents()

            UiLog(
                "CÀI " +
                $name
            )

            UiLog(
                "DRIVER: " +
                $driver
            )

            UiLog(
                "RAW TCP " +
                $script:CurrentIP +
                ":" +
                $TcpPort
            )

            UiLog(
                "BƯỚC 1/2: tạo TCP/IP port"
            )

            Ensure-TcpPort `
                -PortName $portName `
                -IP $script:CurrentIP `
                -TcpPort $TcpPort

            $portCheck =
                Get-PrinterPort `
                    -Name $portName `
                    -ErrorAction SilentlyContinue

            if (-not $portCheck) {
                throw (
                    "Không tìm thấy TCP/IP port sau khi tạo."
                )
            }

            UiLog(
                "BƯỚC 1/2: OK - " +
                $portName
            )

            UiLog(
                "BƯỚC 2/2: tạo Windows printer queue"
            )

            $existingPrinter =
                Get-Printer `
                    -Name $name `
                    -ErrorAction SilentlyContinue

            if ($existingPrinter) {
                UiLog(
                    "QUEUE: printer đã có, cập nhật port/driver"
                )

                Set-Printer `
                    -Name $name `
                    -DriverName $driver `
                    -PortName $portName `
                    -ErrorAction Stop

            } else {
                UiLog(
                    "QUEUE: tạo printer mới"
                )

                Add-Printer `
                    -Name $name `
                    -DriverName $driver `
                    -PortName $portName `
                    -ErrorAction Stop
            }

            $printerCheck =
                Get-Printer `
                    -Name $name `
                    -ErrorAction SilentlyContinue

            if (-not $printerCheck) {
                throw (
                    "Windows không tìm thấy printer queue sau khi tạo."
                )
            }

            UiLog "Cấu hình phản hồi hoàn tất RAW: tắt SNMP + BIDI"
            Configure-RawCompletionMode -PrinterName $name -PortName $portName

            UiLog(
                "BƯỚC 2/2: OK"
            )

            $lblTop.Text =
                "Cài thành công"

            UiLog(
                "HOÀN TẤT: " +
                $name
            )

            $ans =
                [System.Windows.Forms.MessageBox]::Show(
                    "Đã cài xong:`r`n" +
                    $name +
                    "`r`n`r`nRAW " +
                    $script:CurrentIP +
                    ":" +
                    $TcpPort +
                    "`r`n`r`nMở Printers & scanners để kiểm tra?",
                    "Thành công",
                    "YesNo",
                    "Information"
                )

            if (
                $ans -eq
                [Windows.Forms.DialogResult]::Yes
            ) {
                Start-Process `
                    "ms-settings:printers"
            }

        } catch {
            $lblTop.Text =
                "Cài printer lỗi"

            $line =
                $_.InvocationInfo.ScriptLineNumber

            UiLog(
                "CÀI PRINTER LỖI line=" +
                $line +
                ": " +
                $_.Exception.Message
            )

            if ($_.ScriptStackTrace) {
                UiLog(
                    "STACK: " +
                    $_.ScriptStackTrace
                )
            }

            [System.Windows.Forms.MessageBox]::Show(
                "Không cài được Printer " +
                $Letter +
                ".`r`n`r`n" +
                $_.Exception.Message +
                "`r`n`r`nXem Log để biết lỗi ở BƯỚC 1 (TCP port) hay BƯỚC 2 (printer queue).",
                "Lỗi",
                "OK",
                "Error"
            ) | Out-Null
        }
    }

    Add-Type -AssemblyName Microsoft.VisualBasic

    # =========================== UI ==============================
    $form = New-Object Windows.Forms.Form
    $form.Text = "Lumitool Printsever Setup V3.4"
    $form.Size = New-Object Drawing.Size(820,690)
    $form.MinimumSize = New-Object Drawing.Size(790,650)
    $form.StartPosition = "CenterScreen"
    $form.Font = New-Object Drawing.Font("Segoe UI",9)
    $form.BackColor = [Drawing.Color]::FromArgb(245,246,248)

    $title = New-Object Windows.Forms.Label
    $title.Text = "Lumitool Printsever"
    $title.Font = New-Object Drawing.Font("Segoe UI Semibold",20)
    $title.Location = New-Object Drawing.Point(20,14)
    $title.AutoSize = $true
    $form.Controls.Add($title)

    $lblTop = New-Object Windows.Forms.Label
    $lblTop.Text = "Sẵn sàng"
    $lblTop.Location = New-Object Drawing.Point(24,54)
    $lblTop.Size = New-Object Drawing.Size(740,22)
    $form.Controls.Add($lblTop)

    $grpDevice = New-Object Windows.Forms.GroupBox
    $grpDevice.Text = "1. Kết nối ESP"
    $grpDevice.Location = New-Object Drawing.Point(20,82)
    $grpDevice.Size = New-Object Drawing.Size(760,116)
    $form.Controls.Add($grpDevice)

    $btnFind = New-Object Windows.Forms.Button
    $btnFind.Text = "QUÉT TẤT CẢ MẠNG"
    $btnFind.Location = New-Object Drawing.Point(14,27)
    $btnFind.Size = New-Object Drawing.Size(120,34)
    $grpDevice.Controls.Add($btnFind)

    $btnCancelScan = New-Object Windows.Forms.Button
    $btnCancelScan.Text = "HỦY"
    $btnCancelScan.Location = New-Object Drawing.Point(140,27)
    $btnCancelScan.Size = New-Object Drawing.Size(65,34)
    $btnCancelScan.Enabled = $false
    $grpDevice.Controls.Add($btnCancelScan)

    $txtIP = New-Object Windows.Forms.TextBox
    $txtIP.Text = "192.168.10.1"
    $txtIP.Location = New-Object Drawing.Point(215,32)
    $txtIP.Size = New-Object Drawing.Size(125,26)
    $grpDevice.Controls.Add($txtIP)

    $btnConnect = New-Object Windows.Forms.Button
    $btnConnect.Text = "Kết nối IP"
    $btnConnect.Location = New-Object Drawing.Point(350,28)
    $btnConnect.Size = New-Object Drawing.Size(105,32)
    $grpDevice.Controls.Add($btnConnect)

    $btnRefresh = New-Object Windows.Forms.Button
    $btnRefresh.Text = "Làm mới"
    $btnRefresh.Location = New-Object Drawing.Point(465,28)
    $btnRefresh.Size = New-Object Drawing.Size(90,32)
    $grpDevice.Controls.Add($btnRefresh)

    $btnWeb = New-Object Windows.Forms.Button
    $btnWeb.Text = "Mở Web"
    $btnWeb.Location = New-Object Drawing.Point(555,28)
    $btnWeb.Size = New-Object Drawing.Size(88,32)
    $grpDevice.Controls.Add($btnWeb)

    $btnSupport = New-Object Windows.Forms.Button
    $btnSupport.Text = "Hỗ trợ"
    $btnSupport.Location = New-Object Drawing.Point(650,28)
    $btnSupport.Size = New-Object Drawing.Size(88,32)
    $grpDevice.Controls.Add($btnSupport)

    $lblDevice = New-Object Windows.Forms.Label
    $lblDevice.Text = "-"
    $lblDevice.Font = New-Object Drawing.Font("Segoe UI Semibold",10)
    $lblDevice.Location = New-Object Drawing.Point(16,75)
    $lblDevice.Size = New-Object Drawing.Size(710,24)
    $grpDevice.Controls.Add($lblDevice)

    $scanProgress = New-Object Windows.Forms.ProgressBar
    $scanProgress.Location = New-Object Drawing.Point(16,100)
    $scanProgress.Size = New-Object Drawing.Size(724,10)
    $scanProgress.Style = "Blocks"
    $scanProgress.Value = 0
    $grpDevice.Controls.Add($scanProgress)

    $grpDriver = New-Object Windows.Forms.GroupBox
    $grpDriver.Text = "2. Chọn driver máy in (hỗ trợ mọi model đã cài driver trên Windows)"
    $grpDriver.Location = New-Object Drawing.Point(20,208)
    $grpDriver.Size = New-Object Drawing.Size(760,118)
    $form.Controls.Add($grpDriver)

    $lblDriverSearch = New-Object Windows.Forms.Label
    $lblDriverSearch.Text = "Tìm driver:"
    $lblDriverSearch.Location = New-Object Drawing.Point(14,31)
    $lblDriverSearch.Size = New-Object Drawing.Size(72,22)
    $grpDriver.Controls.Add($lblDriverSearch)

    $txtDriverSearch = New-Object Windows.Forms.TextBox
    $txtDriverSearch.Location = New-Object Drawing.Point(88,27)
    $txtDriverSearch.Size = New-Object Drawing.Size(256,26)
    $grpDriver.Controls.Add($txtDriverSearch)

    $btnReloadDrivers = New-Object Windows.Forms.Button
    $btnReloadDrivers.Text = "Nạp lại driver"
    $btnReloadDrivers.Location = New-Object Drawing.Point(354,24)
    $btnReloadDrivers.Size = New-Object Drawing.Size(120,32)
    $grpDriver.Controls.Add($btnReloadDrivers)

    $cmbDriver = New-Object Windows.Forms.ComboBox
    $cmbDriver.DropDownStyle = "DropDownList"
    $cmbDriver.Location = New-Object Drawing.Point(14,61)
    $cmbDriver.Size = New-Object Drawing.Size(550,28)
    $grpDriver.Controls.Add($cmbDriver)

    $lblDriver = New-Object Windows.Forms.Label
    $lblDriver.Text = "-"
    $lblDriver.Location = New-Object Drawing.Point(575,65)
    $lblDriver.Size = New-Object Drawing.Size(165,40)
    $grpDriver.Controls.Add($lblDriver)

    function Make-PrinterBox {
        param([string]$Letter,[int]$Port,[int]$X)

        $g = New-Object Windows.Forms.GroupBox
        $g.Text = "Printer " + $Letter + " · RAW " + $Port
        $g.Location = New-Object Drawing.Point($X,338)
        $g.Size = New-Object Drawing.Size(242,150)
        $form.Controls.Add($g)

        $st = New-Object Windows.Forms.Label
        $st.Text = "Chưa kết nối"
        $st.Font = New-Object Drawing.Font("Segoe UI Semibold",10)
        $st.Location = New-Object Drawing.Point(14,31)
        $st.Size = New-Object Drawing.Size(205,26)
        $g.Controls.Add($st)

        $note = New-Object Windows.Forms.Label
        $note.Text = "Chọn đúng driver ở phía trên."
        $note.Location = New-Object Drawing.Point(14,62)
        $note.Size = New-Object Drawing.Size(205,34)
        $note.ForeColor = [Drawing.Color]::DimGray
        $g.Controls.Add($note)

        $b = New-Object Windows.Forms.Button
        $b.Text = "CÀI PRINTER " + $Letter
        $b.Location = New-Object Drawing.Point(14,105)
        $b.Size = New-Object Drawing.Size(210,32)
        $g.Controls.Add($b)

        return [pscustomobject]@{ Status=$st; Button=$b }
    }

    $A = Make-PrinterBox "A" 9101 20
    $B = Make-PrinterBox "B" 9102 272
    $C = Make-PrinterBox "C" 9103 524

    $lblA = $A.Status
    $lblB = $B.Status
    $lblC = $C.Status

    $grpLog = New-Object Windows.Forms.GroupBox
    $grpLog.Text = "Log"
    $grpLog.Location = New-Object Drawing.Point(20,500)
    $grpLog.Size = New-Object Drawing.Size(760,130)
    $grpLog.Anchor = "Top,Bottom,Left,Right"
    $form.Controls.Add($grpLog)

    $script:txtLog = New-Object Windows.Forms.TextBox
    $script:txtLog.Multiline = $true
    $script:txtLog.ReadOnly = $true
    $script:txtLog.ScrollBars = "Vertical"
    $script:txtLog.Font = New-Object Drawing.Font("Consolas",9)
    $script:txtLog.Location = New-Object Drawing.Point(12,23)
    $script:txtLog.Size = New-Object Drawing.Size(736,95)
    $script:txtLog.Anchor = "Top,Bottom,Left,Right"
    $grpLog.Controls.Add($script:txtLog)

    $scanTimer = New-Object Windows.Forms.Timer
    $scanTimer.Interval = 250
    $scanTimer.Add_Tick({
        if ($script:ScanProgressPath -and (Test-Path $script:ScanProgressPath)) {
            try {
                $p = Get-Content $script:ScanProgressPath -Raw -ErrorAction Stop | ConvertFrom-Json
                if ($p.text) { $lblTop.Text = [string]$p.text }

                if ($null -ne $p.percent) {
                    $value = [Math]::Max(0, [Math]::Min(100, [int]$p.percent))
                    $scanProgress.Style = "Blocks"
                    $scanProgress.Value = $value
                }
            } catch {}
        }

        if ($script:ScanProcess -and $script:ScanProcess.HasExited) {
            $scanTimer.Stop()
            $btnFind.Enabled = $true
            $btnCancelScan.Enabled = $false
            $scanProgress.Style = "Blocks"

            $devices = @()

            if ($script:ScanResultPath -and (Test-Path $script:ScanResultPath)) {
                try {
                    $raw = Get-Content $script:ScanResultPath -Raw -ErrorAction Stop
                    if (-not [string]::IsNullOrWhiteSpace($raw)) {
                        $devices = @($raw | ConvertFrom-Json)
                    }
                } catch {
                    UiLog ("Đọc kết quả quét lỗi: " + $_.Exception.Message)
                }
            }

            $elapsed = 0
            if ($script:ScanStartedAt) {
                $elapsed = [int]((Get-Date) - $script:ScanStartedAt).TotalSeconds
            }

            UiLog ("Quét nền hoàn tất sau " + $elapsed + "s; tìm thấy " + $devices.Count + " thiết bị")

            try { Remove-Item $script:ScanResultPath -Force -ErrorAction SilentlyContinue } catch {}
            try { Remove-Item $script:ScanProgressPath -Force -ErrorAction SilentlyContinue } catch {}

            $script:ScanProcess = $null
            $scanProgress.Value = 100

            Show-ScanResults -Devices $devices
        }
    })

    # Events
    $btnFind.Add_Click({ Start-AsyncScan })
    $btnCancelScan.Add_Click({ Stop-AsyncScan })
    $btnConnect.Add_Click({ Connect-IP })

    $btnRefresh.Add_Click({
        if ($script:CurrentIP) {
            $txtIP.Text = $script:CurrentIP
        }
        Connect-IP
    })

    $btnWeb.Add_Click({
        if ($script:CurrentIP) {
            Start-Process ("http://" + $script:CurrentIP + "/")
        }
    })

    $btnSupport.Add_Click({
        if ($script:CurrentIP) {
            Start-Process ("http://" + $script:CurrentIP + "/support")
        }
    })

    $btnReloadDrivers.Add_Click({
        Load-Drivers
        Filter-Drivers
    })

    $txtDriverSearch.Add_TextChanged({
        Filter-Drivers
    })

    $A.Button.Add_Click({ Install-PrinterFor "A" 9101 })
    $B.Button.Add_Click({ Install-PrinterFor "B" 9102 })
    $C.Button.Add_Click({ Install-PrinterFor "C" 9103 })

    $txtIP.Add_KeyDown({
        if ($_.KeyCode -eq [Windows.Forms.Keys]::Enter) {
            Connect-IP
            $_.SuppressKeyPress = $true
        }
    })

    Load-Drivers

    UiLog "Lumitool Printsever Setup V3.4"
    UiLog "Hỗ trợ mọi máy in có driver Windows đã được cài."
    UiLog "A=9101 · B=9102 · C=9103 · RAW completion = TCP close · SNMP/BIDI OFF"
    UiLog ("Debug log: " + $LogPath)

    $form.Add_FormClosing({
        try { $scanTimer.Stop() } catch {}
        if ($script:ScanProcess -and -not $script:ScanProcess.HasExited) {
            try { $script:ScanProcess.Kill() } catch {}
        }
    })

    [void]$form.ShowDialog()
    Write-DebugLog "=== NORMAL EXIT ==="
}
catch {
    $msg = $_.Exception.Message
    $detail = $_ | Out-String

    Write-DebugLog ("FATAL: " + $detail)

    try {
        Add-Type -AssemblyName System.Windows.Forms -ErrorAction SilentlyContinue
        [System.Windows.Forms.MessageBox]::Show(
            "Lumitool Printsever Setup gặp lỗi:`r`n`r`n" + $msg + "`r`n`r`nLog:`r`n" + $LogPath,
            "Lumitool Printsever Setup - Lỗi",
            "OK",
            "Error"
        ) | Out-Null
    } catch {}

    Write-Host ""
    Write-Host "ESP PRINT SERVER SETUP ERROR" -ForegroundColor Red
    Write-Host $detail
    Write-Host ""
    Write-Host ("Log: " + $LogPath)
    Read-Host "Nhan Enter de dong"
    exit 1
}
