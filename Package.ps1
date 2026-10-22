param(
    [string]$NativeBuild = 'obj/package/cs2',
    [switch]$SourceOnly
)

$ErrorActionPreference = 'Stop'

if ($SourceOnly) {
    python "$PSScriptRoot/package.py" --source-only
}
else {
    python "$PSScriptRoot/package.py" --build $NativeBuild
}

if ($LASTEXITCODE -ne 0) {
    throw 'Native packaging failed.'
}
