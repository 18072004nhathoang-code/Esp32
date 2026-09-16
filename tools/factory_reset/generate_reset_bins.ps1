# ============================================================
# ESP32-S3 Factory Reset - Tao file blank .bin
# Khong can Python rieng - dung .NET (co san tren Windows)
# ============================================================

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

# Partition layout (tu partitions.csv)
$partitions = @{
    "nvs"     = @{ offset = 0x9000;   size = 0x5000   }  # 20 KB
    "otadata" = @{ offset = 0xE000;   size = 0x2000   }  # 8 KB
}

Write-Host ""
Write-Host "============================================================" -ForegroundColor Cyan
Write-Host "  ESP32-S3 Factory Reset - Blank Binary Generator" -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan
Write-Host ""

foreach ($name in $partitions.Keys) {
    $size    = $partitions[$name]["size"]
    $outFile = Join-Path $scriptDir "${name}_blank.bin"

    # Tao mang byte 0xFF
    $data = [byte[]]::new($size)
    for ($i = 0; $i -lt $size; $i++) { $data[$i] = 0xFF }

    [System.IO.File]::WriteAllBytes($outFile, $data)
    Write-Host "[OK] $outFile  ($size bytes = $($size/1KB) KB)" -ForegroundColor Green
}

Write-Host ""
Write-Host "[INFO] Tao xong! File .bin da san sang tai:" -ForegroundColor Yellow
Write-Host "       $scriptDir" -ForegroundColor Yellow
Write-Host ""
Write-Host "Tiep theo: mo flash_reset.bat, doi COM_PORT va chay!" -ForegroundColor Cyan
Write-Host "============================================================" -ForegroundColor Cyan
