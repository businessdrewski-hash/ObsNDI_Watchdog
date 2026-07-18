"""Static v0.5 audit: every persisted control must load, save, and feed runtime state."""

from pathlib import Path


SOURCE = (Path(__file__).parents[1] / "src" / "plugin-main.cpp").read_text(encoding="utf-8")

PARAMETERS = {
    "automation_mode": "automationMode_",
    "only_when_output_active": "onlyWhenOutputActive_",
    "require_active_sources": "requireActiveSources_",
    "enable_freeze_detection": "enableFreezeDetection_",
    "enable_drift_detection": "enableDriftDetection_",
    "auto_escalate": "autoEscalate_",
    "soft_sync_enabled": "enableSoftSync_",
    "soft_sync_link_mic": "softSyncLinkMic_",
    "video_stall_ms": "videoStallMs_",
    "audio_stall_ms": "audioStallMs_",
    "drift_threshold_ms": "driftThresholdMs_",
    "drift_persistence_ms": "driftPersistenceMs_",
    "cooldown_sec": "cooldownSec_",
    "max_auto_resets_per_hour": "maxAutoResetsPerHour_",
    "startup_grace_sec": "startupGraceSec_",
    "verify_delay_sec": "verifyDelaySec_",
    "pulse_duration_ms": "pulseDurationMs_",
    "soft_sync_dead_zone_ms": "softSyncDeadZoneMs_",
    "soft_sync_max_ppm": "softSyncMaxPpm_",
    "soft_sync_slew_ppm_per_sec": "softSyncSlewPpmPerSec_",
    "chapter_markers": "chapterMarkers_",
    "json_logging": "jsonLogging_",
    "incident_reports": "incidentReports_",
    "advanced_settings_visible": "advancedToggleButton_",
    "video_source": "sourceCombos_",
    "desktop_source": "sourceCombos_",
    "mic_source": "sourceCombos_",
    "auto_tune_thresholds": "autoTuneThresholds_",
    "setup_confirmed": "setupConfirmed_",
}


def run():
    failures = []
    for key, control in PARAMETERS.items():
        key_count = SOURCE.count(f'"{key}"')
        control_count = SOURCE.count(control)
        if key_count < 2:
            failures.append(f"{key}: missing load/save path ({key_count} references)")
        if control_count < 4:
            failures.append(f"{control}: missing construction/signal/runtime path ({control_count} references)")

    assert 'obs_data_set_int(data, "config_version", 5)' in SOURCE
    assert "issue == IssueKind::PersistentDrift" in SOURCE
    assert "Never pulse or rebuild a receiver" in SOURCE
    assert "Measurement: %1" in SOURCE
    assert "kIncidentQuarantineNs" in SOURCE
    assert "softSyncValidationFailures_" in SOURCE
    assert "suggestSourceMappings" in SOURCE
    assert "runProtectionTest" in SOURCE
    assert "exportSupportReport" in SOURCE
    assert "effectiveStallThresholdMs" in SOURCE
    assert not failures, "\n".join(failures)
    print(f"Sync Guardian v0.5.0 parameter audit passed for {len(PARAMETERS)} runtime controls")


if __name__ == "__main__":
    run()
