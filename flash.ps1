# Flash esp32s3-netkeeper and start it running.
#
# usage:  .\flash.ps1 COM6
#
# Reset notes (learned the hard way on this board):
#   * EN is NOT wired to USB RTS, so "--after hard-reset" is a no-op here.
#     A flashed chip stays in the ROM download mode and the screen never lights
#     up, which looks exactly like "firmware is broken".
#   * "--after watchdog-reset" IS a CPU level reset and does boot from flash,
#     but esptool loses the serial port mid-command and exits non-zero, which
#     would make a successful flash look failed.
#   * So: write with no-reset (exit code reflects only the write), then fire a
#     best-effort watchdog reset in a second command and ignore its status.
#   * If the console still stays silent afterwards, long-press PWR once: that
#     is a full PMIC power cycle and always works.
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
    0x10000 "$proj\build\esp32s3_netkeeper.bin" *>> $log
$w = $LASTEXITCODE

if ($w -eq 0) {
    "== reset ==" | Out-File $log -Append -Encoding ascii
    # Best effort: this exits non-zero because the port disappears while the
    # chip resets. The flash already succeeded, so do not propagate the code.
    & $py -m esptool --chip esp32s3 -p $port --before usb-reset --after watchdog-reset `
        chip_id *>> $log
}

"$w|$port" | Write-Host
exit $w
