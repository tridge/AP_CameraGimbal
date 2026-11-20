# stop only the G1 test Chrome (processes using the g1prof profile)
$p = Get-CimInstance Win32_Process -Filter "Name='chrome.exe'" | Where-Object { $_.CommandLine -match 'g1prof' }
"stopping $($p.Count) test chrome processes"
$p | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
