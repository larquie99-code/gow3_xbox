#requires -Version 7.2
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$assets=Join-Path $root '.local/build/layout/Assets'
New-Item -ItemType Directory -Force $assets | Out-Null
# Pre-sized icons and splash artwork are included with the package sources.
foreach($name in @('StoreLogo.png','Square150x150Logo.png','Square44x44Logo.png','SplashScreen.png')) {
    Copy-Item -LiteralPath (Join-Path $root "assets/$name") -Destination (Join-Path $assets $name) -Force
}
