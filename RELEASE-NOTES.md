# Sync Guardian v0.5.0

This release turns the v0.4 monitoring and recovery engine into a streamer-oriented install-and-run experience.

On first launch, Sync Guardian proposes likely DistroAV video, desktop-audio, and microphone assignments. The main card shows the detected mapping and asks for one confirmation; no automatic correction or recovery is permitted before that confirmation. Automatic Protection then enables gradual Drift Controller correction and guarded targeted recovery behind one switch.

Normal healthy callback gaps are learned and used to raise effective stall thresholds when necessary, reducing false recoveries caused by routine OBS scheduling jitter. The learned threshold numbers remain visible in Protection settings. If desktop audio and microphone appear to originate from the same sender identity, linked correction is recommended automatically.

The compact dashboard now reports setup/readiness state, corrected offset, measurement reliability, and recent actions. A Protection Test performs a controlled group rebuild, restores captured DistroAV and OBS state, and verifies recovery. Support-report export writes a privacy-conscious JSON report without exposing mapped source names.

All detailed values remain available: raw and filtered offsets, baseline, drift slope, ppm correction, filter validation error, packet ages, timestamp jumps, reset counts, receiver properties, and event history.
