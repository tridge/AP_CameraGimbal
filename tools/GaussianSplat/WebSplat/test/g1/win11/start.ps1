# Headless Chrome for G1 runs on the win11 machine: own profile, localhost-only DevTools port
# (reached through ssh -L), the harness server reached through ssh -R as its localhost.
# --force_high_performance_gpu is needed: without it this Chrome reports no WebGPU adapter.
param([int]$port = 9353, [string]$extra = "")
$a = @("--headless=new", "--remote-debugging-port=$port", "--remote-debugging-address=127.0.0.1",
  "--user-data-dir=C:\cygwin64\tmp\g1prof", "--no-first-run", "--no-default-browser-check",
  "--enable-unsafe-webgpu", "--force_high_performance_gpu",
  "--disable-features=LocalNetworkAccessChecks,PrivateNetworkAccessRespectPreflightResults,PrivateNetworkAccessSendPreflights")
if ($extra) { $a += $extra.Split(" ") }
$a += "about:blank"
Start-Process -FilePath "C:\Program Files\Google\Chrome\Application\chrome.exe" -ArgumentList $a
