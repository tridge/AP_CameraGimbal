# Start or stop a private headless Chrome with remote debugging (Windows test machine).
#   powershell -File win_chrome.ps1 start|stop|list [-port 9334] [-name stage4prof]
# The profile lives in $env:TEMP\<name>; stop kills only processes using that profile.
param([string]$action = "start", [int]$port = 9334, [string]$name = "stage4prof")
$prof = Join-Path $env:TEMP $name
$mine = { Get-CimInstance Win32_Process -Filter "Name='chrome.exe'" | Where-Object { $_.CommandLine -match [regex]::Escape($name) } }
if ($action -eq "start") {
  $a = @("--headless=new", "--remote-debugging-port=$port", "--remote-debugging-address=127.0.0.1",
    "--user-data-dir=$prof", "--no-first-run", "--no-default-browser-check", "--force_high_performance_gpu", "--enable-unsafe-webgpu",
    "--disable-features=LocalNetworkAccessChecks,PrivateNetworkAccessRespectPreflightResults,PrivateNetworkAccessSendPreflights",
    "about:blank")
  Start-Process -FilePath "C:\Program Files\Google\Chrome\Application\chrome.exe" -ArgumentList $a
  "started on port $port, profile $prof"
} elseif ($action -eq "stop") {
  $p = & $mine
  "stopping $($p.Count) chrome processes for $name"
  $p | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
} else {
  Get-CimInstance Win32_Process -Filter "Name='chrome.exe'" | Where-Object { $_.CommandLine -match "remote-debugging-port" } |
    ForEach-Object { ($_.CommandLine -split " " | Where-Object { $_ -match "remote-debugging-port|user-data-dir" }) -join " " } | Sort-Object -Unique
}
