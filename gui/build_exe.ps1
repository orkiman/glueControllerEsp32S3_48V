# Build the standalone Windows GUI: dist\GlueController.exe (single file).
# Run from anywhere:  powershell -ExecutionPolicy Bypass -File gui\build_exe.ps1
# Needs the GUI venv (gui\.venv) with requirements.txt + pyinstaller installed.
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

& .\.venv\Scripts\python.exe -m PyInstaller --noconfirm --clean --noconsole --onefile `
    --name GlueController `
    --add-data "ui/styles.qss;ui" `
    run.py
if ($LASTEXITCODE -ne 0) { throw "PyInstaller failed" }

Get-Item dist\GlueController.exe | Select-Object FullName, @{n = 'MB'; e = { [math]::Round($_.Length / 1MB, 1) } }
