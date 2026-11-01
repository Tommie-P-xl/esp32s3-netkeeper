# Build portal_guard from the command line.
#
# Requires an ESP-IDF environment: either run "export.ps1" from your IDF install
# first, or set IDF_PATH + IDF_PYTHON_ENV_PATH yourself.
#
# ASCII only: this file runs under Windows PowerShell 5.1 with an ANSI code page,
# so any non-ASCII byte in it breaks the parser.
$ErrorActionPreference = "Continue"

$proj = $PSScriptRoot

if (-not $env:IDF_PATH) {
    "ERROR: IDF_PATH is not set. Run your ESP-IDF export script first." | Write-Host
    exit 1
}
$py = "$env:IDF_PYTHON_ENV_PATH\Scripts\python.exe"
if (-not (Test-Path $py)) {
    "ERROR: python not found at $py (check IDF_PYTHON_ENV_PATH)." | Write-Host
    exit 1
}

$log = Join-Path $proj "build.log"
if (Test-Path $log) { Remove-Item $log -Force }

& $py "$env:IDF_PATH\tools\idf.py" -C $proj build *> $log
$rc = $LASTEXITCODE
"build=$rc" | Write-Host
exit $rc
