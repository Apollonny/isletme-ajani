param([string]$Python='python',[string]$Runtime,[switch]$Offline,[switch]$PrepareOnly)
$ErrorActionPreference='Stop'
$arguments=@((Join-Path $PSScriptRoot 'build.py'))
if($Runtime){$arguments+=@('--runtime',$Runtime)}
if($Offline){$arguments+='--offline'}
if($PrepareOnly){$arguments+='--prepare-only'}
& $Python @arguments
if($LASTEXITCODE -ne 0){throw "Source build failed: $LASTEXITCODE"}
