"""Dependency-free sanity model for Sync Guardian v0.3.0 behavior."""

VIDEO_STALL_MS = 1000
AUDIO_STALL_MS = 1000
STALL_CONFIRM_MS = 500
DRIFT_THRESHOLD_MS = 200
DRIFT_PERSISTENCE_MS = 10_000
COOLDOWN_SECONDS = 180
MAX_AUTO_RESETS_PER_HOUR = 3
STARTUP_GRACE_SECONDS = 30
FIRST_SAMPLE_GRACE_MS = 2_000
VERIFY_DELAY_SECONDS = 5
RESET_PULSE_MS = 180
PPM_PER_MS_PER_MINUTE = 16.6666666667


def ready_for_detection(video_timestamp_seen, desktop_timestamp_seen, settled_ms):
    return video_timestamp_seen and desktop_timestamp_seen and settled_ms >= FIRST_SAMPLE_GRACE_MS


def choose_issue(video_age, desktop_age, mic_age, drift, drift_duration_ms, mic_expected=True, ready=True):
    if not ready:
        return None
    video_fresh = video_age < VIDEO_STALL_MS
    desktop_fresh = desktop_age < AUDIO_STALL_MS
    mic_stale_if_expected = (not mic_expected) or mic_age >= AUDIO_STALL_MS

    if not video_fresh and not desktop_fresh and mic_stale_if_expected:
        return "group"
    if video_fresh and mic_expected and not desktop_fresh and mic_age >= AUDIO_STALL_MS:
        return "both-audio"
    if not video_fresh and desktop_fresh:
        return "video"
    if not desktop_fresh and video_fresh:
        return "desktop"
    if mic_expected and mic_age >= AUDIO_STALL_MS and (video_fresh or desktop_fresh):
        return "mic"
    if video_fresh and desktop_fresh and abs(drift) >= DRIFT_THRESHOLD_MS and drift_duration_ms >= DRIFT_PERSISTENCE_MS:
        return "video" if drift < 0 else "desktop"
    return None


def ppm_for_rate(rate_ms_per_minute, max_ppm=50):
    target = -rate_ms_per_minute * PPM_PER_MS_PER_MINUTE
    return max(-max_ppm, min(max_ppm, target))


def slew(current, target, ppm_per_second, elapsed_seconds):
    max_step = ppm_per_second * elapsed_seconds
    return current + max(-max_step, min(max_step, target - current))


def effective_drift(raw_transport_drift_ms, net_output_frames, sample_rate=48_000):
    return raw_transport_drift_ms + net_output_frames * 1000.0 / sample_rate


def reset_restores_original_framesync(original_framesync):
    pulse_value = not original_framesync
    restored_value = original_framesync
    return pulse_value, restored_value


def run():
    assert STALL_CONFIRM_MS == 500
    assert COOLDOWN_SECONDS == 180
    assert MAX_AUTO_RESETS_PER_HOUR == 3
    assert STARTUP_GRACE_SECONDS == 30
    assert FIRST_SAMPLE_GRACE_MS == 2_000
    assert VERIFY_DELAY_SECONDS == 5
    assert RESET_PULSE_MS == 180

    assert not ready_for_detection(False, True, 60_000)
    assert not ready_for_detection(True, False, 60_000)
    assert not ready_for_detection(True, True, 1_999)
    assert ready_for_detection(True, True, 2_000)
    assert choose_issue(float("inf"), 20, 20, 0, 0, ready=False) is None
    assert choose_issue(20, 20, 20, 70, 60_000) is None
    assert choose_issue(1200, 20, 20, 0, 0) == "video"
    assert choose_issue(20, 1200, 20, 0, 0) == "desktop"
    assert choose_issue(20, 20, 1200, 0, 0) == "mic"
    assert choose_issue(20, 1200, 1200, 0, 0) == "both-audio"
    assert choose_issue(1200, 1200, 1200, 0, 0) == "group"
    assert choose_issue(1200, 1200, 0, 0, 0, mic_expected=False) == "group"
    assert choose_issue(20, 20, 20, -250, 9_000) is None
    assert choose_issue(20, 20, 20, -250, 10_000) == "video"
    assert choose_issue(20, 20, 20, 250, 10_000) == "desktop"

    # A -1.5 ms/min trend needs approximately +25 ppm (slightly slower audio).
    assert abs(ppm_for_rate(-1.5) - 25.0) < 1e-6
    assert abs(ppm_for_rate(1.5) + 25.0) < 1e-6
    assert ppm_for_rate(-10.0) == 50
    assert ppm_for_rate(10.0) == -50
    assert slew(0.0, 25.0, 1.0, 0.25) == 0.25
    assert slew(10.0, -10.0, 2.0, 0.5) == 9.0
    assert abs(effective_drift(-20.0, 480) - (-10.0)) < 1e-9

    # Soft Sync may make corrected output drift near zero, while reset fallback
    # still watches raw transport drift so accumulated trim is periodically recentered.
    raw_transport_drift = -200.0
    corrected_output_drift = effective_drift(raw_transport_drift, 9_600)
    assert abs(corrected_output_drift) < 1e-9
    assert abs(raw_transport_drift) >= DRIFT_THRESHOLD_MS

    # Reset pulses may temporarily invert FrameSync, but must restore the exact
    # pre-reset value for both audio-pass-through and video FrameSync setups.
    assert reset_restores_original_framesync(False) == (True, False)
    assert reset_restores_original_framesync(True) == (False, True)

    # Fully disabled Soft Sync means no filter correction is applied.
    soft_sync_enabled = False
    correction_ppm = ppm_for_rate(-1.5) if soft_sync_enabled else 0.0
    assert correction_ppm == 0.0

    print("Sync Guardian v0.3.0 detection, reset-preservation, and Soft Sync model checks passed")


if __name__ == "__main__":
    run()
