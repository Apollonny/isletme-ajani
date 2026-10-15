$ErrorActionPreference='Stop'
$config=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'config.yaml') -Raw|ConvertFrom-Json
foreach($channel in @(@('sip','YerelAjanSIP.exe'),@('sms','YerelAjanSMS.exe'),@('whatsapp','YerelAjanGateway.exe'))){if($config.channels.($channel[0]).enabled){Start-Process -FilePath (Join-Path $PSScriptRoot $channel[1]) -WorkingDirectory $PSScriptRoot -WindowStyle Hidden}}
