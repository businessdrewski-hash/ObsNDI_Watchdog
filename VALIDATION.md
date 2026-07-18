# Validation notes — v0.5.0

Dependency-free checks cover:

- Automatic actions remain disabled until source assignments are confirmed.
- Adaptive thresholds never go below configured safety values and cap at 10 seconds.
- Automatic Protection maps to Auto; disabling it maps to Monitor while monitoring continues.
- Slow drift still maps only to Drift Controller or alert behavior, never receiver resets.
- Jump/stall classification, sample quarantine, reliability gating, non-destructive restoration, correction validation, and repeated-failure fail-safe behavior remain covered.
- All persisted controls have load, save, signal, and runtime paths.
- Source detection, Protection Test, support-report export, and effective-threshold code paths are present.
- Installer, workflow, and build metadata are v0.5.0.

Run:

```text
python3 tests/test_detection_model.py
python3 tests/test_parameter_paths.py
```

The Windows DLL and installer must still be compiled by the included GitHub Actions workflow for full OBS/DistroAV runtime validation.
