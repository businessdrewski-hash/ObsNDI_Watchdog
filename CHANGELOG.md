# Changelog

## v0.5.0

- Added a hands-free first-run path that detects likely DistroAV video, desktop-audio, and microphone roles.
- Added one-click source confirmation; automatic actions stay locked until the proposed mapping is confirmed.
- Replaced the normal mode selector with a single Automatic Protection switch while retaining Monitor/Ask/Auto under Protection settings.
- Enabled Drift Controller automatically with Automatic Protection and recommends linked mic correction when both audio sources expose the same sender identity.
- Added adaptive stall thresholds learned from healthy callback gaps; the learned effective numbers remain visible.
- Added plain-language readiness states and temporary success/failure notifications on the compact dashboard.
- Added a controlled Protection Test that captures settings, rebuilds the mapped group, restores it non-destructively, and verifies fresh timestamps.
- Added one-click privacy-conscious support-report export with measurements, controller state, thresholds, and source health counters.
- Kept raw/filtered offsets, baseline, slope, ppm, correction validation, packet ages, timestamps, jumps, resets, properties, and event history in expandable diagnostics.

## v0.4.0

- Moved every major section chevron to the left, immediately beside its title.
- Reduced section-toggle width and allowed titles to yield horizontal space so the chevrons remain visible in very narrow OBS docks.
- Added a compact everyday dashboard with status, corrected offset, Monitor/Ask/Auto mode, and contextual Fix button.
- Separated timestamp jumps/stalls from slow drift; gradual drift can no longer reset receivers.
- Added per-source jump classification and targeted jump recovery.
- Replaced the misleading confidence display with Waiting, Calibrating, Reliable, and Unreliable measurement states.
- Quarantined incident/recovery samples from calibration and drift estimation.
- Changed calibration to require a stable, reliable window and persist the last known baseline.
- Paused and recentered Drift Controller during incidents and added correction-output validation and saturation reporting.
- Added repeated-failure fallback to Monitor mode, DistroAV property compatibility checks, source recreation handling, and v3-to-v4 settings migration.
- Added dependency-free behavior tests and a load/save/runtime audit for all persisted parameters.

## v0.3.4 — High-range Adaptive Sync hotfix

- Raised the configurable Adaptive Soft Sync ceiling from 200 ppm to 5000 ppm.
- Raised the internal resampler clamp to ±5000 ppm.
- Added a smooth x^1.5 catch-up curve: gentle near the dead zone, progressively faster for larger corrected errors, reaching the selected ceiling around 250 ms outside the dead zone.
- Expanded the slew control to 1000 ppm/sec and changed braking/deceleration to run at twice the configured acceleration rate to reduce lingering correction and overshoot.
- Kept the default maximum correction at the conservative 50 ppm for existing users.

## v0.3.4

- Rebuilt collapsible sections as compact one-line headers.
- Collapsed sections now remove their content height completely.
- Replaced large Show/Hide buttons with small chevrons.
- Reduced UI padding and spacing throughout the dock.

## v0.3.2

- Changed ordinary persistent-drift detection to use estimated corrected output drift whenever Adaptive Soft Sync is active.
- Kept raw transport drift visible in diagnostics while avoiding resets for drift already corrected by Adaptive Soft Sync.
- Reworked Live Diagnostics around a large color-coded `Drift from baseline` line.
- Added an inline `Adaptive Sync corrected to ...` value.
- Moved trend, direction, and baseline to a shorter secondary line.
- Added a show/hide control for technical diagnostic details.
- Made every major dock section collapsible.
- Simplified the Adaptive Soft Sync status wording.

## v0.3.1

- Changed receiver restoration from destructive settings replacement to non-destructive settings update.
- Preserved DistroAV properties plus OBS sync offset, audio tracks, monitoring mode, volume, mute, balance, source flags, and related runtime settings during targeted and full-group rebuilds.
- Applied the same preservation behavior to Restore Saved Settings.

## v0.3.0

- Moved stall detection, reset scheduling, pulse restoration, verification, and automatic recovery to a background watchdog independent of Qt hover/focus events.
- Changed the visible drift and rate refresh cadence to 1000 ms while retaining 250 ms internal sampling.
- Expanded the offset median to 10 seconds and added robust 60-second display and 120-second controller rate estimates.
- Added plain-English drift direction text.
- Increased dark-theme contrast for secondary text and tooltips.
- Reset pulses capture, restore, and verify the original DistroAV configuration, including FrameSync off on audio sources.
- Added experimental Adaptive Soft Sync, disabled by default and fully removable.
- Added a Windows installer workflow producing `SyncGuardian-Setup.exe`, a portable ZIP, and checksums.
