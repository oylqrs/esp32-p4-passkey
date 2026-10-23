param(
    [string]$Compiler = 'C:\Espressif\tools\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\bin\riscv32-esp-elf-gcc.exe',
    [string]$Python = 'C:\Espressif\tools\python\v5.4.4\venv\Scripts\python.exe',
    [switch]$CompileOnly
)
$ErrorActionPreference = 'Stop'
$portDirectory = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$buildDirectory = Join-Path $PSScriptRoot 'build'
New-Item -ItemType Directory -Path $buildDirectory -Force | Out-Null
$hostDirectory = Join-Path $PSScriptRoot 'host'
$includeDirectory = Join-Path $portDirectory 'include'
$testElf = Join-Path $buildDirectory 'nfc_bridge_test.elf'
& $Compiler -march=rv32ima -mabi=ilp32 -std=c17 -O0 -ffreestanding -fno-builtin -msmall-data-limit=0 -nostdlib -Wall -Wextra -Wno-unused-variable -DCANOKEY_NFC_POLL_SCHEDULING=0 -I $hostDirectory -I $includeDirectory '-Wl,-Ttext=0x10000' '-Wl,-e,test_nfc_bridge' '-Wl,--no-relax' '-Wl,--wrap=get_touch_result' (Join-Path $portDirectory 'port_nfc.c') (Join-Path $PSScriptRoot 'test_nfc_bridge.c') -o $testElf
if ($LASTEXITCODE -ne 0) { throw 'NFC bridge test compile failed' }
if ($CompileOnly) {
    Write-Output 'NFC bridge test harness compiled (behavior tests not executed)'
    return
}
& $Python (Join-Path $PSScriptRoot 'run_nfc_bridge.py') $testElf
if ($LASTEXITCODE -ne 0) { throw 'NFC bridge behavior tests failed' }
