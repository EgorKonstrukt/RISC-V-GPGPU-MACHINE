# One harness run at a time, for every session on this computer. Runs the command when its turn comes.
#   pwsh tools\gpu_queue.ps1 bin\rvc_harness.exe --d3d11 --rvc experiments\rvc_opt ...
# Every rvc_harness run goes through it, GPU or CPU, --cpu --warp included. The harnesses' own
# lock (run_lock) refuses one that breaks "many CPU harnesses or one GPU harness" with exit 75
# (docs/machine/cpu-harness.md, "One card, many processors"); it does not wait, this queue does.
$mutex = [Threading.Mutex]::new($false, 'Global\ShaderX86-gpu-run')
$waited = [Diagnostics.Stopwatch]::StartNew()
try { [void]$mutex.WaitOne() } catch [Threading.AbandonedMutexException] { }   # its holder died: the turn is ours
if ($waited.Elapsed.TotalSeconds -ge 1) { [Console]::Error.WriteLine("gpu_queue: waited $([int]$waited.Elapsed.TotalSeconds) s for the card") }
try {
    $command, $rest = $args
    & $command @rest
    # 75: the harness's own lock refused it (a CPU harness running, or another GPU one); its line says which
    if ($LASTEXITCODE -eq 75) { [Console]::Error.WriteLine("gpu_queue: REFUSED by the harness lock, nothing ran (see the [lock] line above)") }
    exit $LASTEXITCODE
} finally {
    $mutex.ReleaseMutex()
}
