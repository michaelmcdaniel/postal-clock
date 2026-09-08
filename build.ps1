$ErrorActionPreference = 'Stop'

$arduinoCli = Get-Command arduino-cli -ErrorAction SilentlyContinue
if (-not $arduinoCli) {
    $installedCli = 'C:\Program Files\Arduino CLI\arduino-cli.exe'
    if (-not (Test-Path -LiteralPath $installedCli)) {
        throw 'arduino-cli was not found. See README.md for installation instructions.'
    }
    $arduinoCliPath = $installedCli
} else {
    $arduinoCliPath = $arduinoCli.Source
}

$projectDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path
$buildDirectory = Join-Path $projectDirectory 'build'
& $arduinoCliPath compile `
    --fqbn 'rp2040:rp2040:rpipico2w:flash=4194304_262144' `
    --warnings all `
    --build-path $buildDirectory `
    $projectDirectory
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}
