# One run on the graphics card at a time, for every session on this computer: a timing taken
# while another harness runs beside it is wrong. Runs the command when its turn comes.
#   pwsh tools\gpu_queue.ps1 bin\rvc_harness.exe --d3d11 --rvc experiments\rvc_opt ...
# Only for runs whose seconds matter (instructions a second, frame rates, pass utilization).
# bin\rvc_cpu.exe needs no turn: it counts instructions and does not use the card.
$mutex = [Threading.Mutex]::new($false, 'Global\ShaderX86-gpu-run')
$waited = [Diagnostics.Stopwatch]::StartNew()
try { [void]$mutex.WaitOne() } catch [Threading.AbandonedMutexException] { }   # its holder died: the turn is ours
if ($waited.Elapsed.TotalSeconds -ge 1) { [Console]::Error.WriteLine("gpu_queue: waited $([int]$waited.Elapsed.TotalSeconds) s for the card") }
try {
    $command, $rest = $args
    & $command @rest
    exit $LASTEXITCODE
} finally {
    $mutex.ReleaseMutex()
}
