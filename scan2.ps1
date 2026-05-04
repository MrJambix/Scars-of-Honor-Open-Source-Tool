param([string]$Dump,[string]$Pat)
$lines = Get-Content $Dump
$inCore = $false
$owner = ""
$out = New-Object System.Collections.ArrayList
for ($i=0; $i -lt $lines.Length; $i++) {
    $L = $lines[$i]
    if ($L -match '^// (\S+)\s+\[Code\.Core\.dll\]') { $owner = $Matches[1]; $inCore = $true; continue }
    if ($L -match '^// \S+\s+\[(?!Code\.Core\.dll)') { $inCore = $false; continue }
    if ($inCore -and ($L -match $Pat)) {
        [void]$out.Add("$owner :: $($L.Trim())")
    }
}
"TOTAL=$($out.Count)"
$out
