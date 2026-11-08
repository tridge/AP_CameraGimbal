# stop only the harness Chrome (processes using its profile)
$p = Get-CimInstance Win32_Process -Filter "Name='chrome.exe'" | Where-Object { $_.CommandLine -match 'websplat-harness-profile' }
"stopping $(@($p).Count) harness chrome processes"
$p | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
