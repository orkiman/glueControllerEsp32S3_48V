# Auto-activate the GUI venv and run it from the project root.
$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$venvPython  = Join-Path $projectRoot 'gui\venv\Scripts\python.exe'
$entryPoint  = Join-Path $projectRoot 'gui\run.py'

if (-not (Test-Path $venvPython)) {
    Write-Error "venv Python not found at $venvPython. Run: cd gui; python -m venv venv"
    exit 1
}

& $venvPython $entryPoint
exit $LASTEXITCODE
