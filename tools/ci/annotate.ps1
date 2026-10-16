# Runs a cmd.exe command line; on failure, publishes its error lines as a GitHub annotation, so the cause is
# readable from the run's annotations even where the raw log is not.
# -VcVars: run inside the x64 MSVC developer environment.
param(
    [Parameter(Mandatory)][string]$Title,
    [Parameter(Mandatory)][string]$Command,
    [switch]$VcVars
)
$line = $Command
if ($VcVars) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $line = "call `"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && $Command"
}
$out = & cmd /c "$line 2>&1"
$code = $LASTEXITCODE
$out | ForEach-Object { Write-Host $_ }
if ($code -ne 0) {
    $errs = @($out | Where-Object { $_ -match ': (fatal )?error |error [A-Z]+[0-9]+|warning C[0-9]+|FAIL|failure' } | Select-Object -First 40)
    if ($errs.Count -eq 0) { $errs = @($out | Select-Object -Last 40) }
    $msg = (($errs -join "`n").Replace('%', '%25').Replace("`r", '').Replace("`n", '%0A'))
    Write-Host "::error title=$Title::$msg"
    exit $code
}
