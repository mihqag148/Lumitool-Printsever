param(
    [Parameter(Mandatory=$true)][string]$IP,
    [Parameter(Mandatory=$true)][string]$Password,
    [Parameter(Mandatory=$true)][string]$BinPath,
    [Parameter(Mandatory=$true)][string]$ResultPath,
    [Parameter(Mandatory=$true)][string]$ProgressPath
)

$ErrorActionPreference = "Stop"

function Write-State {
    param([int]$Percent,[string]$Text)
    [pscustomobject]@{ percent=$Percent; text=$Text } | ConvertTo-Json -Compress | Set-Content -Path $ProgressPath -Encoding UTF8
}

function Finish-Result {
    param([bool]$Ok,[string]$Message)
    [pscustomobject]@{ ok=$Ok; message=$Message } | ConvertTo-Json -Compress | Set-Content -Path $ResultPath -Encoding UTF8
}

function Get-AuthHeader {
    $raw = "admin:" + $Password
    $bytes = [Text.Encoding]::ASCII.GetBytes($raw)
    return @{ Authorization = "Basic " + [Convert]::ToBase64String($bytes) }
}

try {
    if (-not (Test-Path $BinPath)) { throw "Không tìm thấy firmware .bin." }
    $info = Get-Item $BinPath
    if ($info.Length -lt 65536) { throw "File .bin quá nhỏ, không giống firmware hợp lệ." }
    $headers = Get-AuthHeader
    Write-State 5 "Đang yêu cầu ESP vào chế độ OTA..."
    Invoke-WebRequest -Uri ("http://" + $IP + "/support/prepare") -Method Post -Headers $headers -UseBasicParsing -TimeoutSec 5 -ErrorAction Stop | Out-Null
    $deadline = (Get-Date).AddSeconds(120)
    $ready = $false
    while ((Get-Date) -lt $deadline) {
        try {
            $status = Invoke-RestMethod -Uri ("http://" + $IP + "/support/status") -Headers $headers -TimeoutSec 4 -ErrorAction Stop
            if ([bool]$status.ready -and -not [bool]$status.uploading) { $ready = $true; break }
            Write-State 15 ("Đang chờ job in hoàn tất... " + [string]$status.message)
        } catch {
            Write-State 10 "Đang chờ ESP phản hồi OTA..."
        }
        Start-Sleep -Milliseconds 700
    }
    if (-not $ready) { throw "ESP chưa sẵn sàng OTA sau 120 giây." }
    Write-State 30 ("Đang upload " + $info.Name + " (" + [Math]::Round($info.Length / 1MB,2) + " MB)...")
    $curlArgs = @("-sS","--max-time","300","-u",("admin:" + $Password),"-F",("firmware=@" + $BinPath + ";type=application/octet-stream"),("http://" + $IP + "/update"))
    $output = & curl.exe @curlArgs 2>&1
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) { throw ("Upload OTA thất bại, curl exit=" + $exitCode + ". " + ($output -join " ")) }
    Write-State 90 "Firmware đã gửi xong. Đang chờ ESP khởi động lại..."
    Start-Sleep -Seconds 3
    $rebootDeadline = (Get-Date).AddSeconds(45)
    $version = ""
    while ((Get-Date) -lt $rebootDeadline) {
        try {
            $dev = Invoke-RestMethod -Uri ("http://" + $IP + "/device-info") -TimeoutSec 2 -ErrorAction Stop
            if ([string]$dev.magic -eq "LUMITOOL_PRINTSEVER") { $version = [string]$dev.version; break }
        } catch {}
        Start-Sleep -Milliseconds 900
    }
    Write-State 100 "OTA hoàn tất."
    if ($version) { Finish-Result $true ("OTA thành công. Thiết bị đã online lại: " + $version) }
    else { Finish-Result $true "OTA đã gửi thành công. ESP đang khởi động lại; nếu chưa thấy online hãy chờ thêm vài giây." }
}
catch {
    try { Write-State 0 ("OTA lỗi: " + $_.Exception.Message) } catch {}
    try { Finish-Result $false ("OTA lỗi: " + $_.Exception.Message) } catch {}
    exit 1
}