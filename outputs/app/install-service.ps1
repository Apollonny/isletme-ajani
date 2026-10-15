param([switch]$Remove)
$ErrorActionPreference='Stop'
$identity=[Security.Principal.WindowsIdentity]::GetCurrent();$principal=[Security.Principal.WindowsPrincipal]::new($identity)
if(!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){throw 'Run this script in an elevated PowerShell. The installer uses per-user desktop mode by default.'}
$name='YerelAjan'
if($Remove){if(Get-Service $name -ErrorAction SilentlyContinue){Stop-Service $name -ErrorAction SilentlyContinue;sc.exe delete $name | Out-Null};Get-NetFirewallRule -DisplayName 'YerelAjan core offline' -ErrorAction SilentlyContinue|Remove-NetFirewallRule;return}
$exe=Join-Path $PSScriptRoot 'YerelAjan.exe';$config=Join-Path $PSScriptRoot 'config.yaml'
# Service data and protected secrets must be configured under the same Windows identity.
$credential=Get-Credential -Message 'Windows account for the local agent service (the account that configured DPAPI secrets)'
New-Service -Name $name -DisplayName 'YerelAjan local customer service' -BinaryPathName ('"'+$exe+'" --service --config "'+$config+'"') -Credential $credential -StartupType Automatic -Description 'Local AI core; WhatsApp uses a separate gateway.' | Out-Null
New-NetFirewallRule -DisplayName 'YerelAjan core offline' -Direction Outbound -Program $exe -Action Block -Profile Any | Out-Null
Start-Service $name
