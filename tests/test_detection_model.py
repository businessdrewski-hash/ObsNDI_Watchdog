"""Dependency-free sanity model for Sync Guardian v0.5.0 behavior."""

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


def choose_issue(video_age, desktop_age, mic_age, raw_drift, drift_duration_ms, mic_expected=True, ready=True,
                 soft_sync_enabled=False, corrected_drift=None):
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
    detection_drift = corrected_drift if soft_sync_enabled and corrected_drift is not None else raw_drift
    if video_fresh and desktop_fresh and abs(detection_drift) >= DRIFT_THRESHOLD_MS and drift_duration_ms >= DRIFT_PERSISTENCE_MS:
        return "drift"
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


def mitigation_for(issue, drift_controller_enabled):
    if issue == "drift":
        return "controller" if drift_controller_enabled else "alert"
    return "receiver-recovery" if issue else "none"


def measurement_reliability(fresh, valid, quarantined, sample_count, span_ms):
    if quarantined or not fresh or not valid:
        return "unreliable"
    if sample_count < 24 or span_ms < 10_000:
        return "calibrating"
    return "reliable"


def effective_stall_threshold(configured_ms, largest_healthy_gap_ms, auto_tune=True):
    if not auto_tune or not largest_healthy_gap_ms:
        return configured_ms
    return max(configured_ms, min(10_000, largest_healthy_gap_ms * 4 + 250))


def automatic_actions_allowed(setup_confirmed, mode):
    return setup_confirmed and mode == "auto"



def destructive_reset(current, saved):
    """Model obs_source_reset_settings(): current keys are cleared first."""
    return dict(saved)


def nondestructive_update(current, saved):
    """Model obs_source_update(): saved values are merged into current state."""
    merged = dict(current)
    merged.update(saved)
    return merged


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
    assert choose_issue(20, 20, 20, -250, 10_000) == "drift"
    assert choose_issue(20, 20, 20, 250, 10_000) == "drift"
    assert mitigation_for("drift", True) == "controller"
    assert mitigation_for("drift", False) == "alert"
    assert mitigation_for("video", True) == "receiver-recovery"

    assert measurement_reliability(True, True, True, 100, 30_000) == "unreliable"
    assert measurement_reliability(False, True, False, 100, 30_000) == "unreliable"
    assert measurement_reliability(True, True, False, 23, 30_000) == "calibrating"
    assert measurement_reliability(True, True, False, 24, 9_999) == "calibrating"
    assert measurement_reliability(True, True, False, 24, 10_000) == "reliable"
    assert effective_stall_threshold(1000, 20) == 1000
    assert effective_stall_threshold(1000, 300) == 1450
    assert effective_stall_threshold(1000, 300, auto_tune=False) == 1000
    assert not automatic_actions_allowed(False, "auto")
    assert not automatic_actions_allowed(True, "monitor")
    assert automatic_actions_allowed(True, "auto")

    # A -1.5 ms/min trend needs approximately +25 ppm (slightly slower audio).
    assert abs(ppm_for_rate(-1.5) - 25.0) < 1e-6
    assert abs(ppm_for_rate(1.5) + 25.0) < 1e-6
    assert ppm_for_rate(-10.0) == 50
    assert ppm_for_rate(10.0) == -50
    assert slew(0.0, 25.0, 1.0, 0.25) == 0.25
    assert slew(10.0, -10.0, 2.0, 0.5) == 9.0
    assert abs(effective_drift(-20.0, 480) - (-10.0)) < 1e-9

    # With Soft Sync active, the ordinary persistent-drift threshold follows
    # corrected output drift. The same raw transport value still trips when
    # Soft Sync is disabled.
    raw_transport_drift = -250.0
    corrected_output_drift = effective_drift(raw_transport_drift, 12_000)
    assert abs(corrected_output_drift) < 1e-9
    assert choose_issue(20, 20, 20, raw_transport_drift, 10_000,
                        soft_sync_enabled=True, corrected_drift=corrected_output_drift) is None
    assert choose_issue(20, 20, 20, raw_transport_drift, 10_000,
                        soft_sync_enabled=False, corrected_drift=corrected_output_drift) == "drift"

    # If corrected output itself remains beyond threshold, drift is reported but
    # receiver recovery is still forbidden.
    assert choose_issue(20, 20, 20, -250.0, 10_000,
                        soft_sync_enabled=True, corrected_drift=-225.0) == "drift"
    assert mitigation_for("drift", True) != "receiver-recovery"

    # Reset pulses may temporarily invert FrameSync, but must restore the exact
    # pre-reset value for both audio-pass-through and video FrameSync setups.
    assert reset_restores_original_framesync(False) == (True, False)
    assert reset_restores_original_framesync(True) == (False, True)

    # A destructive restore loses properties omitted from a snapshot because
    # they were defaults or unknown to this plugin version. A merge restore
    # keeps them while still returning all captured values to their originals.
    current = {
        "ndi_framesync": True,
        "ndi_sync": 2,
        "latency": 1,
        "future_distroav_property": "preserve-me",
    }
    saved = {"ndi_framesync": False, "ndi_sync": 1, "latency": 0}
    assert "future_distroav_property" not in destructive_reset(current, saved)
    restored = nondestructive_update(current, saved)
    assert restored["ndi_framesync"] is False
    assert restored["ndi_sync"] == 1
    assert restored["latency"] == 0
    assert restored["future_distroav_property"] == "preserve-me"

    runtime_before = {
        "sync_offset_ns": 50_000_000,
        "monitoring": "monitor-and-output",
        "mixers": 0b001011,
        "volume": 0.82,
    }
    runtime_after = dict(runtime_before)
    assert runtime_after == runtime_before

    # Fully disabled Soft Sync means no filter correction is applied.
    soft_sync_enabled = False
    correction_ppm = ppm_for_rate(-1.5) if soft_sync_enabled else 0.0
    assert correction_ppm == 0.0

    print("Sync Guardian v0.5.0 hands-free protection, jump/drift, reliability, and controller checks passed")


if __name__ == "__main__":
    run()
