# Detection and recovery design

## Background watchdog

The watchdog samples every 250 ms on its own thread. It handles timestamp sampling, stall/drift persistence, reset initiation, the reset-pulse deadline, exact settings restoration, verification, cooldowns, and optional automatic escalation. The Qt dock only presents the latest state once per second.

## A/V comparison

Video and desktop-audio timestamps are projected to the same monotonic wall-clock moment. The visible offset uses a 10-second median. A robust 60-second slope drives the displayed rate, while a robust 120-second slope is available to the optional Soft Sync controller. Thirty stable seconds establish the baseline.

When Adaptive Soft Sync is attached, measurement uses the filter's raw input timestamp rather than a possibly corrected post-filter timestamp. The controller then adds its accumulated sample trim separately to estimate corrected output drift.

## Reset pulse and settings preservation

A reset temporarily flips DistroAV FrameSync to force receiver reconstruction. Before the pulse, Sync Guardian captures the complete source settings and explicitly records FrameSync, latency, timing mode, and NDI target. At the pulse deadline it clears the temporary settings, restores the complete captured object, verifies the critical values, and retries once if necessary.

The temporary flip is not a requested final setting. An audio source that entered the reset with FrameSync off must finish with FrameSync off.

## Adaptive Soft Sync

Soft Sync is optional, disabled by default, and removable. It applies a bounded linear resample correction to desktop audio. Positive ppm adds a tiny amount of duration to slow audio; negative ppm removes a tiny amount to speed it. Defaults are a 12 ms position dead zone, 50 ppm maximum, and 1 ppm/sec slew.

The mapped mic is not corrected unless linked explicitly. Disabling Soft Sync detaches the private filters and zeros all correction state.

Persistent-drift reset fallback still watches raw transport drift. A successful video/group recovery recenters accumulated desktop and linked-mic trim, preventing Soft Sync from hiding an ever-growing transport offset indefinitely.

## Default safeguards

- Observe only by default.
- 1000 ms stale threshold plus 500 ms confirmation.
- 200 ms drift for 10 seconds.
- 180-second automatic reset cooldown.
- Three automatic reset actions per hour.
- Thirty-second startup/scene-change grace.
- Five-second post-reset verification.
- At most one full-group escalation after a targeted reset fails.
