# Headless Chrome for the WebSplat harness on the win11 machine, with its own profile and a
# localhost-only remote debugging port (reached through an ssh -L tunnel).
param([int]$port = 9345, [string]$extra = "", [string]$name = "websplat-harness-profile")
$profile = "C:\cygwin64\tmp\$name"
$a = @("--headless=new", "--remote-debugging-port=$port", "--remote-debugging-address=127.0.0.1",
  "--user-data-dir=$profile", "--no-first-run", "--no-default-browser-check",
  "--enable-unsafe-webgpu", "--force_high_performance_gpu",
  "--disable-features=LocalNetworkAccessChecks,PrivateNetworkAccessRespectPreflightResults,PrivateNetworkAccessSendPreflights")
if ($extra) { $a += $extra.Split(" ") }
$a += "about:blank"
Start-Process -FilePath "C:\Program Files\Google\Chrome\Application\chrome.exe" -ArgumentList $a
