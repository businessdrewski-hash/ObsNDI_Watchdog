# Sync Guardian for OBS Studio

Sync Guardian is an experimental OBS Studio plugin for people dealing with **NDI / DistroAV audio-video sync problems** on a receiving or streaming PC.

It monitors selected NDI sources, tracks gradual A/V drift, detects timestamp stalls and receiver failures, and can recover individual sources or rebuild an entire sync group while preserving the source settings you already configured.

It is designed for setups where video, desktop audio, and microphone audio may arrive through separate NDI sources and slowly move out of alignment over time.

> Install Sync Guardian on the **receiving PC** where the DistroAV sources exist in OBS.

## What problems it is designed to help with

Sync Guardian may be useful when an NDI-based OBS setup experiences problems such as:

- Video gradually drifting behind or ahead of desktop audio
- A/V sync changing over a long stream or recording
- An NDI video source freezing while audio continues
- A source reconnecting with the wrong timing relationship
- OBS or DistroAV sources failing to recover until the interface is interacted with
- Manual receiver resets changing or wiping source properties
- Separate NDI audio sources needing to remain aligned with video

Sync Guardian does not replace DistroAV or NDI. It watches the sources already present in OBS and adds monitoring, controlled recovery, diagnostics, and optional timing correction.

## Hands-free setup

On first launch, Sync Guardian proposes likely DistroAV video, desktop-audio, and microphone assignments. Review the names on the main card and press **Confirm detected setup**. Automatic Protection remains locked until this confirmation, then becomes ready after stable timestamps have been measured.

The normal dock shows only protection status, corrected offset, readiness, Automatic Protection, and a contextual Fix button. Open the collapsed sections whenever raw numbers or manual controls are needed.

Use **Test protection** before an important stream to perform a controlled receiver rebuild and verify that DistroAV and OBS source settings survive. **Export support report** creates a privacy-conscious diagnostic JSON file when troubleshooting is needed.

## Main features

### Live A/V drift monitoring

Sync Guardian compares the selected video and desktop-audio timestamps and establishes a session baseline.

The main diagnostic line shows:

- Current drift from baseline
- Whether video is gradually rushing or dragging
- Estimated drift rate in milliseconds per minute
- Raw transport drift
- Estimated corrected drift when Adaptive Soft Sync is active

The display refreshes once per second while timestamps continue to be sampled internally at a higher rate. Median filtering and longer trend windows reduce noisy rate changes.

### Clear, color-coded diagnostics

The Live Diagnostics area emphasizes the most important value: **Drift from baseline**.

- Green: near the current sync target
- Amber: moving into a concerning range
- Red: beyond the configured persistent-drift threshold

Technical details such as packet age, raw timestamps, jump counters, measurement reliability, and OBS statistics can be shown only when needed.

### Background watchdog

Detection and recovery run independently of the dock's paint and mouse events.

This means automatic recovery should continue even when:

- OBS is unfocused
- The dock is collapsed
- The mouse is not moving over the interface
- The UI is temporarily idle

The watchdog handles jump/stall detection, Drift Controller timing, reset pulse completion, verification, escalation, and cooldowns. Gradual drift never triggers a receiver reset.

### Targeted NDI receiver reset

A single mapped source can be reset without restarting OBS.

Sync Guardian briefly changes a DistroAV receiver property to force receiver recreation, then restores the complete captured configuration. Reset pulses preserve the original FrameSync state rather than automatically enabling it.

### Full sync-group rebuild

Video, desktop audio, and microphone sources can be rebuilt together when a single-source reset is not enough.

Before the rebuild, Sync Guardian captures and restores:

- DistroAV source selection and receiver properties
- FrameSync state
- Timing and latency settings
- OBS sync offset
- Audio-track routing
- Monitoring mode
- Volume, mute, and balance
- Push-to-talk and push-to-mute settings
- Enabled state and source flags
- Deinterlacing settings

The restore path is non-destructive, so properties not explicitly understood by Sync Guardian are not intentionally erased.

### Automatic recovery modes

Sync Guardian supports multiple operating modes:

- **Observe only** — display and log problems without changing sources
- **Ask before recovery** — request confirmation before taking action
- **Fully automatic** — perform targeted recovery when configured conditions persist

Safety controls include startup grace periods, scene-change grace periods, reset cooldowns, verification, escalation limits, and maximum automatic resets per hour.

### Persistent drift recovery

A configurable persistent-drift threshold can trigger recovery when A/V drift remains outside the allowed range long enough.

Threshold behavior depends on Adaptive Soft Sync:

- **Adaptive Soft Sync off:** the threshold uses raw transport drift
- **Adaptive Soft Sync active:** the threshold uses estimated corrected output drift

Example:

```text
Raw drift:                 -50 ms
Adaptive correction:       +48 ms
Estimated corrected drift:  -2 ms
```

With a 50 ms threshold, that corrected result does not trigger ordinary persistent-drift recovery.

### Adaptive Soft Sync

Adaptive Soft Sync is an **experimental, optional audio-resampling system** intended to counter slow, steady drift without using large audible cuts.

When enabled, Sync Guardian attaches a private audio filter to the mapped desktop-audio source and applies a very small correction measured in parts per million.

A typical drift of about `-1.5 ms/min` requires only approximately `25 ppm` of correction. The default limit is conservative, and the controller changes correction gradually rather than reacting to every short-term fluctuation.

Adaptive Soft Sync can:

- Slow or speed the effective audio timeline by tiny amounts
- Hold corrected output closer to the calibrated baseline
- Avoid treating already-corrected drift as an ordinary threshold violation
- Optionally apply linked correction to the microphone source
- Recenter after a receiver reset

It can also be completely disabled. Clearing **Enable Adaptive Soft Sync** or pressing **Disable and remove Soft Sync filters** removes Sync Guardian's private filters and returns audio to normal pass-through.

### Event history and diagnostics export

Sync Guardian records important events such as:

- Timestamp jumps
- Source stalls
- Persistent drift episodes
- Reset attempts
- Verification results
- Escalation and cooldown states

The dock is arranged into collapsible sections so routine use can remain compact while detailed information stays available for troubleshooting.

## Recommended first-time setup

1. Install the plugin on the receiving or streaming PC.
2. Open OBS and select **Docks → Sync Guardian**.
3. Map the NDI video, desktop-audio, and optional microphone sources.
4. Begin in **Observe only** mode.
5. Let the system establish a baseline and run long enough to understand the normal drift pattern.
6. Configure conservative thresholds before enabling automatic recovery.
7. Leave Adaptive Soft Sync disabled until ordinary monitoring and reset behavior are confirmed stable.

For split-source setups, audio FrameSync behavior should be tested carefully. Some systems are more stable with FrameSync disabled on audio-only NDI sources.

## Installation

### Windows installer

Download the Windows build artifact from GitHub Actions and run:

```text
SyncGuardian-Setup.exe
```

Close OBS before installing. The default OBS location is:

```text
C:\Program Files\obs-studio\
```

The installer adds normal Windows uninstall support.

### Portable installation

Extract `SyncGuardian-Portable.zip`, then copy its `obs-plugins` and `data` folders into:

```text
C:\Program Files\obs-studio\
```

The plugin DLL should end up at:

```text
C:\Program Files\obs-studio\obs-plugins\64bit\sync-guardian.dll
```

Start OBS and open:

```text
Docks → Sync Guardian
```

## Building from GitHub

Open the repository's **Actions** tab and run:

```text
Build Sync Guardian for Windows
```

The generated artifact contains:

- `SyncGuardian-Setup.exe`
- `SyncGuardian-Portable.zip`
- `SHA256SUMS.txt`

The source package itself does not include a precompiled Windows DLL.

## Changelog

### v0.5.0 — Hands-free protection and easier setup

- Automatically proposes DistroAV video, desktop-audio, and microphone assignments, then requires one confirmation before automatic actions are allowed.
- Added a single **Automatic Protection** switch, plain-language readiness states, adaptive stall thresholds, source-clock-based mic-link suggestions, and temporary action notifications.
- Added **Test protection** for a controlled rebuild/restore/verification cycle and **Export support report** for privacy-conscious diagnostics.
- Reduced the normal dock to the essential protection card while keeping all timing numbers and manual controls expandable.

### v0.4.0 — Separate drift correction from jump recovery

- Separated gradual clock drift from timestamp jumps and stalls: drift uses the Drift Controller, while confirmed discontinuities use targeted receiver recovery.
- Added per-source jump classification, incident-sample quarantine, stability-based calibration, real measurement-reliability states, correction-output validation, and correction-range warnings.
- Added repeated-failure fallback to Monitor mode, automatic source recreation handling, DistroAV compatibility checks, and v3 settings migration.
- Moved every section chevron to the left and made the compact dock scale correctly at narrow widths.

### v0.3.x — Reliable background monitoring and correction

- Moved monitoring, reset restoration, verification, escalation, and cooldown timing to an independent 250 ms watchdog.
- Added robust median/trend estimation and Adaptive Soft Sync with configurable ppm limits, smooth catch-up, slew control, and corrected-output drift reporting.
- Added non-destructive restoration of DistroAV settings and OBS audio/runtime properties.
- Added compact collapsible diagnostics, the private video timestamp probe, installer generation, portable packaging, and checksums.

### v0.2.x — Automatic detection and usable diagnostics

- Added automatic stall/drift detection, Monitor/Ask/Auto modes, cooldowns, startup grace, verification, escalation, event logging, source validation, and clearer dock behavior.
- Improved small-window scaling, visual grouping, diagnostic summaries, GitHub Actions packaging, and video frame/timestamp readiness tracking.

### v0.1.x — Manual recovery foundation

- Added the OBS dock, mapped DistroAV roles, targeted receiver resets, saved-state restoration, and the reset-pulse receiver recreation method.

See [CHANGELOG.md](CHANGELOG.md) for the detailed release history.

## Important limitations

Sync Guardian is experimental software intended for controlled testing before use on an important live production.

- Timestamp drift does not always equal viewer-visible lip-sync error exactly
- Adaptive Soft Sync still requires broad real-world testing across OBS, DistroAV, NDI, audio hardware, and network configurations
- Receiver resets may create a brief interruption while DistroAV reconnects
- Large sender-side timestamp discontinuities cannot always be repaired without a reset
- Hardware clock locking, combined A/V NDI streams, or professional synchronization systems may still be preferable in demanding environments

A local recording with visible and audible sync markers remains the best way to verify final viewer-facing alignment.

## License

See [LICENSE](LICENSE).
