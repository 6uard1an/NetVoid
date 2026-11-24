# Sends this PC's current Wi-Fi name and saved password to the ESP32 over USB. The password is never displayed or saved to a file.
param([string]$Port = 'COM8')
$ErrorActionPreference = 'Stop'
$line = netsh wlan show interfaces | Select-String '^\s+Profile\s+:\s(.*)$' | Select-Object -First 1
if (-not $line) { throw 'This PC is not connected to Wi-Fi.' }
$name = $line.Matches[0].Groups[1].Value.Trim()
$key = netsh wlan show profile name="$name" key=clear | Select-String '^\s+Key Content\s+:\s(.*)$' | Select-Object -First 1
if (-not $key) { throw "No saved password found for $name." }
$sp = New-Object System.IO.Ports.SerialPort $Port, 115200
$sp.Encoding = [Text.Encoding]::UTF8
$sp.DtrEnable = $false; $sp.RtsEnable = $false
try {
  $sp.Open()
  Start-Sleep 3
  $sp.Write("setwifi=$name;$($key.Matches[0].Groups[1].Value)`n")
  Write-Host "Sent the login for $name. Waiting for the ESP32 to connect..."
  Start-Sleep 15
  [void]$sp.ReadExisting()
  $sp.Write("status`n")
  Start-Sleep 1
  $sp.ReadExisting()
} finally { $sp.Close() }
