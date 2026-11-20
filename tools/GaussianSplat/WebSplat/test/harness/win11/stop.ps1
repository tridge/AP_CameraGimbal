# stop only the harness Chrome: processes whose --user-data-dir argument is exactly this profile
# (quoted or unquoted; not a profile whose name merely starts with it)
param([string]$name = "websplat-harness-profile", [switch]$test)
$profile = "C:\cygwin64\tmp\$name"
$e = [regex]::Escape($profile)
$pat = '(^|\s)(--user-data-dir="' + $e + '"|"--user-data-dir=' + $e + '"|--user-data-dir=' + $e + ')(?=\s|$)'
if ($test) {
  # predicate check only: command lines on stdin, one per line; prints 1/0 per line
  $input | ForEach-Object { if ($_ -match $pat) { 1 } else { 0 } }
  exit
}
$p = Get-CimInstance Win32_Process -Filter "Name='chrome.exe'" | Where-Object { $_.CommandLine -match $pat }
"stopping $(@($p).Count) harness chrome processes"
$p | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
