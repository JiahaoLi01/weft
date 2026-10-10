param([string]$Condition = 'all')
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskOutput = Join-Path $taskRoot 'artifacts/local/render-stages'
$executables = @{
    before = Join-Path $taskOutput 'baseline/build/bin/Release/ce_d3d11_probe.exe'
    after = Join-Path $taskRoot 'build/windows-msvc/bin/Release/ce_d3d11_probe.exe'
}
$conditions = if ($Condition -eq 'all') { @('paired','controlled') } else { @($Condition) }
foreach ($sampling in $conditions) {
    if ($sampling -notin @('paired','controlled','sanity')) { throw 'Unknown sampling condition' }
    $rounds = if ($sampling -eq 'sanity') { @(1) } else { @(1,2,3) }
    $scenarios = if ($sampling -eq 'sanity') { @('css-top','d3') } else { @('css-middle','css-end') }
    foreach ($round in $rounds) {
        foreach ($scenario in $scenarios) {
            $phases = if ($round -eq 2) { @('after','before') } else { @('before','after') }
            foreach ($phase in $phases) {
                $destination = Join-Path $taskOutput "sampling-v2/$sampling/round-$round/$phase/$scenario"
                if (Test-Path -LiteralPath (Join-Path $destination 'benchmark.json')) { throw "Already sampled: $destination" }
                New-Item -ItemType Directory -Force -Path $destination | Out-Null
                $arguments = @('--gallery-benchmark-hardware', ('"{0}"' -f $destination), $scenario)
                $process = Start-Process -FilePath $executables[$phase] -ArgumentList $arguments -WorkingDirectory $taskRoot -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $destination 'stdout.log') -RedirectStandardError (Join-Path $destination 'stderr.log')
                if ($sampling -eq 'controlled') {
                    $process.ProcessorAffinity = [IntPtr]65535
                    $process.PriorityClass = 'High'
                    @{ affinity_mask = $process.ProcessorAffinity.ToInt64(); priority = [string]$process.PriorityClass; set_during_startup = $true } | ConvertTo-Json | Set-Content -Encoding utf8 (Join-Path $destination 'process-controls.json')
                }
                $process.WaitForExit()
                if ($process.ExitCode -ne 0) { throw "Benchmark failed: $destination / $($process.ExitCode)" }
                $result = Get-Content -Raw -LiteralPath (Join-Path $destination 'benchmark.json') | ConvertFrom-Json
                Write-Output "$sampling / $round / $scenario / $phase : $($result.warm_throughput_fps) FPS"
            }
        }
    }
}
