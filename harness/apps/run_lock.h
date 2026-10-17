// One rule for every harness on this computer: any number of CPU-mode harnesses together, or
// one GPU harness alone (docs/machine/cpu-harness.md, "One card, many processors").
#pragma once

constexpr int kRunLockRefused = 75;   // the exit code of a harness the lock refused

// Takes the lock for the process's life (the OS frees it when the process ends, however).
// Refused: prints one line naming the holders and returns false.
bool run_lock_take(bool gpu, const char* program);
