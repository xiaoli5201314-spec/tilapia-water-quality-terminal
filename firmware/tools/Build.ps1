param([string]$KeilPath = $env:KEIL_UV4)
# Synchronize the project, rebuild with MDK, then check reserved Flash boundaries.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($KeilPath) -or !(Test-Path -LiteralPath $KeilPath -PathType Leaf)) {
    throw 'Pass -KeilPath with the full path to UV4.exe, or set KEIL_UV4.'
}
& (Join-Path $PSScriptRoot 'Sync_Keil_Project.ps1')
& (Join-Path $PSScriptRoot 'Verify_Project.ps1')
$project = Join-Path $root 'MDK-ARM\XIAOLI-CODE.uvprojx'
$log = Join-Path $root 'build_log.txt'
$buildArgs = '-r "{0}" -t "XIAOLI-CODE" -o "{1}"' -f $project, $log
$process = Start-Process -FilePath $KeilPath -ArgumentList $buildArgs -WindowStyle Hidden -PassThru
$process.WaitForExit()
$content = [IO.File]::ReadAllText($log)
Write-Output $content
if ($process.ExitCode -ne 0 -or $content -notmatch '0 Error\(s\), 0 Warning\(s\)') {
    throw "Keil build not clean. Exit=$($process.ExitCode). See $log"
}
& (Join-Path $PSScriptRoot 'Verify_Project.ps1') -LinkedImage
