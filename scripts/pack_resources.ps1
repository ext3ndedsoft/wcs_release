$ErrorActionPreference = "Stop"

$baseDir = Join-Path $PSScriptRoot "..\resources"
$outputFile = Join-Path $PSScriptRoot "..\src\resources.pak"

if (!(Test-Path $baseDir)) {
    Write-Error "Resources directory not found at $baseDir"
    exit 1
}

$subdirs = @("branding", "icons", "web", "covers")
$fileList = [System.Collections.Generic.List[string]]::new()
$baseFull = (Resolve-Path $baseDir).Path

foreach ($sub in $subdirs) {
    $fullSub = Join-Path $baseFull $sub
    if (Test-Path $fullSub) {
        Get-ChildItem -Path $fullSub -Recurse -File | ForEach-Object {
            if ($_.Name -notlike "*original*" -and $_.FullName -notlike "*\covers\cache\*" -and $_.Extension -ne ".nocover") {
                $fileList.Add($_.FullName)
            }
        }
    }
}

$fs = [System.IO.FileStream]::new($outputFile, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write)
$bw = [System.IO.BinaryWriter]::new($fs)

try {
    # Magic "WCSP"
    $bw.Write([System.Text.Encoding]::ASCII.GetBytes("WCSP"))
    $bw.Write([uint32]$fileList.Count)

    foreach ($file in $fileList) {
        $relPath = $file.Substring($baseFull.Length).TrimStart('\', '/').Replace('\', '/')
        $relBytes = [System.Text.Encoding]::UTF8.GetBytes($relPath)
        $content = [System.IO.File]::ReadAllBytes($file)

        $bw.Write([uint16]$relBytes.Length)
        $bw.Write($relBytes)
        $bw.Write([uint32]$content.Length)
        $bw.Write($content)
    }
} finally {
    $bw.Dispose()
    $fs.Dispose()
}

$pakItem = Get-Item $outputFile
Write-Host "[PACK] Successfully packed $($fileList.Count) files into $($pakItem.FullName) ($([Math]::Round($pakItem.Length / 1MB, 2)) MB)"
