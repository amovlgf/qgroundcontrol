param([Parameter(Mandatory = $true)][string]$Installer)
$ErrorActionPreference = 'Stop'
$installDir = Join-Path $env:RUNNER_TEMP 'qgc-installed-ai'
# This ephemeral CI runner validates application packaging, not USB drivers.
# Mark drivers as present to avoid the installer's interactive driver MSI.
New-Item 'HKCU:\SOFTWARE\MichaelOborne\driver' -Force | Out-Null
New-ItemProperty 'HKCU:\SOFTWARE\MichaelOborne\driver' -Name installed -Value 1 -PropertyType DWord -Force | Out-Null
New-Item 'HKCU:\SOFTWARE\QGroundControlUAVDrivers' -Force | Out-Null
New-ItemProperty 'HKCU:\SOFTWARE\QGroundControlUAVDrivers' -Name version -Value 3 -PropertyType DWord -Force | Out-Null
$install = Start-Process -FilePath $Installer -ArgumentList @('/S', "/D=$installDir") -PassThru
if (-not $install.WaitForExit(180000)) { $install.Kill(); throw 'Installer timed out' }
if ($install.ExitCode -ne 0) { throw "Installer exited with $($install.ExitCode)" }
$binary = Join-Path $installDir 'bin/QGroundControl.exe'
if (-not (Test-Path $binary)) { throw 'Installed executable is missing' }
# Remove developer Qt paths: the installed package must supply its own runtime.
$env:QT_PLUGIN_PATH = ''
$env:QML2_IMPORT_PATH = ''
$env:QML_IMPORT_PATH = ''
$env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
$boot = Start-Process -FilePath $binary -ArgumentList '--simple-boot-test' -WorkingDirectory $installDir -PassThru
if (-not $boot.WaitForExit(90000)) { $boot.Kill(); throw 'Installed application boot timed out' }
if ($boot.ExitCode -ne 0) { throw "Installed application exited with $($boot.ExitCode)" }
Write-Output 'Installed Windows package boot passed (USB driver installation not tested).'
