# Flash portal_guard and reboot the chip.
#
# The board's EN pin is not wired to USB RTS, but this chip routes its console and
# reset through the USB-Serial/JTAG peripheral, so esptool's usb-reset / hard-reset
# sequences work and no physical power-cycle is needed.
#
# usage:  .\flash.ps1 COM6
#
# ASCII only: executed by Windows PowerShell 5.1 with an ANSI code page.
$ErrorActionPreference = "Continue"

$proj = $PSScriptRoot
$log  = Join-Path $proj "flash.log"
$port = $args[0]
if (-not $port) { $port = "COM6" }

if (-not $env:IDF_PATH) {
    "ERROR: IDF_PATH is not set. Run your ESP-IDF export script first." | Write-Host
    exit 1
}
$py = "$env:IDF_PYTHON_ENV_PATH\Scripts\python.exe"
if (-not (Test-Path $py)) {
    "ERROR: python not found at $py (check IDF_PYTHON_ENV_PATH)." | Write-Host
    exit 1
}

if (Test-Path $log) { Remove-Item $log -Force }

"== write ==" | Out-File $log -Encoding ascii
# NOTE: flash-mode MUST stay dio. Writing qio here rewrites the image headers and
# the ROM then boots in QIO mode, where this board hangs right after the first
# segment load (verified on hardware 2026-11-01). The sdkconfig still says QIO;
# the header value is what the ROM uses, and DIO is the one that boots.
& $py -m esptool --chip esp32s3 -p $port -b 460800 --before usb-reset --after no-reset `
    write-flash --flash-mode dio --flash-freq 80m --flash-size 16MB `
    0x0     "$proj\build\bootloader\bootloader.bin" `
    0x8000  "$proj\build\partition_table\partition-table.bin" `
    0x10000 "$proj\build\portal_guard.bin" *>> $log
$w = $LASTEXITCODE

"$w|$port" | Write-Host
exit $w
