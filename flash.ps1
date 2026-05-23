#Requires -Version 5.1
<#
.SYNOPSIS
    Download and flash the latest LumiGate firmware.
    Auto-detects ESP32 vs ESP32-S3.
#>

$ErrorActionPreference = "Stop"
$REPO = "tombueng/LumiGate"

function Write-Step($msg) { Write-Host "`n==> $msg" -ForegroundColor Cyan }
function Write-Ok($msg)   { Write-Host "    OK: $msg" -ForegroundColor Green }
function Write-Err($msg)  { Write-Host "    ERROR: $msg" -ForegroundColor Red; exit 1 }

# ---------------------------------------------------------------------------
# 1. Python
# ---------------------------------------------------------------------------
Write-Step "Checking Python..."
$python = $null
foreach ($cmd in @("python", "python3")) {
    try {
        $ver = & $cmd --version 2>&1
        if ($ver -match "Python 3") { $python = $cmd; break }
    } catch {}
}
if (-not $python) {
    Write-Host "    Python 3 not found. Installing via winget..." -ForegroundColor Yellow
    winget install --id Python.Python.3.12 --accept-source-agreements --accept-package-agreements
    $env:PATH = [System.Environment]::GetEnvironmentVariable("PATH", "Machine") + ";" +
                [System.Environment]::GetEnvironmentVariable("PATH", "User")
    $python = "python"
}
Write-Ok (& $python --version)

# ---------------------------------------------------------------------------
# 2. esptool
# ---------------------------------------------------------------------------
Write-Step "Checking esptool..."
$esptoolOk = $false
try { if (& $python -m esptool version 2>&1) { $esptoolOk = $true } } catch {}
if (-not $esptoolOk) {
    Write-Host "    Installing esptool..." -ForegroundColor Yellow
    & $python -m pip install --quiet esptool
}
Write-Ok "esptool ready"

# ---------------------------------------------------------------------------
# 3. Select COM port
# ---------------------------------------------------------------------------
Write-Step "Available COM ports:"
$ports = [System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object
if ($ports.Count -eq 0) { Write-Err "No COM ports found. Is the board connected?" }
for ($i = 0; $i -lt $ports.Count; $i++) { Write-Host "    [$i] $($ports[$i])" }
$idx = Read-Host "`n    Enter number for your board's COM port"
if (-not ($idx -match '^\d+$') -or [int]$idx -ge $ports.Count) { Write-Err "Invalid selection." }
$port = $ports[[int]$idx]
Write-Ok "Selected $port"

# ---------------------------------------------------------------------------
# 4. Boot mode
# ---------------------------------------------------------------------------
Write-Host ""
Write-Host "###############################################################" -ForegroundColor Yellow
Write-Host "#   PUT THE BOARD INTO BOOT MODE                              #" -ForegroundColor Yellow
Write-Host "#                                                              #" -ForegroundColor Yellow
Write-Host "#   1. Hold the BOOT button                                   #" -ForegroundColor Yellow
Write-Host "#   2. While holding BOOT, press and release EN/RST           #" -ForegroundColor Yellow
Write-Host "#   3. Release BOOT                                           #" -ForegroundColor Yellow
Write-Host "#                                                              #" -ForegroundColor Yellow
Write-Host "#   ESP32-S3: use the USB-UART port, not the native USB port  #" -ForegroundColor Yellow
Write-Host "###############################################################" -ForegroundColor Yellow
Read-Host "`n    Press ENTER when the board is in boot mode"

# ---------------------------------------------------------------------------
# 5. Auto-detect chip
# ---------------------------------------------------------------------------
Write-Step "Detecting chip on $port ..."
$chipOut = & $python -m esptool --port $port --before no_reset chip_id 2>&1
$chipStr = $chipOut | Out-String

if ($chipStr -match "ESP32-S3") {
    $chip      = "esp32s3"
    $baud      = 921600
    $boardName = "ESP32-S3"
    $files = [ordered]@{
        "bootloader-esp32s3.bin" = "0x0000"
        "partitions-esp32s3.bin" = "0x8000"
        "boot_app0.bin"          = "0xe000"
        "firmware-esp32s3.bin"   = "0x10000"
    }
} elseif ($chipStr -match "ESP32") {
    $chip      = "esp32"
    $baud      = 460800
    $boardName = "ESP32"
    $files = [ordered]@{
        "bootloader.bin"  = "0x1000"
        "partitions.bin"  = "0x8000"
        "boot_app0.bin"   = "0xe000"
        "firmware.bin"    = "0x10000"
    }
} else {
    Write-Host $chipStr
    Write-Err "Could not identify chip. Check the COM port and boot mode."
}
Write-Ok "Detected: $boardName"

# ---------------------------------------------------------------------------
# 6. Download firmware
# ---------------------------------------------------------------------------
Write-Step "Fetching latest release from github.com/$REPO ..."
$headers = @{ "User-Agent" = "LumiGate-flash-script" }
try {
    $release = Invoke-RestMethod -Uri "https://api.github.com/repos/$REPO/releases/tags/latest" -Headers $headers
} catch {
    Write-Err "Could not reach GitHub API: $_"
}

$tmpDir = Join-Path $env:TEMP "lumigate_flash"
New-Item -ItemType Directory -Force -Path $tmpDir | Out-Null

foreach ($file in $files.Keys) {
    $asset = $release.assets | Where-Object { $_.name -eq $file }
    if (-not $asset) { Write-Err "Asset '$file' not found in release." }
    $dest = Join-Path $tmpDir $file
    Write-Host "    Downloading $file ..." -NoNewline
    Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $dest -UseBasicParsing
    Write-Host " done" -ForegroundColor Green
}

# ---------------------------------------------------------------------------
# 7. Flash
# ---------------------------------------------------------------------------
Write-Step "Flashing $boardName on $port at $baud baud..."

$flashArgs = @(
    "-m", "esptool",
    "--chip",   $chip,
    "--port",   $port,
    "--baud",   $baud,
    "--before", "no_reset",
    "--after",  "hard_reset",
    "write_flash", "-z", "--flash_mode", "dio", "--flash_freq", "80m"
)
foreach ($file in $files.Keys) {
    $flashArgs += $files[$file]
    $flashArgs += (Join-Path $tmpDir $file)
}

& $python @flashArgs
if ($LASTEXITCODE -ne 0) { Write-Err "esptool exited with code $LASTEXITCODE" }

Write-Host ""
Write-Host "###############################################################" -ForegroundColor Green
Write-Host "#   Flash complete! ($boardName)                               #" -ForegroundColor Green
Write-Host "#   Press EN/RST to boot.                                     #" -ForegroundColor Green
Write-Host "#   First boot opens WiFi AP: DMX-Gateway (no password)       #" -ForegroundColor Green
Write-Host "###############################################################" -ForegroundColor Green
