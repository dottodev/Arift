param(
    [switch]$Report
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

function Get-LineCount([string]$dir, [string[]]$exts) {
    $total = 0
    if (-not (Test-Path $dir)) { return 0 }
    Get-ChildItem -Path $dir -Recurse -File | Where-Object {
        $exts -contains $_.Extension.ToLower()
    } | ForEach-Object {
        $total += (Get-Content -LiteralPath $_.FullName | Measure-Object -Line).Lines
    }
    return $total
}

$targets = @(
    @{ Name = "ESP";        Dir = "modules\esp";        Min = 1000 },
    @{ Name = "Map Hack";   Dir = "modules\maphack";    Min = 1000 },
    @{ Name = "Rank Booster"; Dir = "modules\rankbooster"; Min = 9000 },
    @{ Name = "Enemy Lag";  Dir = "modules\enemylag";   Min = 3000 },
    @{ Name = "Void Ban";   Dir = "modules\voidban";    Min = 7000 },
    @{ Name = "Auto Retri"; Dir = "modules\autoretri";  Min = 1000 },
    @{ Name = "Auto Aim";   Dir = "modules\autoaim";    Min = 1000 },
    @{ Name = "Tank Defense"; Dir = "modules\tankdefense"; Min = 1000 },
    @{ Name = "Physical Damage"; Dir = "modules\physicaldamage"; Min = 1000 },
    @{ Name = "Anti Detect"; Dir = "modules\antidetect"; Min = 1000 }
)

$exts = @(".cpp", ".h", ".hpp", ".c", ".cc")

$allPass = $true
foreach ($t in $targets) {
    $dir = Join-Path $root $t.Dir
    $count = Get-LineCount $dir $exts
    $pass = $count -ge $t.Min
    if (-not $pass) { $allPass = $false }
    $status = if ($pass) { "PASS" } else { "FAIL" }
    Write-Host ("[{0}] {1,-14} {2,6} lines (min {3})" -f $status, $t.Name, $count, $t.Min) `
        -ForegroundColor $(if ($pass) { "Green" } else { "Red" })
}

$nativeDir = Join-Path $root "vae\native"
$nativeCount = Get-LineCount $nativeDir $exts
Write-Host ("[INFO] Native infra: {0} lines" -f $nativeCount) -ForegroundColor Yellow

$ktDir = Join-Path $root "vae\android"
$ktCount = Get-LineCount $ktDir @(".kt", ".java", ".xml", ".kts")
Write-Host ("[INFO] Host app (Kotlin/XML): {0} lines" -f $ktCount) -ForegroundColor Yellow

if (-not $allPass) {
    Write-Host "[RESULT] SOME MODULES BELOW TARGET - NOT APPROVED FOR RELEASE" -ForegroundColor Red
    exit 1
}
Write-Host "[RESULT] ALL MODULE LINE-COUNT TARGETS MET" -ForegroundColor Green
exit 0