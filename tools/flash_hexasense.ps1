# HexaSense Flash Writer Helper (Windows)
# Source: docs/spresense_official/04_sdk_build_and_flashing_guide.md Sec.3
# Usage: powershell -ExecutionPolicy Bypass -File tools/flash_hexasense.ps1 -ComPort COM6 -SpkPath sdk/nuttx.spk

param(
    [string]$ComPort = "COM6",
    [string]$SpkPath = "nuttx.spk",
    [string]$SdkDir = "$HOME/spresense/sdk"
)

$flashWriter = Join-Path $SdkDir "tools/flash_writer/scripts/flash_writer.py"
if (-not (Test-Path $flashWriter)) {
    # fallback: try relative path when called from spresense/sdk
    $flashWriter = "tools/flash_writer/scripts/flash_writer.py"
}
if (-not (Test-Path $flashWriter)) {
    Write-Error "flash_writer.py not found: $flashWriter`n先に Spresense SDK を展開してください (spresense/sdk/tools/...)"
    exit 1
}
if (-not (Test-Path $SpkPath)) {
    $alt = Join-Path $SdkDir $SpkPath
    if (Test-Path $alt) { $SpkPath = $alt }
    else {
        Write-Error "nuttx.spk not found: $SpkPath`n先に 'make -j$(nproc)' でビルドしてください"
        exit 1
    }
}

Write-Host "Flashing $SpkPath -> $ComPort via $flashWriter"
python $flashWriter -c $ComPort -d -s $SpkPath
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "`n書き込み完了。115200bpsでシリアル接続し 'nsh> synth` で起動、`File->Disconnect` 後にヘッドホン出力を確認してください。"
