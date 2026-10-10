param([string]$OutputDirectory = 'artifacts/local/gallery-cluster/performance')
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskOutput = [System.IO.Path]::GetFullPath((Join-Path $taskRoot $OutputDirectory))
$taskExecutable = Join-Path $taskRoot 'build/windows-msvc/bin/Release/weft_d3d11_probe.exe'
foreach ($scenario in @('d3-interactive', 'kline', 'framer', 'spring', 'icons', 'anime', 'css-middle')) {
    $destination = Join-Path $taskOutput $scenario
    if (Test-Path -LiteralPath $destination) { throw "Output already exists: $destination" }
    New-Item -ItemType Directory -Path $destination -Force | Out-Null
    $arguments = @('--gallery-benchmark-hardware', ('"{0}"' -f $destination), $scenario)
    $process = Start-Process -FilePath $taskExecutable -ArgumentList $arguments -WorkingDirectory $taskRoot -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $destination 'stdout.log') -RedirectStandardError (Join-Path $destination 'stderr.log')
    $process.WaitForExit()
    if ($process.ExitCode -ne 0) { throw "Benchmark failed: $scenario / $($process.ExitCode)" }
    $result = Get-Content -Raw -LiteralPath (Join-Path $destination 'benchmark.json') | ConvertFrom-Json
    Write-Output "$scenario : $($result.warm_throughput_fps) FPS"
}
