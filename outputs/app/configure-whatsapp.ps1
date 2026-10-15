param([Parameter(Mandatory=$true)][string]$CertificateThumbprint,[int]$Port=8443)
$ErrorActionPreference='Stop'
$identity=[Security.Principal.WindowsIdentity]::GetCurrent()
$principal=[Security.Principal.WindowsPrincipal]::new($identity)
if(!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){throw 'HTTPS binding requires an elevated PowerShell.'}
$thumb=$CertificateThumbprint.Replace(' ','')
if(!(Test-Path -LiteralPath "Cert:\LocalMachine\My\$thumb")){throw 'Install your publicly trusted domain certificate in LocalMachine\My first.'}
$certificate=Get-Item -LiteralPath "Cert:\LocalMachine\My\$thumb"
if(!$certificate.HasPrivateKey){throw 'Certificate has no private key.'}
$appId='{48c4a1ac-4dbe-47b9-aa57-1966d9f225e7}'
netsh.exe http add sslcert "ipport=0.0.0.0:$Port" "certhash=$thumb" "appid=$appId" certstorename=MY
if($LASTEXITCODE){throw 'SSL binding failed; inspect existing binding before changing it.'}
netsh.exe http add urlacl "url=https://+:$Port/wa/webhook/" "user=$($identity.Name)"
if($LASTEXITCODE){throw 'URL ACL failed.'}
New-NetFirewallRule -DisplayName 'YerelAjan WhatsApp HTTPS' -Direction Inbound -Protocol TCP -LocalPort $Port -Action Allow | Out-Null
Write-Output "Configure DNS and router forwarding to this HTTPS port, then set Meta callback URL to https://YOUR-DOMAIN:$Port/wa/webhook/."
