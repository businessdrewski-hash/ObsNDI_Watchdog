// SPDX-License-Identifier: GPL-2.0-or-later
// Sync Guardian v0.5.0 - hands-free OBS companion for DistroAV/NDI sync protection.

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs.h>
#include <util/platform.h>

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QMetaObject>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStringList>
#include <QTableWidget>
#include <QTextEdit>
#include <QToolButton>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("sync-guardian", "en-US")

#ifndef PLUGIN_VERSION
#define PLUGIN_VERSION "0.5.0"
#endif

namespace {

constexpr const char *kDockId = "sync_guardian_dock";
constexpr const char *kDistroAvSourceId = "ndi_source";
constexpr const char *kVideoProbeFilterId = "sync_guardian_video_timestamp_probe";
constexpr const char *kVideoProbeFilterName = "[Sync Guardian] Video Timestamp Probe";
constexpr const char *kVideoProbeTokenKey = "sync_guardian_runtime_token";
constexpr const char *kSoftSyncFilterId = "sync_guardian_adaptive_soft_sync";
constexpr const char *kSoftSyncFilterName = "[Sync Guardian] Adaptive Soft Sync";
constexpr const char *kSoftSyncTokenKey = "sync_guardian_soft_sync_token";
constexpr const char *kPropSource = "ndi_source_name";
constexpr const char *kPropLatency = "latency";
constexpr const char *kPropFrameSync = "ndi_framesync";
constexpr const char *kPropSync = "ndi_sync";
constexpr uint64_t kNsPerMs = 1'000'000ULL;
constexpr uint64_t kNsPerSecond = 1'000'000'000ULL;
constexpr uint64_t kJumpThresholdNs = 50'000'000ULL;
constexpr uint64_t kJumpCooldownNs = 500'000'000ULL;
constexpr uint64_t kOffsetWindowNs = 10ULL * kNsPerSecond;
constexpr uint64_t kRateWindowNs = 60ULL * kNsPerSecond;
constexpr uint64_t kControllerRateWindowNs = 120ULL * kNsPerSecond;
constexpr uint64_t kBaselineWindowNs = 30ULL * kNsPerSecond;
constexpr uint64_t kDiagnosticHistoryNs = 120ULL * kNsPerSecond;
constexpr uint64_t kIncidentPreNs = 30ULL * kNsPerSecond;
constexpr uint64_t kIncidentPostNs = 30ULL * kNsPerSecond;
constexpr uint64_t kIncidentCooldownNs = 5ULL * 60ULL * kNsPerSecond;
constexpr uint64_t kResetLimitWindowNs = 60ULL * 60ULL * kNsPerSecond;
constexpr uint64_t kObserveRepeatNs = 30ULL * kNsPerSecond;
constexpr uint64_t kJumpEvidenceWindowNs = 5ULL * kNsPerSecond;
constexpr uint64_t kIncidentQuarantineNs = 8ULL * kNsPerSecond;
constexpr uint64_t kStableCalibrationMinimumNs = 30ULL * kNsPerSecond;
constexpr uint64_t kReliableMinimumSpanNs = 10ULL * kNsPerSecond;
constexpr uint64_t kStallConfirmNs = 500ULL * kNsPerMs;
constexpr uint64_t kFirstSampleGraceNs = 2ULL * kNsPerSecond;
constexpr uint64_t kIssueEpisodeMergeNs = 60ULL * kNsPerSecond;
constexpr int kWatchdogSampleIntervalMs = 250;
constexpr int kUiRefreshIntervalMs = 1000;
constexpr double kPpmPerMsPerMinute = 16.6666666667;

static int64_t signedDelta(uint64_t newer, uint64_t older)
{
	return static_cast<int64_t>(newer) - static_cast<int64_t>(older);
}

static double nsToMs(int64_t value)
{
	return static_cast<double>(value) / static_cast<double>(kNsPerMs);
}

static double median(std::vector<double> values)
{
	if (values.empty())
		return std::numeric_limits<double>::quiet_NaN();
	const size_t middle = values.size() / 2;
	std::nth_element(values.begin(), values.begin() + static_cast<ptrdiff_t>(middle), values.end());
	double result = values[middle];
	if ((values.size() % 2) == 0) {
		const auto lower = std::max_element(values.begin(), values.begin() + static_cast<ptrdiff_t>(middle));
		result = (*lower + result) * 0.5;
	}
	return result;
}

static void updateAtomicMaximum(std::atomic<uint64_t> &value, uint64_t candidate)
{
	uint64_t current = value.load();
	while (candidate > current && !value.compare_exchange_weak(current, candidate)) {
	}
}

static QString driftDirectionShort(double rateMsPerMinute)
{
	if (!std::isfinite(rateMsPerMinute))
		return QStringLiteral("Trend settling");
	if (std::fabs(rateMsPerMinute) < 0.25)
		return QStringLiteral("Stable");
	return rateMsPerMinute < 0.0 ? QStringLiteral("Video dragging") : QStringLiteral("Video rushing");
}

static QString latencyName(long long value)
{
	switch (value) {
	case 0:
		return QStringLiteral("Normal");
	case 1:
		return QStringLiteral("Low");
	case 2:
		return QStringLiteral("Lowest");
	default:
		return QStringLiteral("Unknown (%1)").arg(value);
	}
}

static QString syncModeName(long long value)
{
	switch (value) {
	case 1:
		return QStringLiteral("NDI Timestamp");
	case 2:
		return QStringLiteral("NDI Source Timecode");
	default:
		return QStringLiteral("Unknown (%1)").arg(value);
	}
}

static void makeDistroAvSettingsExplicit(obs_data_t *settings)
{
	if (!settings)
		return;
	const bool frameSync = obs_data_get_bool(settings, kPropFrameSync);
	const long long latency = obs_data_get_int(settings, kPropLatency);
	const long long syncMode = obs_data_get_int(settings, kPropSync);
	const QString ndiTarget = QString::fromUtf8(obs_data_get_string(settings, kPropSource));
	obs_data_set_bool(settings, kPropFrameSync, frameSync);
	obs_data_set_int(settings, kPropLatency, latency);
	obs_data_set_int(settings, kPropSync, syncMode);
	obs_data_set_string(settings, kPropSource, ndiTarget.toUtf8().constData());
}

enum class AutomationMode : int {
	Observe = 0,
	Ask = 1,
	Automatic = 2,
};

enum class RecoveryTarget : int {
	None = 0,
	Video,
	DesktopAudio,
	Mic,
	BothAudio,
	EntireGroup,
};

enum class IssueKind : int {
	None = 0,
	EntireGroupStall,
	BothAudioStall,
	VideoStall,
	DesktopAudioStall,
	MicStall,
	VideoJump,
	DesktopAudioJump,
	MicJump,
	PersistentDrift,
};

enum class MeasurementReliability : int {
	Waiting = 0,
	Calibrating,
	Reliable,
	Unreliable,
};

enum class MonitorState : int {
	Stable = 0,
	Drifting,
	JumpSuspected,
	Recovering,
	Verifying,
	Failed,
};

static QString reliabilityName(MeasurementReliability reliability)
{
	switch (reliability) {
	case MeasurementReliability::Calibrating:
		return QStringLiteral("Calibrating");
	case MeasurementReliability::Reliable:
		return QStringLiteral("Reliable");
	case MeasurementReliability::Unreliable:
		return QStringLiteral("Unreliable");
	default:
		return QStringLiteral("Waiting");
	}
}

static QString targetName(RecoveryTarget target)
{
	switch (target) {
	case RecoveryTarget::Video:
		return QStringLiteral("NDI Video");
	case RecoveryTarget::DesktopAudio:
		return QStringLiteral("NDI Desktop Audio");
	case RecoveryTarget::Mic:
		return QStringLiteral("NDI Mic");
	case RecoveryTarget::BothAudio:
		return QStringLiteral("Both NDI audio sources");
	case RecoveryTarget::EntireGroup:
		return QStringLiteral("Entire NDI sync group");
	default:
		return QStringLiteral("None");
	}
}

static QString modeName(AutomationMode mode)
{
	switch (mode) {
	case AutomationMode::Ask:
		return QStringLiteral("Ask before reset");
	case AutomationMode::Automatic:
		return QStringLiteral("Fully automatic");
	default:
		return QStringLiteral("Observe only");
	}
}

static QString issueSummaryName(IssueKind issue)
{
	switch (issue) {
	case IssueKind::EntireGroupStall:
		return QStringLiteral("all mapped NDI streams stalled");
	case IssueKind::BothAudioStall:
		return QStringLiteral("both NDI audio streams stalled");
	case IssueKind::VideoStall:
		return QStringLiteral("NDI video stalled");
	case IssueKind::DesktopAudioStall:
		return QStringLiteral("NDI desktop audio stalled");
	case IssueKind::MicStall:
		return QStringLiteral("NDI microphone stalled");
	case IssueKind::VideoJump:
		return QStringLiteral("NDI video timestamp jumped");
	case IssueKind::DesktopAudioJump:
		return QStringLiteral("NDI desktop-audio timestamp jumped");
	case IssueKind::MicJump:
		return QStringLiteral("NDI microphone timestamp jumped");
	case IssueKind::PersistentDrift:
		return QStringLiteral("persistent A/V timestamp drift detected");
	default:
		return QStringLiteral("no active sync issue");
	}
}

struct OffsetSample {
	uint64_t wallNs = 0;
	double valueMs = std::numeric_limits<double>::quiet_NaN();
};

struct DiagnosticSample {
	uint64_t wallNs = 0;
	QString time;
	double rawOffsetMs = std::numeric_limits<double>::quiet_NaN();
	double filteredOffsetMs = std::numeric_limits<double>::quiet_NaN();
	double driftMs = std::numeric_limits<double>::quiet_NaN();
	double correctedDriftMs = std::numeric_limits<double>::quiet_NaN();
	double driftRateMsPerMinute = std::numeric_limits<double>::quiet_NaN();
	double videoAgeMs = std::numeric_limits<double>::quiet_NaN();
	double desktopAgeMs = std::numeric_limits<double>::quiet_NaN();
	double micAgeMs = std::numeric_limits<double>::quiet_NaN();
	int confidence = 0;
};

static double robustSlopeMsPerMinute(const std::deque<OffsetSample> &samples, uint64_t now, uint64_t windowNs)
{
	std::vector<const OffsetSample *> selected;
	selected.reserve(samples.size());
	for (const auto &sample : samples) {
		if (now >= sample.wallNs && now - sample.wallNs <= windowNs && std::isfinite(sample.valueMs))
			selected.push_back(&sample);
	}
	if (selected.size() < 12)
		return std::numeric_limits<double>::quiet_NaN();
	const uint64_t span = selected.back()->wallNs - selected.front()->wallNs;
	if (span < std::min<uint64_t>(windowNs / 2, 30ULL * kNsPerSecond))
		return std::numeric_limits<double>::quiet_NaN();

	std::vector<double> slopes;
	const size_t stride = std::max<size_t>(1, selected.size() / 24);
	for (size_t i = 0; i < selected.size(); i += stride) {
		for (size_t j = i + stride; j < selected.size(); j += stride) {
			const uint64_t dtNs = selected[j]->wallNs - selected[i]->wallNs;
			if (dtNs < 15ULL * kNsPerSecond)
				continue;
			const double minutes = static_cast<double>(dtNs) / (60.0 * static_cast<double>(kNsPerSecond));
			slopes.push_back((selected[j]->valueMs - selected[i]->valueMs) / minutes);
		}
	}
	return median(std::move(slopes));
}

struct SoftSyncControl {
	std::atomic_bool enabled{false};
	std::atomic<double> correctionPpm{0.0};
	std::atomic<double> targetPpm{0.0};
	std::atomic<uint64_t> generation{1};
	std::atomic<uint64_t> processedBlocks{0};
	std::atomic<int64_t> netFrameAdjustment{0};
	std::atomic<uint32_t> sampleRate{48000};
	// Raw, pre-filter timing. The source audio capture callback may observe
	// post-filter timestamps, so the watchdog uses these values whenever Soft
	// Sync is attached to avoid counting the correction twice.
	std::atomic<uint64_t> lastInputTimestampNs{0};
	std::atomic<uint64_t> lastInputWallNs{0};
	std::atomic<uint64_t> lastOutputTimestampNs{0};
	std::atomic<uint64_t> lastOutputWallNs{0};
};

std::mutex g_softSyncRegistryMutex;
std::unordered_map<uint64_t, SoftSyncControl *> g_softSyncRegistry;
std::atomic<uint64_t> g_nextSoftSyncToken{1};

struct SoftSyncFilterData {
	SoftSyncControl *control = nullptr;
	std::array<std::vector<float>, MAX_AV_PLANES> planes;
	obs_audio_data output{};
	double frameRemainder = 0.0;
	uint32_t sampleRate = 48000;
	uint64_t expectedInputTimestampNs = 0;
	uint64_t nextOutputTimestampNs = 0;
	uint64_t inputTimestampRemainder = 0;
	uint64_t outputTimestampRemainder = 0;
	uint64_t observedGeneration = 0;
	bool timelineInitialized = false;
};

static const char *softSyncFilterDisplayName(void *)
{
	return "Sync Guardian Adaptive Soft Sync";
}

static uint64_t framesToDurationNs(uint32_t frames, uint32_t sampleRate, uint64_t &remainder)
{
	if (!sampleRate)
		return 0;
	const uint64_t numerator = static_cast<uint64_t>(frames) * kNsPerSecond + remainder;
	const uint64_t duration = numerator / sampleRate;
	remainder = numerator % sampleRate;
	return duration;
}

static void resetSoftSyncTimeline(SoftSyncFilterData *filter, const obs_audio_data *audio)
{
	filter->frameRemainder = 0.0;
	filter->inputTimestampRemainder = 0;
	filter->outputTimestampRemainder = 0;
	filter->expectedInputTimestampNs = audio ? audio->timestamp : 0;
	filter->nextOutputTimestampNs = audio ? audio->timestamp : 0;
	filter->timelineInitialized = audio && audio->timestamp;
	if (filter->control) {
		filter->control->netFrameAdjustment.store(0);
		filter->control->lastOutputTimestampNs.store(audio ? audio->timestamp : 0);
		filter->control->lastOutputWallNs.store(audio ? os_gettime_ns() : 0);
	}
}

static void *softSyncFilterCreate(obs_data_t *settings, obs_source_t *)
{
	auto *filter = new SoftSyncFilterData();
	const uint64_t token = static_cast<uint64_t>(obs_data_get_int(settings, kSoftSyncTokenKey));
	{
		std::lock_guard<std::mutex> lock(g_softSyncRegistryMutex);
		const auto it = g_softSyncRegistry.find(token);
		if (it != g_softSyncRegistry.end())
			filter->control = it->second;
	}
	obs_audio_info info{};
	if (obs_get_audio_info(&info) && info.samples_per_sec)
		filter->sampleRate = info.samples_per_sec;
	if (filter->control)
		filter->control->sampleRate.store(filter->sampleRate);
	return filter;
}

static void softSyncFilterDestroy(void *data)
{
	delete static_cast<SoftSyncFilterData *>(data);
}

static obs_audio_data *softSyncFilterAudio(void *data, obs_audio_data *audio)
{
	auto *filter = static_cast<SoftSyncFilterData *>(data);
	if (!filter || !filter->control || !audio || !audio->frames)
		return audio;

	const uint64_t callbackWallNs = os_gettime_ns();
	filter->control->lastInputTimestampNs.store(audio->timestamp);
	filter->control->lastInputWallNs.store(callbackWallNs);

	const uint64_t generation = filter->control->generation.load();
	if (generation != filter->observedGeneration) {
		filter->observedGeneration = generation;
		resetSoftSyncTimeline(filter, audio);
	}
	if (!filter->control->enabled.load())
		return audio;

	const double ppm = std::clamp(filter->control->correctionPpm.load(), -5000.0, 5000.0);
	if (std::fabs(ppm) < 0.001) {
		// Preserve the already accumulated timing trim while running at neutral
		// speed. Returning the original input timestamp here would abruptly erase
		// the trim and create a catch-up jump.
		if (!filter->timelineInitialized)
			resetSoftSyncTimeline(filter, audio);
		filter->output = *audio;
		filter->output.timestamp = filter->nextOutputTimestampNs ? filter->nextOutputTimestampNs : audio->timestamp;
		if (audio->timestamp)
			filter->expectedInputTimestampNs = audio->timestamp +
				framesToDurationNs(audio->frames, filter->sampleRate, filter->inputTimestampRemainder);
		if (filter->output.timestamp)
			filter->nextOutputTimestampNs = filter->output.timestamp +
				framesToDurationNs(audio->frames, filter->sampleRate, filter->outputTimestampRemainder);
		filter->control->processedBlocks.fetch_add(1);
		filter->control->lastOutputTimestampNs.store(filter->output.timestamp);
		filter->control->lastOutputWallNs.store(callbackWallNs);
		return &filter->output;
	}

	// Positive ppm produces slightly more output samples, gently slowing the audio
	// timeline. Negative ppm produces slightly fewer samples, gently speeding it up.
	const double stretch = 1.0 + ppm * 1.0e-6;
	const double desiredFrames = static_cast<double>(audio->frames) * stretch + filter->frameRemainder;
	const uint32_t outputFrames = static_cast<uint32_t>(std::max(1.0, std::floor(desiredFrames)));
	filter->frameRemainder = desiredFrames - static_cast<double>(outputFrames);

	if (!filter->timelineInitialized) {
		resetSoftSyncTimeline(filter, audio);
	} else if (audio->timestamp && filter->expectedInputTimestampNs) {
		const int64_t inputError = signedDelta(audio->timestamp, filter->expectedInputTimestampNs);
		if (std::llabs(inputError) > static_cast<int64_t>(kJumpThresholdNs))
			resetSoftSyncTimeline(filter, audio);
	}

	filter->output = *audio;
	filter->output.frames = outputFrames;
	filter->output.timestamp = filter->nextOutputTimestampNs ? filter->nextOutputTimestampNs : audio->timestamp;

	const uint32_t inputFrames = audio->frames;
	for (size_t channel = 0; channel < MAX_AV_PLANES; ++channel) {
		if (!audio->data[channel]) {
			filter->output.data[channel] = nullptr;
			continue;
		}

		const float *input = reinterpret_cast<const float *>(audio->data[channel]);
		auto &output = filter->planes[channel];
		output.resize(outputFrames);
		if (outputFrames == 1 || inputFrames == 1) {
			output[0] = input[0];
		} else {
			const double scale = static_cast<double>(inputFrames - 1) / static_cast<double>(outputFrames - 1);
			for (uint32_t frame = 0; frame < outputFrames; ++frame) {
				const double position = static_cast<double>(frame) * scale;
				const uint32_t left = static_cast<uint32_t>(position);
				const uint32_t right = std::min(left + 1, inputFrames - 1);
				const float fraction = static_cast<float>(position - static_cast<double>(left));
				output[frame] = input[left] + (input[right] - input[left]) * fraction;
			}
		}
		filter->output.data[channel] = reinterpret_cast<uint8_t *>(output.data());
	}

	if (audio->timestamp) {
		filter->expectedInputTimestampNs = audio->timestamp +
			framesToDurationNs(inputFrames, filter->sampleRate, filter->inputTimestampRemainder);
	}
	if (filter->output.timestamp) {
		filter->nextOutputTimestampNs = filter->output.timestamp +
			framesToDurationNs(outputFrames, filter->sampleRate, filter->outputTimestampRemainder);
	}

	filter->control->processedBlocks.fetch_add(1);
	filter->control->netFrameAdjustment.fetch_add(static_cast<int64_t>(outputFrames) -
						static_cast<int64_t>(inputFrames));
	filter->control->lastOutputTimestampNs.store(filter->output.timestamp);
	filter->control->lastOutputWallNs.store(callbackWallNs);
	return &filter->output;
}

static void registerSoftSyncFilter()
{
	obs_source_info info = {};
	info.id = kSoftSyncFilterId;
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_AUDIO;
	info.get_name = softSyncFilterDisplayName;
	info.create = softSyncFilterCreate;
	info.destroy = softSyncFilterDestroy;
	info.filter_audio = softSyncFilterAudio;
	obs_register_source(&info);
}

// DistroAV's property object is only one part of an OBS source's state.  Audio
// routing, sync offset, monitoring, mute/volume, source flags and deinterlacing
// live on obs_source_t itself.  A reset pulse must preserve both layers.
struct SourceRuntimeSnapshot {
	bool captured = false;
	bool enabled = true;
	bool hasAudio = false;
	bool hasVideo = false;
	bool muted = false;
	float volume = 1.0f;
	float balance = 0.5f;
	int64_t syncOffsetNs = 0;
	uint32_t audioMixers = 0;
	enum obs_monitoring_type monitoringType = OBS_MONITORING_TYPE_NONE;
	bool pushToMuteEnabled = false;
	uint64_t pushToMuteDelay = 0;
	bool pushToTalkEnabled = false;
	uint64_t pushToTalkDelay = 0;
	uint32_t sourceFlags = 0;
	enum obs_deinterlace_mode deinterlaceMode = OBS_DEINTERLACE_MODE_DISABLE;
	enum obs_deinterlace_field_order deinterlaceFieldOrder = OBS_DEINTERLACE_FIELD_ORDER_TOP;

	void capture(obs_source_t *source)
	{
		if (!source)
			return;
		const uint32_t outputFlags = obs_source_get_output_flags(source);
		hasAudio = (outputFlags & OBS_SOURCE_AUDIO) != 0;
		hasVideo = (outputFlags & OBS_SOURCE_VIDEO) != 0;
		enabled = obs_source_enabled(source);
		sourceFlags = obs_source_get_flags(source);
		if (hasAudio) {
			muted = obs_source_muted(source);
			volume = obs_source_get_volume(source);
			balance = obs_source_get_balance_value(source);
			syncOffsetNs = obs_source_get_sync_offset(source);
			audioMixers = obs_source_get_audio_mixers(source);
			monitoringType = obs_source_get_monitoring_type(source);
			pushToMuteEnabled = obs_source_push_to_mute_enabled(source);
			pushToMuteDelay = obs_source_get_push_to_mute_delay(source);
			pushToTalkEnabled = obs_source_push_to_talk_enabled(source);
			pushToTalkDelay = obs_source_get_push_to_talk_delay(source);
		}
		if (hasVideo) {
			deinterlaceMode = obs_source_get_deinterlace_mode(source);
			deinterlaceFieldOrder = obs_source_get_deinterlace_field_order(source);
		}
		captured = true;
	}

	void restore(obs_source_t *source) const
	{
		if (!captured || !source)
			return;
		obs_source_set_enabled(source, enabled);
		obs_source_set_flags(source, sourceFlags);
		if (hasAudio) {
			obs_source_set_muted(source, muted);
			obs_source_set_volume(source, volume);
			obs_source_set_balance_value(source, balance);
			obs_source_set_sync_offset(source, syncOffsetNs);
			obs_source_set_audio_mixers(source, audioMixers);
			obs_source_set_monitoring_type(source, monitoringType);
			obs_source_enable_push_to_mute(source, pushToMuteEnabled);
			obs_source_set_push_to_mute_delay(source, pushToMuteDelay);
			obs_source_enable_push_to_talk(source, pushToTalkEnabled);
			obs_source_set_push_to_talk_delay(source, pushToTalkDelay);
		}
		if (hasVideo) {
			obs_source_set_deinterlace_mode(source, deinterlaceMode);
			obs_source_set_deinterlace_field_order(source, deinterlaceFieldOrder);
		}
	}

	bool matches(obs_source_t *source) const
	{
		if (!captured || !source)
			return false;
		if (obs_source_enabled(source) != enabled || obs_source_get_flags(source) != sourceFlags)
			return false;
		if (hasAudio) {
			if (obs_source_muted(source) != muted ||
			    std::fabs(obs_source_get_volume(source) - volume) > 0.0001f ||
			    std::fabs(obs_source_get_balance_value(source) - balance) > 0.0001f ||
			    obs_source_get_sync_offset(source) != syncOffsetNs ||
			    obs_source_get_audio_mixers(source) != audioMixers ||
			    obs_source_get_monitoring_type(source) != monitoringType ||
			    obs_source_push_to_mute_enabled(source) != pushToMuteEnabled ||
			    obs_source_get_push_to_mute_delay(source) != pushToMuteDelay ||
			    obs_source_push_to_talk_enabled(source) != pushToTalkEnabled ||
			    obs_source_get_push_to_talk_delay(source) != pushToTalkDelay)
				return false;
		}
		if (hasVideo &&
		    (obs_source_get_deinterlace_mode(source) != deinterlaceMode ||
		     obs_source_get_deinterlace_field_order(source) != deinterlaceFieldOrder))
			return false;
		return true;
	}
};

struct SourceState {
	QString role;
	QString sourceName;
	obs_weak_source_t *weak = nullptr;
	obs_data_t *snapshot = nullptr;
	SourceRuntimeSnapshot snapshotRuntime;
	bool monitorAudio = false;

	std::atomic<uint64_t> lastAudioTimestampNs{0};
	std::atomic<uint64_t> lastAudioWallNs{0};
	std::atomic<uint64_t> lastAudioJumpWallNs{0};
	std::atomic<int64_t> lastAudioJumpErrorNs{0};
	std::atomic<uint64_t> audioJumpCount{0};

	std::atomic<uint64_t> lastVideoTimestampNs{0};
	std::atomic<uint64_t> lastVideoWallNs{0};
	std::atomic<uint64_t> lastVideoArrivalWallNs{0};
	std::atomic<uint64_t> lastVideoJumpWallNs{0};
	std::atomic<int64_t> lastVideoJumpErrorNs{0};
	std::atomic<uint64_t> videoJumpCount{0};
	std::atomic<uint64_t> firstValidSampleWallNs{0};
	std::atomic<uint64_t> observedHealthyGapNs{0};
	obs_weak_source_t *videoProbeWeak = nullptr;
	uint64_t videoProbeToken = 0;
	obs_weak_source_t *softSyncFilterWeak = nullptr;
	uint64_t softSyncToken = 0;
	SoftSyncControl softSync;

	std::atomic<uint64_t> resetCount{0};
	std::atomic<uint64_t> recoveryCount{0};
	std::shared_ptr<std::atomic_bool> resetInProgress = std::make_shared<std::atomic_bool>(false);
	std::atomic_bool wasFresh{false};
	std::atomic_bool hasEverBeenFresh{false};
	std::atomic_bool enabled{false};
	std::atomic_bool active{false};
	std::atomic_bool showing{false};
};

std::mutex g_videoProbeRegistryMutex;
std::unordered_map<uint64_t, SourceState *> g_videoProbeRegistry;
std::atomic<uint64_t> g_nextVideoProbeToken{1};

struct VideoProbeData {
	SourceState *state = nullptr;
};

static const char *videoProbeName(void *)
{
	return "Sync Guardian Video Timestamp Probe";
}

static void *videoProbeCreate(obs_data_t *settings, obs_source_t *)
{
	auto *probe = new VideoProbeData();
	const uint64_t token = static_cast<uint64_t>(obs_data_get_int(settings, kVideoProbeTokenKey));
	std::lock_guard<std::mutex> lock(g_videoProbeRegistryMutex);
	const auto it = g_videoProbeRegistry.find(token);
	if (it != g_videoProbeRegistry.end())
		probe->state = it->second;
	return probe;
}

static void videoProbeDestroy(void *data)
{
	delete static_cast<VideoProbeData *>(data);
}

static obs_source_frame *videoProbeFilter(void *data, obs_source_frame *frame)
{
	auto *probe = static_cast<VideoProbeData *>(data);
	if (!probe || !probe->state || !frame)
		return frame;

	SourceState *state = probe->state;
	const uint64_t now = os_gettime_ns();
	state->lastVideoArrivalWallNs.store(now);

	const uint64_t timestamp = frame->timestamp;
	if (!timestamp)
		return frame;

	uint64_t expectedFirst = 0;
	state->firstValidSampleWallNs.compare_exchange_strong(expectedFirst, now);

	const uint64_t previousTimestamp = state->lastVideoTimestampNs.load();
	if (timestamp == previousTimestamp)
		return frame;

	const uint64_t previousWall = state->lastVideoWallNs.load();
	state->lastVideoTimestampNs.store(timestamp);
	state->lastVideoWallNs.store(now);

	if (!previousTimestamp || !previousWall)
		return frame;

	const int64_t timestampDelta = signedDelta(timestamp, previousTimestamp);
	const int64_t wallDelta = signedDelta(now, previousWall);
	const int64_t error = timestampDelta - wallDelta;
	if (std::llabs(error) < static_cast<int64_t>(kJumpThresholdNs)) {
		if (wallDelta > 0 && wallDelta <= static_cast<int64_t>(500ULL * kNsPerMs))
			updateAtomicMaximum(state->observedHealthyGapNs, static_cast<uint64_t>(wallDelta));
		return frame;
	}

	const uint64_t previousJump = state->lastVideoJumpWallNs.load();
	if (previousJump && now - previousJump < kJumpCooldownNs)
		return frame;

	state->lastVideoJumpWallNs.store(now);
	state->lastVideoJumpErrorNs.store(error);
	state->videoJumpCount.fetch_add(1);
	return frame;
}

static void registerVideoProbeFilter()
{
	obs_source_info info = {};
	info.id = kVideoProbeFilterId;
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_ASYNC_VIDEO;
	info.get_name = videoProbeName;
	info.create = videoProbeCreate;
	info.destroy = videoProbeDestroy;
	info.filter_video = videoProbeFilter;
	obs_register_source(&info);
}

struct ResetTicket {
	obs_weak_source_t *weak = nullptr;
	obs_data_t *original = nullptr;
	std::shared_ptr<std::atomic_bool> resetFlag;
	uint64_t restoreAtNs = 0;
	bool expectedFrameSync = false;
	long long expectedLatency = 0;
	long long expectedSyncMode = 0;
	QString expectedNdiTarget;
	QString role;
	SourceRuntimeSnapshot runtime;
	std::atomic_bool restored{false};

	bool knownSettingsMatch(obs_data_t *settings) const
	{
		if (!settings)
			return false;
		return obs_data_get_bool(settings, kPropFrameSync) == expectedFrameSync &&
		       obs_data_get_int(settings, kPropLatency) == expectedLatency &&
		       obs_data_get_int(settings, kPropSync) == expectedSyncMode &&
		       QString::fromUtf8(obs_data_get_string(settings, kPropSource)) == expectedNdiTarget;
	}

	bool restore()
	{
		bool expected = false;
		if (!restored.compare_exchange_strong(expected, true))
			return true;
		bool settingsMatch = false;
		if (weak && original) {
			obs_source_t *source = obs_weak_source_get_source(weak);
			if (source) {
				// Apply the saved object non-destructively. obs_source_reset_settings()
				// clears keys that are absent from the snapshot, which can wipe DistroAV
				// properties that were left at defaults or added by a newer plugin build.
				// obs_source_update() restores the captured values while leaving every
				// unrelated/unknown property intact.
				obs_source_update(source, original);
				runtime.restore(source);
				obs_data_t *verify = obs_source_get_settings(source);
				settingsMatch = knownSettingsMatch(verify) && runtime.matches(source);
				obs_data_release(verify);
				if (!settingsMatch) {
					// Retry with every critical DistroAV value explicit, then restore OBS's
					// source-level audio routing and presentation properties again.
					obs_data_set_bool(original, kPropFrameSync, expectedFrameSync);
					obs_data_set_int(original, kPropLatency, expectedLatency);
					obs_data_set_int(original, kPropSync, expectedSyncMode);
					obs_data_set_string(original, kPropSource, expectedNdiTarget.toUtf8().constData());
					obs_source_update(source, original);
					runtime.restore(source);
					verify = obs_source_get_settings(source);
					settingsMatch = knownSettingsMatch(verify) && runtime.matches(source);
					obs_data_release(verify);
				}
				obs_source_release(source);
			}
		}
		if (resetFlag)
			resetFlag->store(false);
		return settingsMatch;
	}

	~ResetTicket()
	{
		restore();
		if (original)
			obs_data_release(original);
		if (weak)
			obs_weak_source_release(weak);
	}
};

struct RecoveryAttempt {
	bool active = false;
	bool automated = false;
	bool escalationUsed = false;
	RecoveryTarget target = RecoveryTarget::None;
	IssueKind issue = IssueKind::None;
	QString reason;
	uint64_t verifyAtNs = 0;
	double preDriftMs = std::numeric_limits<double>::quiet_NaN();
};

struct IncidentCapture {
	bool active = false;
	QString path;
	uint64_t finalizeAtNs = 0;
	QJsonObject root;
	QJsonArray samples;
};

struct EngineSettings {
	std::atomic<int> automationMode{static_cast<int>(AutomationMode::Observe)};
	std::atomic_bool onlyWhenOutputActive{true};
	std::atomic_bool requireActiveSources{true};
	std::atomic_bool enableFreezeDetection{true};
	std::atomic_bool enableDriftDetection{true};
	std::atomic_bool autoEscalate{true};
	std::atomic_bool autoTuneThresholds{true};
	std::atomic<int> videoStallMs{1000};
	std::atomic<int> audioStallMs{1000};
	std::atomic<int> driftThresholdMs{200};
	std::atomic<int> driftPersistenceMs{10000};
	std::atomic<int> cooldownSec{180};
	std::atomic<int> maxAutoResetsPerHour{3};
	std::atomic<int> startupGraceSec{30};
	std::atomic<int> verifyDelaySec{5};
	std::atomic<int> pulseDurationMs{180};
	std::atomic_bool softSyncEnabled{false};
	std::atomic_bool softSyncLinkMic{false};
	std::atomic<int> softSyncDeadZoneMs{12};
	std::atomic<int> softSyncMaxPpm{50};
	std::atomic<int> softSyncSlewPpmPerSec{1};
};

class SyncGuardian {
public:
	SyncGuardian()
	{
		lifetimeContext_ = new QObject();
		states_[0].role = QStringLiteral("NDI Video");
		states_[0].monitorAudio = false;
		states_[1].role = QStringLiteral("Desktop Audio");
		states_[1].monitorAudio = true;
		states_[2].role = QStringLiteral("Mic");
		states_[2].monitorAudio = true;

		buildUi();
		validateParameterBindings();
		refreshSourceLists();
		loadConfiguration();
		bindAllSources();
		registerHotkeys();
		updateEngineSettings();
		syncSoftSyncFilters();

		const uint64_t now = os_gettime_ns();
		monitoringGraceUntilNs_ = now + static_cast<uint64_t>(startupGraceSec_->value()) * kNsPerSecond;
		monitoringGraceUntilAtomicNs_.store(monitoringGraceUntilNs_);

		refreshTimer_ = new QTimer(lifetimeContext_);
		refreshTimer_->setInterval(kUiRefreshIntervalMs);
		QObject::connect(refreshTimer_, &QTimer::timeout, [this]() { refreshDiagnostics(); });
		refreshTimer_->start();

		stopWatchdog_.store(false);
		watchdogThread_ = std::thread([this]() { watchdogLoop(); });

		obs_frontend_add_event_callback(frontendEvent, this);
		appendEvent(QStringLiteral("Sync Guardian %1 loaded in %2 mode")
				    .arg(QStringLiteral(PLUGIN_VERSION), modeName(currentMode())),
			    false);
	}

	~SyncGuardian()
	{
		obs_frontend_remove_event_callback(frontendEvent, this);
		unregisterHotkeys();

		snapshotRebuildAtNs_.store(0);
		stopWatchdog_.store(true);
		watchdogCv_.notify_all();
		if (watchdogThread_.joinable())
			watchdogThread_.join();

		{
			std::lock_guard<std::mutex> lock(resetMutex_);
			for (const auto &ticket : pendingResets_)
				ticket->restore();
			pendingResets_.clear();
		}
		finalizeIncident(true);

		delete lifetimeContext_;
		lifetimeContext_ = nullptr;

		for (auto &state : states_) {
			detachSource(state);
			if (state.snapshot) {
				obs_data_release(state.snapshot);
				state.snapshot = nullptr;
			}
		}
		obs_frontend_remove_dock(kDockId);
	}

private:
	QObject *lifetimeContext_ = nullptr;
	QWidget *panel_ = nullptr;
	std::array<QComboBox *, 3> sourceCombos_{};
	QSpinBox *pulseDurationMs_ = nullptr;
	QCheckBox *chapterMarkers_ = nullptr;
	QCheckBox *jsonLogging_ = nullptr;
	QCheckBox *incidentReports_ = nullptr;
	QCheckBox *enableSoftSync_ = nullptr;
	QCheckBox *softSyncLinkMic_ = nullptr;
	QSpinBox *softSyncDeadZoneMs_ = nullptr;
	QSpinBox *softSyncMaxPpm_ = nullptr;
	QSpinBox *softSyncSlewPpmPerSec_ = nullptr;

	QComboBox *automationMode_ = nullptr;
	QCheckBox *automaticProtection_ = nullptr;
	QCheckBox *autoTuneThresholds_ = nullptr;
	QLabel *readinessLabel_ = nullptr;
	QLabel *notificationLabel_ = nullptr;
	QLabel *effectiveThresholdsLabel_ = nullptr;
	QPushButton *confirmSetupButton_ = nullptr;
	QPushButton *protectionTestButton_ = nullptr;
	QPushButton *exportReportButton_ = nullptr;
	QPushButton *advancedToggleButton_ = nullptr;
	QWidget *advancedSettingsWidget_ = nullptr;
	QCheckBox *onlyWhenOutputActive_ = nullptr;
	QCheckBox *requireActiveSources_ = nullptr;
	QCheckBox *enableFreezeDetection_ = nullptr;
	QCheckBox *enableDriftDetection_ = nullptr;
	QCheckBox *autoEscalate_ = nullptr;
	QSpinBox *videoStallMs_ = nullptr;
	QSpinBox *audioStallMs_ = nullptr;
	QSpinBox *driftThresholdMs_ = nullptr;
	QSpinBox *driftPersistenceMs_ = nullptr;
	QSpinBox *cooldownSec_ = nullptr;
	QSpinBox *maxAutoResetsPerHour_ = nullptr;
	QSpinBox *startupGraceSec_ = nullptr;
	QSpinBox *verifyDelaySec_ = nullptr;
	QPushButton *resetVideoButton_ = nullptr;
	QPushButton *resetDesktopButton_ = nullptr;
	QPushButton *resetMicButton_ = nullptr;
	QPushButton *resetBothAudioButton_ = nullptr;
	QPushButton *rebuildGroupButton_ = nullptr;
	QLabel *manualSuggestionLabel_ = nullptr;
	QLabel *compactStatusLabel_ = nullptr;
	QLabel *compactOffsetLabel_ = nullptr;
	QPushButton *fixNowButton_ = nullptr;

	QLabel *automationStatusLabel_ = nullptr;
	QLabel *overallSummaryLabel_ = nullptr;
	QLabel *healthLabel_ = nullptr;
	QLabel *avOffsetLabel_ = nullptr;
	QLabel *driftLabel_ = nullptr;
	QLabel *driftTrendLabel_ = nullptr;
	QWidget *diagnosticDetailsWidget_ = nullptr;
	QToolButton *diagnosticDetailsToggle_ = nullptr;
	QLabel *micOffsetLabel_ = nullptr;
	QLabel *obsStatsLabel_ = nullptr;
	QLabel *softSyncStatusLabel_ = nullptr;
	QTableWidget *statusTable_ = nullptr;
	QTextEdit *eventLog_ = nullptr;
	QTimer *refreshTimer_ = nullptr;

	EngineSettings engineSettings_{};
	std::thread watchdogThread_;
	std::atomic_bool stopWatchdog_{false};
	std::mutex watchdogWaitMutex_;
	std::condition_variable watchdogCv_;
	mutable std::mutex sourceMutex_;
	std::mutex resetMutex_;
	std::mutex measurementMutex_;
	std::mutex backgroundEventMutex_;
	std::deque<QString> backgroundEvents_;

	std::array<SourceState, 3> states_{};
	std::vector<std::shared_ptr<ResetTicket>> pendingResets_;
	std::deque<OffsetSample> offsetSamples_;
	std::deque<DiagnosticSample> diagnosticHistory_;
	std::deque<uint64_t> automatedResetTimes_;
	RecoveryAttempt recovery_;
	IncidentCapture incident_;

	std::atomic<double> rawOffsetMs_{std::numeric_limits<double>::quiet_NaN()};
	std::atomic<double> baselineOffsetMs_{std::numeric_limits<double>::quiet_NaN()};
	std::atomic<double> filteredOffsetMs_{std::numeric_limits<double>::quiet_NaN()};
	std::atomic<double> driftRateMsPerMinute_{std::numeric_limits<double>::quiet_NaN()};
	std::atomic<double> controllerRateMsPerMinute_{std::numeric_limits<double>::quiet_NaN()};
	std::atomic<double> softSyncEstimatedDriftMs_{std::numeric_limits<double>::quiet_NaN()};
	std::atomic<int> measurementReliability_{static_cast<int>(MeasurementReliability::Waiting)};
	std::atomic<int> monitorState_{static_cast<int>(MonitorState::Stable)};
	std::atomic<uint64_t> quarantineUntilNs_{0};
	std::atomic<uint64_t> lastAcceptedSampleNs_{0};
	std::atomic<uint32_t> consecutiveRecoveryFailures_{0};
	std::atomic_bool setupConfirmedAtomic_{false};
	std::atomic<double> softSyncValidationErrorMs_{std::numeric_limits<double>::quiet_NaN()};
	std::atomic_bool driftControllerSaturated_{false};
	std::atomic<uint32_t> softSyncValidationFailures_{0};
	std::atomic<uint64_t> monitoringGraceUntilAtomicNs_{0};
	std::atomic_bool outputActiveAtomic_{false};
	std::atomic<uint64_t> snapshotRebuildAtNs_{0};
	std::atomic_bool snapshotRebuildResultReady_{false};
	bool restoringSnapshot_ = false;
	bool loadingConfig_ = true;
	bool promptActive_ = false;
	uint64_t moduleStartNs_ = os_gettime_ns();
	uint64_t monitoringGraceUntilNs_ = 0;
	uint64_t detectionSuppressedUntilNs_ = 0;
	uint64_t lastAutomatedResetNs_ = 0;
	uint64_t videoStallSinceNs_ = 0;
	uint64_t desktopStallSinceNs_ = 0;
	uint64_t micStallSinceNs_ = 0;
	uint64_t bothAudioStallSinceNs_ = 0;
	uint64_t entireGroupStallSinceNs_ = 0;
	uint64_t driftSinceNs_ = 0;
	uint64_t lastObservedIssueNs_ = 0;
	uint64_t lastIncidentStartNs_ = 0;
	uint64_t lastLifecycleScanNs_ = 0;
	uint64_t notificationHideAtNs_ = 0;
	bool setupConfirmed_ = false;
	bool firstRun_ = false;
	QString lastObservedIssueKey_;
	RecoveryTarget suggestedTarget_ = RecoveryTarget::None;
	int currentConfidence_ = 0;
	std::array<uint64_t, 3> loggedJumpCounts_{0, 0, 0};
	std::array<uint64_t, 10> issueEpisodeCounts_{};
	IssueKind activeSummaryIssue_ = IssueKind::None;
	IssueKind lastCountedIssue_ = IssueKind::None;
	uint64_t lastCountedIssueNs_ = 0;
	QString currentSummaryState_ = QStringLiteral("Starting");
	QString lastIssueSummary_;
	QDateTime lastIssueTime_;
	std::atomic<uint64_t> verifiedRecoveryCount_{0};
	std::atomic<uint64_t> failedRecoveryCount_{0};

	uint64_t engineVideoStallSinceNs_ = 0;
	uint64_t engineDesktopStallSinceNs_ = 0;
	uint64_t engineMicStallSinceNs_ = 0;
	uint64_t engineBothAudioStallSinceNs_ = 0;
	uint64_t engineEntireGroupStallSinceNs_ = 0;
	uint64_t engineDriftSinceNs_ = 0;
	uint64_t engineLastResetNs_ = 0;
	uint64_t engineDetectionSuppressedUntilNs_ = 0;
	std::deque<uint64_t> engineResetTimes_;
	RecoveryAttempt backgroundRecovery_;
	uint64_t lastSoftSyncControllerNs_ = 0;
	bool softSyncControllerPaused_ = false;

	std::array<obs_hotkey_id, 8> hotkeys_{OBS_INVALID_HOTKEY_ID, OBS_INVALID_HOTKEY_ID,
					      OBS_INVALID_HOTKEY_ID, OBS_INVALID_HOTKEY_ID,
					      OBS_INVALID_HOTKEY_ID, OBS_INVALID_HOTKEY_ID,
					      OBS_INVALID_HOTKEY_ID, OBS_INVALID_HOTKEY_ID};

	QWidget *createCollapsibleSection(const QString &title, QWidget *parent, QWidget *&content,
					 bool expanded = true)
	{
		auto *section = new QWidget(parent);
		section->setProperty("collapsibleSection", true);
		section->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);

		auto *sectionLayout = new QVBoxLayout(section);
		sectionLayout->setContentsMargins(0, 0, 0, 0);
		sectionLayout->setSpacing(0);
		sectionLayout->setSizeConstraint(QLayout::SetMinAndMaxSize);

		auto *headerWidget = new QWidget(section);
		headerWidget->setProperty("collapsibleHeader", true);
		headerWidget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
		headerWidget->setMinimumHeight(24);
		headerWidget->setMaximumHeight(26);
		auto *header = new QHBoxLayout(headerWidget);
		header->setContentsMargins(3, 0, 4, 0);
		header->setSpacing(2);

		auto *toggle = new QToolButton(headerWidget);
		toggle->setObjectName(QStringLiteral("SyncGuardianSectionToggle"));
		toggle->setCheckable(true);
		toggle->setChecked(expanded);
		toggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
		toggle->setToolButtonStyle(Qt::ToolButtonIconOnly);
		toggle->setAutoRaise(true);
		toggle->setFixedSize(18, 18);
		toggle->setFocusPolicy(Qt::NoFocus);
		toggle->setToolTip(QStringLiteral("Show or hide this section."));
		header->addWidget(toggle, 0, Qt::AlignLeft | Qt::AlignVCenter);

		auto *titleLabel = new QLabel(title, headerWidget);
		titleLabel->setProperty("collapsibleTitle", true);
		titleLabel->setMinimumWidth(0);
		titleLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
		header->addWidget(titleLabel, 1, Qt::AlignVCenter);
		sectionLayout->addWidget(headerWidget);

		content = new QWidget(section);
		content->setProperty("collapsibleContent", true);
		content->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
		content->setMinimumHeight(0);
		content->setMaximumHeight(expanded ? QWIDGETSIZE_MAX : 0);
		content->setVisible(expanded);
		sectionLayout->addWidget(content);

		QObject::connect(toggle, &QToolButton::toggled, section,
				 [section, content, toggle](bool shown) {
					 content->setMaximumHeight(shown ? QWIDGETSIZE_MAX : 0);
					 content->setVisible(shown);
					 toggle->setArrowType(shown ? Qt::DownArrow : Qt::RightArrow);
					 if (section->layout()) {
						 section->layout()->invalidate();
						 section->layout()->activate();
					 }
					 section->adjustSize();
					 section->updateGeometry();
					 if (section->parentWidget())
						 section->parentWidget()->updateGeometry();
				 });
		return section;
	}

	void buildUi()
	{
		panel_ = new QWidget();
		panel_->setObjectName(QStringLiteral("SyncGuardianPanel"));
		QFont compactFont = panel_->font();
		if (compactFont.pointSizeF() > 8.0)
			compactFont.setPointSizeF(std::max(8.0, compactFont.pointSizeF() - 0.75));
		panel_->setFont(compactFont);
		panel_->setStyleSheet(QStringLiteral(
			"#SyncGuardianPanel QWidget[collapsibleSection=\"true\"] { background-color: rgba(40, 44, 54, 150); border: 1px solid rgba(20, 23, 29, 165); border-radius: 4px; }"
			"#SyncGuardianPanel QWidget[collapsibleHeader=\"true\"] { background: transparent; border: none; }"
			"#SyncGuardianPanel QWidget[collapsibleContent=\"true\"] { background: transparent; border: none; margin: 0px; padding: 2px 5px 5px 5px; }"
			"#SyncGuardianPanel QLabel[collapsibleTitle=\"true\"] { color: #f0f3f8; font-weight: 600; padding: 0px; margin: 0px; border: none; }"
			"#SyncGuardianPanel QLabel, #SyncGuardianPanel QCheckBox { color: #dde3ec; }"
			"#SyncGuardianPanel QTableWidget { color: #e2e7ef; }"
			"#SyncGuardianPanel QPushButton { padding: 2px 5px; min-height: 20px; }"
			"#SyncGuardianPanel QToolButton { color: #cfd6e1; padding: 0px; margin: 0px; border: none; }"
			"#SyncGuardianPanel QComboBox, #SyncGuardianPanel QSpinBox { min-height: 20px; }"
			"#SyncGuardianPanel QToolTip { color: #f3f6fb; background-color: #2e3440; border: 1px solid #7f8a9a; }"));

		// Keep the OBS dock compact on lower-resolution displays. The outer widget
		// always fits the available dock area while the full control surface scrolls.
		auto *outerLayout = new QVBoxLayout(panel_);
		outerLayout->setContentsMargins(0, 0, 0, 0);
		outerLayout->setSpacing(0);

		auto *scrollArea = new QScrollArea(panel_);
		scrollArea->setObjectName(QStringLiteral("SyncGuardianScrollArea"));
		scrollArea->setWidgetResizable(true);
		scrollArea->setFrameShape(QFrame::NoFrame);
		scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
		scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

		auto *scrollContent = new QWidget(scrollArea);
		scrollContent->setObjectName(QStringLiteral("SyncGuardianScrollContent"));
		auto *root = new QVBoxLayout(scrollContent);
		root->setContentsMargins(4, 4, 4, 4);
		root->setSpacing(3);
		root->setSizeConstraint(QLayout::SetMinAndMaxSize);

		auto *dashboard = new QWidget(scrollContent);
		dashboard->setObjectName(QStringLiteral("SyncGuardianDashboard"));
		auto *dashboardLayout = new QGridLayout(dashboard);
		dashboardLayout->setContentsMargins(6, 5, 6, 5);
		dashboardLayout->setHorizontalSpacing(5);
		dashboardLayout->setVerticalSpacing(2);
		compactStatusLabel_ = new QLabel(QStringLiteral("Starting"), dashboard);
		compactStatusLabel_->setStyleSheet(QStringLiteral("font-weight: 700; font-size: 13px;"));
		compactOffsetLabel_ = new QLabel(QStringLiteral("Corrected offset: —"), dashboard);
		compactOffsetLabel_->setWordWrap(true);
		readinessLabel_ = new QLabel(QStringLiteral("Checking protection readiness…"), dashboard);
		readinessLabel_->setWordWrap(true);
		notificationLabel_ = new QLabel(dashboard);
		notificationLabel_->setWordWrap(true);
		notificationLabel_->setVisible(false);
		automaticProtection_ = new QCheckBox(QStringLiteral("Automatic Protection"), dashboard);
		automaticProtection_->setChecked(true);
		automaticProtection_->setToolTip(QStringLiteral("Automatically corrects slow drift and performs guarded targeted recovery for confirmed jumps or stalls."));
		automationMode_ = new QComboBox(dashboard);
		automationMode_->addItem(QStringLiteral("Monitor"), static_cast<int>(AutomationMode::Observe));
		automationMode_->addItem(QStringLiteral("Ask"), static_cast<int>(AutomationMode::Ask));
		automationMode_->addItem(QStringLiteral("Auto"), static_cast<int>(AutomationMode::Automatic));
		automationMode_->setToolTip(QStringLiteral("Monitor reports only. Ask requests confirmation. Auto performs guarded jump recovery."));
		fixNowButton_ = new QPushButton(QStringLiteral("Fix now"), dashboard);
		fixNowButton_->setEnabled(false);
		fixNowButton_->setToolTip(QStringLiteral("Runs the smallest recovery currently recommended by Sync Guardian."));
		confirmSetupButton_ = new QPushButton(QStringLiteral("Confirm detected setup"), dashboard);
		dashboardLayout->addWidget(compactStatusLabel_, 0, 0, 1, 2);
		dashboardLayout->addWidget(compactOffsetLabel_, 1, 0, 1, 2);
		dashboardLayout->addWidget(readinessLabel_, 2, 0, 1, 2);
		dashboardLayout->addWidget(automaticProtection_, 3, 0);
		dashboardLayout->addWidget(fixNowButton_, 3, 1);
		dashboardLayout->addWidget(confirmSetupButton_, 4, 0, 1, 2);
		dashboardLayout->addWidget(notificationLabel_, 5, 0, 1, 2);
		root->addWidget(dashboard);
		QObject::connect(fixNowButton_, &QPushButton::clicked, [this]() {
			if (suggestedTarget_ != RecoveryTarget::None)
				manualReset(suggestedTarget_, QStringLiteral("Contextual Fix now"));
		});

		QWidget *sourceContent = nullptr;
		auto *sourceBox = createCollapsibleSection(QStringLiteral("Sources"), scrollContent, sourceContent, false);
		auto *sourceForm = new QFormLayout(sourceContent);
		sourceForm->setContentsMargins(0, 0, 0, 0);
		sourceForm->setHorizontalSpacing(6);
		sourceForm->setVerticalSpacing(3);
		auto *sourceHelp = new QLabel(QStringLiteral(
			"Choose the DistroAV receiver sources on this streaming PC. Video timing is read by a pass-through timestamp probe."),
			sourceContent);
		sourceHelp->setWordWrap(true);
		sourceHelp->setStyleSheet(QStringLiteral("color: #c7ced8;"));
		sourceForm->addRow(sourceHelp);
		for (size_t i = 0; i < sourceCombos_.size(); ++i) {
			sourceCombos_[i] = new QComboBox(sourceContent);
			sourceCombos_[i]->setSizeAdjustPolicy(QComboBox::AdjustToContents);
			sourceCombos_[i]->setToolTip(QStringLiteral("Select the OBS DistroAV source used for this role. Use Rescan Sources after adding or renaming a source."));
			sourceForm->addRow(states_[i].role + QStringLiteral(":"), sourceCombos_[i]);
			QObject::connect(sourceCombos_[i], QOverload<int>::of(&QComboBox::currentIndexChanged),
					 [this, i](int) {
						bindSource(i, sourceCombos_[i]->currentData().toString());
						setupConfirmed_ = false;
						setupConfirmedAtomic_.store(false);
						clearCalibration(QStringLiteral("source mapping changed"), false);
						saveConfig();
					 });
		}
		auto *sourceActions = new QWidget(sourceContent);
		auto *sourceActionsLayout = new QHBoxLayout(sourceActions);
		sourceActionsLayout->setContentsMargins(0, 0, 0, 0);
		auto *detectSourcesButton = new QPushButton(QStringLiteral("Detect sources"), sourceActions);
		sourceActionsLayout->addWidget(detectSourcesButton);
		sourceForm->addRow(sourceActions);
		QObject::connect(detectSourcesButton, &QPushButton::clicked, [this]() { suggestSourceMappings(true); });
		QObject::connect(confirmSetupButton_, &QPushButton::clicked, [this]() {
			setupConfirmed_ = true;
			setupConfirmedAtomic_.store(true);
			saveConfig();
			showNotification(QStringLiteral("Setup confirmed. Protection will enable after timing becomes reliable."), false);
		});
		root->addWidget(sourceBox);

		QWidget *automationContent = nullptr;
		auto *automationBox = createCollapsibleSection(QStringLiteral("Protection settings"), scrollContent, automationContent, false);
		auto *automationForm = new QFormLayout(automationContent);
		automationForm->setContentsMargins(0, 0, 0, 0);
		automationForm->setHorizontalSpacing(6);
		automationForm->setVerticalSpacing(3);
		auto *simpleHelp = new QLabel(QStringLiteral(
			"Automatic Protection corrects slow drift and performs guarded recovery for confirmed timestamp jumps or stalls."),
			automationContent);
		simpleHelp->setWordWrap(true);
		simpleHelp->setStyleSheet(QStringLiteral("color: #c7ced8;"));
		automationForm->addRow(simpleHelp);
		automationForm->addRow(QStringLiteral("Detailed protection mode:"), automationMode_);
		autoTuneThresholds_ = new QCheckBox(QStringLiteral("Automatically tune stall thresholds"), automationContent);
		autoTuneThresholds_->setChecked(true);
		autoTuneThresholds_->setToolTip(QStringLiteral("Learns healthy callback gaps and raises stall thresholds enough to ignore routine scheduling jitter."));
		automationForm->addRow(autoTuneThresholds_);
		effectiveThresholdsLabel_ = new QLabel(QStringLiteral("Effective thresholds: learning"), automationContent);
		effectiveThresholdsLabel_->setWordWrap(true);
		automationForm->addRow(effectiveThresholdsLabel_);
		auto *qolActions = new QWidget(automationContent);
		auto *qolActionsLayout = new QHBoxLayout(qolActions);
		qolActionsLayout->setContentsMargins(0, 0, 0, 0);
		protectionTestButton_ = new QPushButton(QStringLiteral("Test protection"), qolActions);
		exportReportButton_ = new QPushButton(QStringLiteral("Export support report"), qolActions);
		qolActionsLayout->addWidget(protectionTestButton_);
		qolActionsLayout->addWidget(exportReportButton_);
		automationForm->addRow(qolActions);
		QObject::connect(protectionTestButton_, &QPushButton::clicked, [this]() { runProtectionTest(); });
		QObject::connect(exportReportButton_, &QPushButton::clicked, [this]() { exportSupportReport(); });

		advancedToggleButton_ = new QPushButton(QStringLiteral("Show advanced detection settings"), automationContent);
		advancedToggleButton_->setCheckable(true);
		advancedToggleButton_->setChecked(false);
		advancedToggleButton_->setToolTip(QStringLiteral(
			"Shows timing thresholds and safety limits. The defaults are deliberately conservative and normally do not need adjustment."));
		automationForm->addRow(advancedToggleButton_);

		advancedSettingsWidget_ = new QWidget(automationContent);
		auto *advancedForm = new QFormLayout(advancedSettingsWidget_);
		advancedForm->setContentsMargins(0, 2, 0, 0);
		advancedForm->setHorizontalSpacing(6);
		advancedForm->setVerticalSpacing(3);

		onlyWhenOutputActive_ = new QCheckBox(QStringLiteral("Only act while output is active"), advancedSettingsWidget_);
		onlyWhenOutputActive_->setChecked(true);
		requireActiveSources_ = new QCheckBox(QStringLiteral("Require active/showing sources"), advancedSettingsWidget_);
		requireActiveSources_->setChecked(true);
		enableFreezeDetection_ = new QCheckBox(QStringLiteral("Detect stalls/frozen video"), advancedSettingsWidget_);
		enableFreezeDetection_->setChecked(true);
		enableDriftDetection_ = new QCheckBox(QStringLiteral("Detect persistent A/V drift"), advancedSettingsWidget_);
		enableDriftDetection_->setChecked(true);
		autoEscalate_ = new QCheckBox(QStringLiteral("Escalate failed reset to full-group rebuild"), advancedSettingsWidget_);
		autoEscalate_->setChecked(true);
		onlyWhenOutputActive_->setToolTip(QStringLiteral("Prevents automatic recovery while neither streaming nor recording. Monitoring still continues."));
		requireActiveSources_->setToolTip(QStringLiteral("Suppresses detection when the mapped sources are inactive, hidden, or disabled."));
		enableFreezeDetection_->setToolTip(QStringLiteral("Detects a timestamp or packet stream that stops while a companion stream continues."));
		enableDriftDetection_->setToolTip(QStringLiteral("Compares the filtered video/audio timestamp relationship against the calibrated normal baseline."));
		autoEscalate_->setToolTip(QStringLiteral("Allows one full-group rebuild if a targeted automatic reset fails verification."));
		auto *automationChecks = new QWidget(advancedSettingsWidget_);
		auto *automationChecksGrid = new QGridLayout(automationChecks);
		automationChecksGrid->setContentsMargins(0, 0, 0, 0);
		automationChecksGrid->setHorizontalSpacing(8);
		automationChecksGrid->setVerticalSpacing(1);
		automationChecksGrid->addWidget(onlyWhenOutputActive_, 0, 0);
		automationChecksGrid->addWidget(requireActiveSources_, 0, 1);
		automationChecksGrid->addWidget(enableFreezeDetection_, 1, 0);
		automationChecksGrid->addWidget(enableDriftDetection_, 1, 1);
		automationChecksGrid->addWidget(autoEscalate_, 2, 0, 1, 2);
		advancedForm->addRow(automationChecks);

		videoStallMs_ = createSpin(advancedSettingsWidget_, 250, 10000, 1000, QStringLiteral(" ms"));
		audioStallMs_ = createSpin(advancedSettingsWidget_, 250, 10000, 1000, QStringLiteral(" ms"));
		driftThresholdMs_ = createSpin(advancedSettingsWidget_, 50, 2000, 200, QStringLiteral(" ms"));
		driftPersistenceMs_ = createSpin(advancedSettingsWidget_, 1000, 60000, 10000, QStringLiteral(" ms"));
		cooldownSec_ = createSpin(advancedSettingsWidget_, 10, 3600, 180, QStringLiteral(" sec"));
		maxAutoResetsPerHour_ = createSpin(advancedSettingsWidget_, 1, 20, 3, QStringLiteral(" / hour"));
		startupGraceSec_ = createSpin(advancedSettingsWidget_, 5, 300, 30, QStringLiteral(" sec"));
		verifyDelaySec_ = createSpin(advancedSettingsWidget_, 2, 30, 5, QStringLiteral(" sec"));
		videoStallMs_->setToolTip(QStringLiteral("Default 1000 ms plus a fixed 500 ms confirmation window before action."));
		audioStallMs_->setToolTip(QStringLiteral("Default 1000 ms plus a fixed 500 ms confirmation window before action."));
		driftThresholdMs_->setToolTip(QStringLiteral("A single timestamp jump does not trigger recovery. With Adaptive Soft Sync active, this threshold follows the estimated corrected output drift; otherwise it follows raw transport drift."));
		driftPersistenceMs_->setToolTip(QStringLiteral("How long a filtered A/V offset must remain beyond the threshold before recovery is suggested."));
		cooldownSec_->setToolTip(QStringLiteral("Minimum delay between automatic recovery attempts."));
		maxAutoResetsPerHour_->setToolTip(QStringLiteral("Hard safety cap across targeted resets and full-group escalations."));
		startupGraceSec_->setToolTip(QStringLiteral("Suppresses detection while sources start, reconnect, or settle after a scene change."));
		verifyDelaySec_->setToolTip(QStringLiteral("How long to wait after a reset before deciding whether the source recovered."));
		advancedForm->addRow(QStringLiteral("Video stall threshold:"), videoStallMs_);
		advancedForm->addRow(QStringLiteral("Audio stall threshold:"), audioStallMs_);
		advancedForm->addRow(QStringLiteral("Persistent drift threshold:"), driftThresholdMs_);
		advancedForm->addRow(QStringLiteral("Drift must persist for:"), driftPersistenceMs_);
		advancedForm->addRow(QStringLiteral("Automatic reset cooldown:"), cooldownSec_);
		advancedForm->addRow(QStringLiteral("Maximum automatic resets:"), maxAutoResetsPerHour_);
		advancedForm->addRow(QStringLiteral("Startup/scene-change grace:"), startupGraceSec_);
		advancedForm->addRow(QStringLiteral("Recovery verification delay:"), verifyDelaySec_);

		auto *calibrationButtons = new QWidget(advancedSettingsWidget_);
		auto *calibrationLayout = new QGridLayout(calibrationButtons);
		calibrationLayout->setContentsMargins(0, 0, 0, 0);
		calibrationLayout->setHorizontalSpacing(4);
		auto *setBaseline = new QPushButton(QStringLiteral("Set Current Baseline"), calibrationButtons);
		auto *autoCalibrate = new QPushButton(QStringLiteral("Restart Auto Calibration"), calibrationButtons);
		setBaseline->setToolTip(QStringLiteral("Treats the current stable video/audio timestamp relationship as normal."));
		autoCalibrate->setToolTip(QStringLiteral("Clears the current baseline and learns a new one from 30 seconds of stable data."));
		calibrationLayout->addWidget(setBaseline, 0, 0);
		calibrationLayout->addWidget(autoCalibrate, 0, 1);
		advancedForm->addRow(QStringLiteral("Calibration:"), calibrationButtons);
		advancedSettingsWidget_->setVisible(false);
		automationForm->addRow(advancedSettingsWidget_);
		root->addWidget(automationBox);

		QWidget *softSyncContent = nullptr;
		auto *softSyncBox = createCollapsibleSection(QStringLiteral("Drift Controller"), scrollContent, softSyncContent, false);
		auto *softSyncForm = new QFormLayout(softSyncContent);
		softSyncForm->setContentsMargins(0, 0, 0, 0);
		softSyncForm->setHorizontalSpacing(6);
		softSyncForm->setVerticalSpacing(3);
		auto *softSyncHelp = new QLabel(QStringLiteral(
			"Optional and disabled by default. Soft Sync applies a tiny, slowly changing resample correction to desktop audio instead of making large cuts. Disabling it removes the filter and returns the source to untouched pass-through audio."),
			softSyncContent);
		softSyncHelp->setWordWrap(true);
		softSyncHelp->setStyleSheet(QStringLiteral("color: #c7ced8;"));
		softSyncForm->addRow(softSyncHelp);

		enableSoftSync_ = new QCheckBox(QStringLiteral("Enable Adaptive Soft Sync"), softSyncContent);
		enableSoftSync_->setChecked(false);
		enableSoftSync_->setToolTip(QStringLiteral("Attaches Sync Guardian's private adaptive resampling filter to the mapped desktop-audio source. Off means no resampling code is active on the source."));
		softSyncForm->addRow(enableSoftSync_);

		softSyncLinkMic_ = new QCheckBox(QStringLiteral("Apply the same correction to the mapped mic"), softSyncContent);
		softSyncLinkMic_->setChecked(false);
		softSyncLinkMic_->setToolTip(QStringLiteral("Keeps desktop audio and microphone on the same corrected rate. Leave this off when the microphone must stay aligned to a separate camera feed."));
		softSyncForm->addRow(softSyncLinkMic_);

		softSyncDeadZoneMs_ = createSpin(softSyncContent, 5, 100, 12, QStringLiteral(" ms"));
		softSyncMaxPpm_ = createSpin(softSyncContent, 5, 5000, 50, QStringLiteral(" ppm"));
		softSyncSlewPpmPerSec_ = createSpin(softSyncContent, 1, 1000, 100, QStringLiteral(" ppm/sec"));
		softSyncDeadZoneMs_->setToolTip(QStringLiteral("Small drift inside this range is treated as normal jitter and does not add position correction."));
		softSyncMaxPpm_->setToolTip(QStringLiteral("Hard ceiling for adaptive speed correction. 5000 ppm equals 0.5% speed and is intended only for catching up larger errors; normal steady drift should use far less."));
		softSyncSlewPpmPerSec_->setToolTip(QStringLiteral("Acceleration limit in ppm per second. Braking back toward neutral is allowed at twice this rate so correction can stop cleanly without lingering or overshooting."));
		softSyncForm->addRow(QStringLiteral("Drift dead zone:"), softSyncDeadZoneMs_);
		softSyncForm->addRow(QStringLiteral("Maximum correction:"), softSyncMaxPpm_);
		softSyncForm->addRow(QStringLiteral("Correction slew limit:"), softSyncSlewPpmPerSec_);

		softSyncStatusLabel_ = new QLabel(QStringLiteral("Soft Sync: Off — audio passes through unchanged"), softSyncContent);
		softSyncStatusLabel_->setWordWrap(true);
		softSyncStatusLabel_->setStyleSheet(QStringLiteral("color: #d8dde6;"));
		softSyncForm->addRow(softSyncStatusLabel_);

		auto *disableSoftSyncButton = new QPushButton(QStringLiteral("Disable and remove Soft Sync filters"), softSyncContent);
		disableSoftSyncButton->setToolTip(QStringLiteral("Immediately disables correction, removes Sync Guardian's private audio filters, and leaves NDI audio as normal pass-through."));
		softSyncForm->addRow(disableSoftSyncButton);
		root->addWidget(softSyncBox);

		QObject::connect(disableSoftSyncButton, &QPushButton::clicked, [this]() {
			{
				QSignalBlocker blocker(enableSoftSync_);
				enableSoftSync_->setChecked(false);
			}
			updateEngineSettings();
			syncSoftSyncFilters();
			saveConfig();
			appendEvent(QStringLiteral("Adaptive Soft Sync disabled and private audio filters removed"), false);
		});

		QWidget *controlContent = nullptr;
		auto *controlBox = createCollapsibleSection(QStringLiteral("Manual recovery"), scrollContent, controlContent, false);
		auto *controlGrid = new QGridLayout(controlContent);
		controlGrid->setContentsMargins(0, 0, 0, 0);
		controlGrid->setHorizontalSpacing(4);
		controlGrid->setVerticalSpacing(3);
		manualSuggestionLabel_ = new QLabel(QStringLiteral("Suggested manual action: none — monitoring healthy. Hover over a button for help."), controlContent);
		manualSuggestionLabel_->setWordWrap(true);
		manualSuggestionLabel_->setObjectName(QStringLiteral("SyncGuardianManualSuggestion"));
		manualSuggestionLabel_->setToolTip(QStringLiteral("Shows the simplest recommended manual action when Sync Guardian detects a likely issue."));
		controlGrid->addWidget(manualSuggestionLabel_, 0, 0, 1, 2);

		resetVideoButton_ = new QPushButton(QStringLiteral("Reset Video Only"), controlContent);
		resetDesktopButton_ = new QPushButton(QStringLiteral("Reset Desktop Audio Only"), controlContent);
		resetMicButton_ = new QPushButton(QStringLiteral("Reset Mic Only"), controlContent);
		resetBothAudioButton_ = new QPushButton(QStringLiteral("Reset Both Audio Sources"), controlContent);
		rebuildGroupButton_ = new QPushButton(QStringLiteral("Rebuild Entire Sync Group"), controlContent);
		auto *captureSnapshot = new QPushButton(QStringLiteral("Save Current Settings"), controlContent);
		auto *restoreSnapshot = new QPushButton(QStringLiteral("Restore Saved Settings"), controlContent);
		auto *markEventButton = new QPushButton(QStringLiteral("Add Log Marker"), controlContent);
		auto *refreshSources = new QPushButton(QStringLiteral("Rescan Sources"), controlContent);

		resetVideoButton_->setToolTip(QStringLiteral("Restarts only the mapped NDI video receiver. Use when video freezes or falls behind while audio remains healthy."));
		resetDesktopButton_->setToolTip(QStringLiteral("Restarts only the mapped desktop/game audio receiver. Use when that audio stops or is the likely drifting stream."));
		resetMicButton_->setToolTip(QStringLiteral("Restarts only the mapped microphone receiver."));
		resetBothAudioButton_->setToolTip(QStringLiteral("Restarts both mapped NDI audio receivers together. Use when both audio streams are stale or need to be realigned together."));
		rebuildGroupButton_->setToolTip(QStringLiteral("Restarts all mapped NDI receivers while preserving each source's DistroAV properties, audio routing, sync offset, monitoring, and other OBS-level settings. This is the broadest recovery action."));
		captureSnapshot->setToolTip(QStringLiteral("Saves the current DistroAV source settings in memory for this OBS session and records the current sync baseline when available."));
		restoreSnapshot->setToolTip(QStringLiteral("Restores the settings saved with Save Current Settings, then rebuilds the mapped receivers."));
		markEventButton->setToolTip(QStringLiteral("Adds a timestamped note to the Sync Guardian event log and, when enabled, the current recording chapter list."));
		refreshSources->setToolTip(QStringLiteral("Rescans OBS for DistroAV receiver sources after sources are added, removed, or renamed."));

		controlGrid->addWidget(resetVideoButton_, 1, 0);
		controlGrid->addWidget(resetDesktopButton_, 1, 1);
		controlGrid->addWidget(resetMicButton_, 2, 0);
		controlGrid->addWidget(resetBothAudioButton_, 2, 1);
		controlGrid->addWidget(rebuildGroupButton_, 3, 0, 1, 2);
		controlGrid->addWidget(captureSnapshot, 4, 0);
		controlGrid->addWidget(restoreSnapshot, 4, 1);
		controlGrid->addWidget(markEventButton, 5, 0);
		controlGrid->addWidget(refreshSources, 5, 1);

		pulseDurationMs_ = createSpin(controlContent, 50, 1500, 180, QStringLiteral(" ms"));
		pulseDurationMs_->setToolTip(QStringLiteral("How long Sync Guardian temporarily changes FrameSync to force a receiver rebuild. DistroAV properties and OBS-level source settings are captured, restored non-destructively, and verified afterward; the default normally should not be changed."));
		controlGrid->addWidget(new QLabel(QStringLiteral("Reset pulse duration:"), controlContent), 6, 0);
		controlGrid->addWidget(pulseDurationMs_, 6, 1);
		chapterMarkers_ = new QCheckBox(QStringLiteral("Add recording chapter on actions"), controlContent);
		chapterMarkers_->setChecked(true);
		jsonLogging_ = new QCheckBox(QStringLiteral("Append events to sync-guardian-events.jsonl"), controlContent);
		jsonLogging_->setChecked(true);
		incidentReports_ = new QCheckBox(QStringLiteral("Capture 30 seconds before/after detected incidents"), controlContent);
		incidentReports_->setChecked(true);
		controlGrid->addWidget(chapterMarkers_, 7, 0, 1, 2);
		controlGrid->addWidget(jsonLogging_, 8, 0, 1, 2);
		controlGrid->addWidget(incidentReports_, 9, 0, 1, 2);
		root->addWidget(controlBox);

		QObject::connect(advancedToggleButton_, &QPushButton::toggled, [this](bool shown) {
			advancedSettingsWidget_->setVisible(shown);
			advancedToggleButton_->setText(shown ? QStringLiteral("Hide advanced detection settings")
						      : QStringLiteral("Show advanced detection settings"));
			saveConfig();
		});

		QObject::connect(resetVideoButton_, &QPushButton::clicked,
				 [this]() { manualReset(RecoveryTarget::Video, QStringLiteral("Reset Video Only")); });
		QObject::connect(resetDesktopButton_, &QPushButton::clicked,
				 [this]() { manualReset(RecoveryTarget::DesktopAudio, QStringLiteral("Reset Desktop Audio Only")); });
		QObject::connect(resetMicButton_, &QPushButton::clicked,
				 [this]() { manualReset(RecoveryTarget::Mic, QStringLiteral("Reset Mic Only")); });
		QObject::connect(resetBothAudioButton_, &QPushButton::clicked,
				 [this]() { manualReset(RecoveryTarget::BothAudio, QStringLiteral("Reset Both Audio Sources")); });
		QObject::connect(rebuildGroupButton_, &QPushButton::clicked,
				 [this]() { manualReset(RecoveryTarget::EntireGroup, QStringLiteral("Rebuild Entire Sync Group")); });
		QObject::connect(captureSnapshot, &QPushButton::clicked, [this]() { captureSnapshotState(); });
		QObject::connect(restoreSnapshot, &QPushButton::clicked, [this]() { restoreSnapshotState(); });
		QObject::connect(setBaseline, &QPushButton::clicked, [this]() {
			const double filtered = filteredOffsetMs_.load();
			const double current = std::isfinite(filtered) ? filtered : calculateAvOffsetMs();
			if (std::isfinite(current)) {
				baselineOffsetMs_.store(current);
				appendEvent(QStringLiteral("A/V baseline manually set to %1 ms").arg(current, 0, 'f', 2));
			} else {
				appendEvent(QStringLiteral("A/V baseline unavailable: both video and desktop audio must be fresh"), false);
			}
		});
		QObject::connect(autoCalibrate, &QPushButton::clicked,
				 [this]() { clearCalibration(QStringLiteral("manual auto-calibration restart"), true); });
		QObject::connect(markEventButton, &QPushButton::clicked,
				 [this]() { appendEvent(QStringLiteral("Manual sync event marker")); });
		QObject::connect(refreshSources, &QPushButton::clicked, [this]() {
			refreshSourceLists();
			bindAllSources();
			clearCalibration(QStringLiteral("source list refreshed"), false);
			appendEvent(QStringLiteral("NDI source list refreshed"), false);
		});

		QWidget *diagnosticsContent = nullptr;
		auto *diagnosticsBox = createCollapsibleSection(QStringLiteral("Numbers and diagnostics"), scrollContent,
								 diagnosticsContent, false);
		auto *diagnosticsLayout = new QVBoxLayout(diagnosticsContent);
		diagnosticsLayout->setContentsMargins(0, 0, 0, 0);
		diagnosticsLayout->setSpacing(3);
		overallSummaryLabel_ = new QLabel(QStringLiteral("Summary: starting monitoring"), diagnosticsContent);
		overallSummaryLabel_->setWordWrap(true);
		overallSummaryLabel_->setStyleSheet(QStringLiteral("font-weight: 600; color: #e9edf4;"));
		driftLabel_ = new QLabel(QStringLiteral("Drift from baseline: —"), diagnosticsContent);
		driftLabel_->setWordWrap(true);
		driftLabel_->setStyleSheet(QStringLiteral("font-size: 14px; font-weight: 700; color: #dde3ec;"));
		driftTrendLabel_ = new QLabel(QStringLiteral("Trend: settling"), diagnosticsContent);
		driftTrendLabel_->setWordWrap(true);
		driftTrendLabel_->setStyleSheet(QStringLiteral("color: #cbd3df;"));
		diagnosticsLayout->addWidget(overallSummaryLabel_);
		diagnosticsLayout->addWidget(driftLabel_);
		diagnosticsLayout->addWidget(driftTrendLabel_);

		diagnosticDetailsToggle_ = new QToolButton(diagnosticsContent);
		diagnosticDetailsToggle_->setCheckable(true);
		diagnosticDetailsToggle_->setChecked(false);
		diagnosticDetailsToggle_->setArrowType(Qt::RightArrow);
		diagnosticDetailsToggle_->setText(QStringLiteral("Show technical details"));
		diagnosticDetailsToggle_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
		diagnosticsLayout->addWidget(diagnosticDetailsToggle_, 0, Qt::AlignLeft);

		diagnosticDetailsWidget_ = new QWidget(diagnosticsContent);
		auto *detailsLayout = new QVBoxLayout(diagnosticDetailsWidget_);
		detailsLayout->setContentsMargins(0, 1, 0, 0);
		detailsLayout->setSpacing(2);
		automationStatusLabel_ = new QLabel(QStringLiteral("Automation: starting"), diagnosticDetailsWidget_);
		healthLabel_ = new QLabel(QStringLiteral("Measurement: Waiting"), diagnosticDetailsWidget_);
		avOffsetLabel_ = new QLabel(QStringLiteral("Video − Desktop Audio timestamp: —"), diagnosticDetailsWidget_);
		micOffsetLabel_ = new QLabel(QStringLiteral("Mic − Desktop Audio timestamp: —"), diagnosticDetailsWidget_);
		obsStatsLabel_ = new QLabel(QStringLiteral("OBS: —"), diagnosticDetailsWidget_);
		detailsLayout->addWidget(automationStatusLabel_);
		detailsLayout->addWidget(healthLabel_);
		detailsLayout->addWidget(avOffsetLabel_);
		detailsLayout->addWidget(micOffsetLabel_);
		detailsLayout->addWidget(obsStatsLabel_);

		statusTable_ = new QTableWidget(3, 8, diagnosticDetailsWidget_);
		statusTable_->setHorizontalHeaderLabels({QStringLiteral("Role"), QStringLiteral("OBS source"),
							 QStringLiteral("State"), QStringLiteral("Packet age"),
							 QStringLiteral("Timestamp jumps"), QStringLiteral("Resets"),
							 QStringLiteral("Verified recoveries"), QStringLiteral("DistroAV settings")});
		statusTable_->verticalHeader()->setVisible(false);
		statusTable_->setToolTip(QStringLiteral(
			"Packet age shows how long it has been since a timestamp advanced. Timestamp jumps are sudden timing corrections. Verified recoveries only count resets that passed the post-reset health check."));
		statusTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
		statusTable_->setSelectionMode(QAbstractItemView::NoSelection);
		statusTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
		statusTable_->horizontalHeader()->setStretchLastSection(true);
		statusTable_->verticalHeader()->setDefaultSectionSize(21);
		statusTable_->horizontalHeader()->setMinimumHeight(22);
		statusTable_->setMaximumHeight(112);
		for (int row = 0; row < 3; ++row) {
			for (int col = 0; col < 8; ++col)
				statusTable_->setItem(row, col, new QTableWidgetItem());
		}
		detailsLayout->addWidget(statusTable_);
		diagnosticDetailsWidget_->setVisible(false);
		diagnosticsLayout->addWidget(diagnosticDetailsWidget_);
		QObject::connect(diagnosticDetailsToggle_, &QToolButton::toggled, [this](bool shown) {
			diagnosticDetailsWidget_->setVisible(shown);
			diagnosticDetailsToggle_->setArrowType(shown ? Qt::DownArrow : Qt::RightArrow);
			diagnosticDetailsToggle_->setText(shown ? QStringLiteral("Hide technical details")
								    : QStringLiteral("Show technical details"));
		});
		root->addWidget(diagnosticsBox);

		QWidget *eventContent = nullptr;
		auto *eventBox = createCollapsibleSection(QStringLiteral("Event history"), scrollContent, eventContent, false);
		auto *eventLayout = new QVBoxLayout(eventContent);
		eventLayout->setContentsMargins(0, 0, 0, 0);
		eventLog_ = new QTextEdit(eventContent);
		eventLog_->setReadOnly(true);
		eventLog_->setMinimumHeight(72);
		eventLog_->setMaximumHeight(110);
		eventLayout->addWidget(eventLog_);
		root->addWidget(eventBox);

		scrollArea->setWidget(scrollContent);
		outerLayout->addWidget(scrollArea);

		connectSettingsSignals();
		if (!obs_frontend_add_dock_by_id(kDockId, "Sync Guardian", panel_))
			blog(LOG_ERROR, "[sync-guardian] Failed to add dock; dock id may already be registered");
	}

	QSpinBox *createSpin(QWidget *parent, int minimum, int maximum, int value, const QString &suffix)
	{
		auto *spin = new QSpinBox(parent);
		spin->setRange(minimum, maximum);
		spin->setValue(value);
		spin->setSuffix(suffix);
		return spin;
	}

	void validateParameterBindings() const
	{
		const std::array<const QWidget *, 35> required = {
			panel_, automationMode_, advancedToggleButton_, advancedSettingsWidget_, onlyWhenOutputActive_,
			requireActiveSources_, enableFreezeDetection_, enableDriftDetection_, autoEscalate_, videoStallMs_,
			audioStallMs_, driftThresholdMs_, driftPersistenceMs_, cooldownSec_, maxAutoResetsPerHour_,
			startupGraceSec_, verifyDelaySec_, pulseDurationMs_, enableSoftSync_, softSyncLinkMic_,
			softSyncDeadZoneMs_, softSyncMaxPpm_, softSyncSlewPpmPerSec_, chapterMarkers_, jsonLogging_,
			incidentReports_, fixNowButton_, automaticProtection_, autoTuneThresholds_, readinessLabel_,
			notificationLabel_, effectiveThresholdsLabel_, confirmSetupButton_, protectionTestButton_, exportReportButton_};
		for (const QWidget *widget : required) {
			if (!widget) {
				blog(LOG_ERROR, "[sync-guardian] v0.5 parameter audit failed: a required UI/runtime binding is null");
				return;
			}
		}
		const bool rangesValid = videoStallMs_->minimum() <= videoStallMs_->value() &&
			videoStallMs_->value() <= videoStallMs_->maximum() &&
			audioStallMs_->minimum() <= audioStallMs_->value() && audioStallMs_->value() <= audioStallMs_->maximum() &&
			softSyncMaxPpm_->minimum() <= softSyncMaxPpm_->value() && softSyncMaxPpm_->value() <= softSyncMaxPpm_->maximum();
		blog(rangesValid ? LOG_INFO : LOG_ERROR,
		     rangesValid ? "[sync-guardian] v0.5 parameter audit passed: visible controls have runtime bindings and valid ranges"
				 : "[sync-guardian] v0.5 parameter audit failed: one or more defaults are outside their control range");
	}

	void showNotification(const QString &message, bool warning)
	{
		if (!notificationLabel_)
			return;
		notificationLabel_->setText(message);
		notificationLabel_->setStyleSheet(warning
			? QStringLiteral("color: #ffb4a9; font-weight: 600;")
			: QStringLiteral("color: #8bd49c; font-weight: 600;"));
		notificationLabel_->setVisible(true);
		notificationHideAtNs_ = os_gettime_ns() + 10ULL * kNsPerSecond;
	}

	void queueUserNotification(const QString &message, bool warning)
	{
		QMetaObject::invokeMethod(lifetimeContext_, [this, message, warning]() {
			showNotification(message, warning);
		}, Qt::QueuedConnection);
	}

	int effectiveStallThresholdMs(size_t index) const
	{
		const int configured = index == 0 ? engineSettings_.videoStallMs.load() : engineSettings_.audioStallMs.load();
		if (!engineSettings_.autoTuneThresholds.load())
			return configured;
		const uint64_t gapNs = states_[index].observedHealthyGapNs.load();
		if (!gapNs)
			return configured;
		const int learned = static_cast<int>(gapNs / kNsPerMs) * 4 + 250;
		return std::clamp(std::max(configured, learned), 250, 10000);
	}

	QString senderHostForSource(const QString &sourceName) const
	{
		obs_source_t *source = obs_get_source_by_name(sourceName.toUtf8().constData());
		if (!source)
			return QString();
		obs_data_t *settings = obs_source_get_settings(source);
		QString target = QString::fromUtf8(obs_data_get_string(settings, kPropSource)).trimmed();
		obs_data_release(settings);
		obs_source_release(source);
		int split = target.indexOf(QStringLiteral(" ("));
		if (split < 0)
			split = target.indexOf(QLatin1Char('('));
		return (split > 0 ? target.left(split) : target).trimmed().toLower();
	}

	void suggestSourceMappings(bool userRequested)
	{
		std::vector<QString> videoCandidates;
		std::vector<QString> audioCandidates;
		std::vector<QString> micCandidates;
		for (int row = 1; row < sourceCombos_[0]->count(); ++row) {
			const QString name = sourceCombos_[0]->itemData(row).toString();
			if (name.isEmpty())
				continue;
			obs_source_t *source = obs_get_source_by_name(name.toUtf8().constData());
			if (!source)
				continue;
			const uint32_t flags = obs_source_get_output_flags(source);
			obs_source_release(source);
			const QString lower = name.toLower();
			if (flags & OBS_SOURCE_VIDEO)
				videoCandidates.push_back(name);
			if (flags & OBS_SOURCE_AUDIO)
				audioCandidates.push_back(name);
			if (lower.contains(QStringLiteral("mic")) || lower.contains(QStringLiteral("microphone")))
				micCandidates.push_back(name);
		}

		auto firstMatching = [](const std::vector<QString> &items, std::initializer_list<const char *> terms,
					     const QString &exclude = QString()) {
			for (const QString &item : items) {
				if (item == exclude)
					continue;
				const QString lower = item.toLower();
				for (const char *term : terms) {
					if (lower.contains(QString::fromUtf8(term)))
						return item;
				}
			}
			for (const QString &item : items)
				if (item != exclude)
					return item;
			return QString();
		};

		const QString video = firstMatching(videoCandidates, {"video", "ndi", "game"});
		const QString mic = firstMatching(micCandidates, {"mic", "microphone"});
		std::vector<QString> desktopCandidates;
		for (const QString &candidate : audioCandidates) {
			if (candidate != mic)
				desktopCandidates.push_back(candidate);
		}
		QString desktop = firstMatching(desktopCandidates, {"desktop", "game", "system", "audio"});
		if (desktop == video && desktopCandidates.size() > 1)
			desktop = firstMatching(desktopCandidates, {"desktop", "audio", "game"}, video);

		auto applySuggestion = [this](size_t index, const QString &name) {
			if (name.isEmpty())
				return;
			const int comboIndex = sourceCombos_[index]->findData(name);
			if (comboIndex < 0)
				return;
			{
				QSignalBlocker blocker(sourceCombos_[index]);
				sourceCombos_[index]->setCurrentIndex(comboIndex);
			}
			bindSource(index, name);
		};
		applySuggestion(0, video);
		applySuggestion(1, desktop);
		applySuggestion(2, mic);
		setupConfirmed_ = false;
		setupConfirmedAtomic_.store(false);

		const QString desktopHost = senderHostForSource(desktop);
		const QString micHost = senderHostForSource(mic);
		const bool micLinkedSuggestion = !desktopHost.isEmpty() && desktopHost == micHost;
		if (micLinkedSuggestion) {
			QSignalBlocker blocker(softSyncLinkMic_);
			softSyncLinkMic_->setChecked(true);
		}
		updateEngineSettings();
		syncSoftSyncFilters();
		saveConfig();
		showNotification(video.isEmpty() || desktop.isEmpty()
			? QStringLiteral("Source detection needs help: select video and desktop audio, then confirm setup.")
			: QStringLiteral("Sources detected%1. Check the assignments once, then confirm setup.")
				.arg(micLinkedSuggestion ? QStringLiteral("; mic appears to share the sender clock and was linked") : QString()),
			video.isEmpty() || desktop.isEmpty());
		if (userRequested)
			appendEvent(QStringLiteral("Automatic source-role detection completed"), false);
	}

	void runProtectionTest()
	{
		if (!setupConfirmed_) {
			showNotification(QStringLiteral("Confirm the source assignments before testing protection."), true);
			return;
		}
		if (recovery_.active || backgroundRecovery_.active || anyResetInProgress()) {
			showNotification(QStringLiteral("Protection test is waiting for the current recovery to finish."), true);
			return;
		}
		if (!sourceMapped(0) || !sourceMapped(1)) {
			showNotification(QStringLiteral("Protection test needs mapped video and desktop-audio sources."), true);
			return;
		}
		if (outputActive()) {
			const auto choice = QMessageBox::warning(panel_, QStringLiteral("Run protection test during output?"),
				QStringLiteral("Streaming or recording is active. The test briefly rebuilds the mapped DistroAV receivers. Run it anyway?"),
				QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
			if (choice != QMessageBox::Yes)
				return;
		}
		captureSnapshotState();
		if (!resetTarget(RecoveryTarget::EntireGroup)) {
			showNotification(QStringLiteral("Protection test could not safely rebuild the mapped receivers."), true);
			return;
		}
		const uint64_t now = os_gettime_ns();
		recovery_.active = true;
		recovery_.target = RecoveryTarget::EntireGroup;
		recovery_.issue = IssueKind::None;
		recovery_.reason = QStringLiteral("user-requested protection self-test");
		recovery_.verifyAtNs = now + static_cast<uint64_t>(engineSettings_.pulseDurationMs.load()) * kNsPerMs +
			static_cast<uint64_t>(engineSettings_.verifyDelaySec.load()) * kNsPerSecond;
		quarantineUntilNs_.store(recovery_.verifyAtNs + kIncidentQuarantineNs);
		showNotification(QStringLiteral("Protection test running; receiver settings will be restored and verified automatically."), false);
		appendEvent(QStringLiteral("Protection self-test started"), false);
	}

	void exportSupportReport()
	{
		QJsonObject report;
		report.insert(QStringLiteral("plugin_version"), QStringLiteral(PLUGIN_VERSION));
		report.insert(QStringLiteral("created"), QDateTime::currentDateTime().toString(Qt::ISODate));
		report.insert(QStringLiteral("automatic_protection"), automaticProtection_->isChecked());
		report.insert(QStringLiteral("setup_confirmed"), setupConfirmed_);
		report.insert(QStringLiteral("measurement"), reliabilityName(static_cast<MeasurementReliability>(measurementReliability_.load())));
		report.insert(QStringLiteral("raw_offset_ms"), rawOffsetMs_.load());
		report.insert(QStringLiteral("filtered_offset_ms"), filteredOffsetMs_.load());
		report.insert(QStringLiteral("baseline_offset_ms"), baselineOffsetMs_.load());
		report.insert(QStringLiteral("drift_rate_ms_per_min"), driftRateMsPerMinute_.load());
		report.insert(QStringLiteral("controller_ppm"), states_[1].softSync.correctionPpm.load());
		report.insert(QStringLiteral("video_threshold_ms"), effectiveStallThresholdMs(0));
		report.insert(QStringLiteral("audio_threshold_ms"), effectiveStallThresholdMs(1));
		report.insert(QStringLiteral("event_history"), eventLog_ ? eventLog_->toPlainText() : QString());
		QJsonArray measurements;
		for (const auto &sample : diagnosticHistory_) {
			QJsonObject point;
			point.insert(QStringLiteral("time"), sample.time);
			point.insert(QStringLiteral("raw_offset_ms"), sample.rawOffsetMs);
			point.insert(QStringLiteral("filtered_offset_ms"), sample.filteredOffsetMs);
			point.insert(QStringLiteral("corrected_drift_ms"), sample.correctedDriftMs);
			point.insert(QStringLiteral("rate_ms_per_min"), sample.driftRateMsPerMinute);
			point.insert(QStringLiteral("video_age_ms"), sample.videoAgeMs);
			point.insert(QStringLiteral("desktop_age_ms"), sample.desktopAgeMs);
			point.insert(QStringLiteral("mic_age_ms"), sample.micAgeMs);
			measurements.append(point);
		}
		report.insert(QStringLiteral("recent_measurements"), measurements);
		QJsonArray sources;
		for (const auto &state : states_) {
			QJsonObject source;
			source.insert(QStringLiteral("role"), state.role);
			source.insert(QStringLiteral("mapped"), !state.sourceName.isEmpty());
			source.insert(QStringLiteral("active"), state.active.load());
			source.insert(QStringLiteral("jump_count"), static_cast<qint64>(state.videoJumpCount.load() + state.audioJumpCount.load()));
			source.insert(QStringLiteral("reset_count"), static_cast<qint64>(state.resetCount.load()));
			sources.append(source);
		}
		report.insert(QStringLiteral("sources"), sources);
		char *rawPath = obs_module_config_path(QStringLiteral("sync-guardian-support-%1.json")
								.arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")))
								.toUtf8().constData());
		if (!rawPath) {
			showNotification(QStringLiteral("Support report could not be created."), true);
			return;
		}
		const QString path = QString::fromUtf8(rawPath);
		bfree(rawPath);
		QFile file(path);
		if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
			showNotification(QStringLiteral("Support report could not be written."), true);
			return;
		}
		file.write(QJsonDocument(report).toJson(QJsonDocument::Indented));
		file.close();
		showNotification(QStringLiteral("Support report saved."), false);
		QMessageBox::information(panel_, QStringLiteral("Sync Guardian support report"),
					 QStringLiteral("Report saved to:\n%1").arg(path));
	}

	QPushButton *buttonForTarget(RecoveryTarget target) const
	{
		switch (target) {
		case RecoveryTarget::Video:
			return resetVideoButton_;
		case RecoveryTarget::DesktopAudio:
			return resetDesktopButton_;
		case RecoveryTarget::Mic:
			return resetMicButton_;
		case RecoveryTarget::BothAudio:
			return resetBothAudioButton_;
		case RecoveryTarget::EntireGroup:
			return rebuildGroupButton_;
		default:
			return nullptr;
		}
	}

	void clearSuggestedRecovery()
	{
		const std::array<QPushButton *, 5> buttons = {resetVideoButton_, resetDesktopButton_, resetMicButton_,
							       resetBothAudioButton_, rebuildGroupButton_};
		for (QPushButton *button : buttons) {
			if (button)
				button->setStyleSheet(QString());
		}
		suggestedTarget_ = RecoveryTarget::None;
		if (fixNowButton_)
			fixNowButton_->setEnabled(false);
		if (manualSuggestionLabel_) {
			manualSuggestionLabel_->setStyleSheet(QString());
			manualSuggestionLabel_->setText(QStringLiteral("Suggested manual action: none — monitoring healthy. Hover over a button for help."));
		}
	}

	void showSuggestedRecovery(RecoveryTarget target, const QString &reason, int confidence)
	{
		const std::array<QPushButton *, 5> buttons = {resetVideoButton_, resetDesktopButton_, resetMicButton_,
							       resetBothAudioButton_, rebuildGroupButton_};
		for (QPushButton *button : buttons) {
			if (button)
				button->setStyleSheet(QString());
		}

		suggestedTarget_ = target;
		if (fixNowButton_) {
			fixNowButton_->setEnabled(target != RecoveryTarget::None);
			fixNowButton_->setText(target == RecoveryTarget::None ? QStringLiteral("Fix now")
									 : QStringLiteral("Fix %1").arg(targetName(target)));
		}
		QPushButton *suggested = buttonForTarget(target);
		if (suggested) {
			suggested->setStyleSheet(QStringLiteral(
				"QPushButton { background-color: #2e7d32; color: white; font-weight: 600; border: 1px solid #66bb6a; }"
				"QPushButton:hover { background-color: #388e3c; }"
				"QPushButton:pressed { background-color: #1b5e20; }"));
		}
		if (manualSuggestionLabel_) {
			manualSuggestionLabel_->setStyleSheet(QStringLiteral("font-weight: 600;"));
			Q_UNUSED(confidence);
			manualSuggestionLabel_->setText(
				QStringLiteral("Suggested manual action: %1\n%2")
					.arg(suggested ? suggested->text() : targetName(target), reason));
		}
	}

	void setSummaryIssue(IssueKind issue, RecoveryTarget target)
	{
		if (issue == IssueKind::None) {
			activeSummaryIssue_ = IssueKind::None;
			return;
		}

		if (activeSummaryIssue_ != issue) {
			activeSummaryIssue_ = issue;
			const uint64_t now = os_gettime_ns();
			const bool mergedRepeat = lastCountedIssue_ == issue && lastCountedIssueNs_ &&
						 now - lastCountedIssueNs_ < kIssueEpisodeMergeNs;
			if (!mergedRepeat) {
				const size_t index = static_cast<size_t>(issue);
				if (index < issueEpisodeCounts_.size())
					issueEpisodeCounts_[index]++;
				lastCountedIssue_ = issue;
				lastCountedIssueNs_ = now;
			}
			lastIssueSummary_ = QStringLiteral("%1; suggested %2")
					    .arg(issueSummaryName(issue), targetName(target));
			lastIssueTime_ = QDateTime::currentDateTime();
		}
	}

	uint64_t totalIssueEpisodes() const
	{
		uint64_t total = 0;
		for (size_t i = 1; i < issueEpisodeCounts_.size(); ++i)
			total += issueEpisodeCounts_[i];
		return total;
	}

	uint64_t totalTimestampJumps() const
	{
		return states_[0].videoJumpCount.load() + states_[1].audioJumpCount.load() + states_[2].audioJumpCount.load();
	}

	uint64_t totalResetPulses() const
	{
		return states_[0].resetCount.load() + states_[1].resetCount.load() + states_[2].resetCount.load();
	}

	void updateOverallSummary()
	{
		if (!overallSummaryLabel_)
			return;

		const uint64_t issues = totalIssueEpisodes();
		const uint64_t jumps = totalTimestampJumps();
		const uint64_t resets = totalResetPulses();
		const uint64_t recoveries = verifiedRecoveryCount_.load();
		const uint64_t failed = failedRecoveryCount_.load();
		QString history;
		if (issues == 0 && jumps == 0 && resets == 0 && recoveries == 0 && failed == 0) {
			history = QStringLiteral("no sync events this session");
		} else {
			history = QStringLiteral("session: %1 issue%2, %3 jump%4, %5 reset%6, %7 recover%8")
					  .arg(static_cast<unsigned long long>(issues))
					  .arg(issues == 1 ? QString() : QStringLiteral("s"))
					  .arg(static_cast<unsigned long long>(jumps))
					  .arg(jumps == 1 ? QString() : QStringLiteral("s"))
					  .arg(static_cast<unsigned long long>(resets))
					  .arg(resets == 1 ? QString() : QStringLiteral("s"))
					  .arg(static_cast<unsigned long long>(recoveries))
					  .arg(recoveries == 1 ? QStringLiteral("y") : QStringLiteral("ies"));
			if (failed > 0)
				history = QStringLiteral("%1, %2 failed verification%3")
						  .arg(history)
						  .arg(static_cast<unsigned long long>(failed))
						  .arg(failed == 1 ? QString() : QStringLiteral("s"));
		}

		QString text = QStringLiteral("Summary: %1; %2").arg(currentSummaryState_, history);
		if (activeSummaryIssue_ == IssueKind::None && !lastIssueSummary_.isEmpty())
			text = QStringLiteral("%1. Last: %2 at %3")
				       .arg(text)
				       .arg(lastIssueSummary_)
				       .arg(lastIssueTime_.toString(QStringLiteral("h:mm:ss AP")));
		overallSummaryLabel_->setText(text);
	}

	void connectSettingsSignals()
	{
		auto save = [this]() {
			updateEngineSettings();
			saveConfig();
		};
		QObject::connect(automationMode_, QOverload<int>::of(&QComboBox::currentIndexChanged), [this, save](int) {
			{
				QSignalBlocker blocker(automaticProtection_);
				automaticProtection_->setChecked(currentMode() == AutomationMode::Automatic);
			}
			save();
			appendEvent(QStringLiteral("Automation mode changed to %1").arg(modeName(currentMode())), false);
		});
		QObject::connect(automaticProtection_, &QCheckBox::toggled, [this](bool enabled) {
			setComboDataBlocked(automationMode_, static_cast<int>(enabled ? AutomationMode::Automatic : AutomationMode::Observe));
			if (enabled) {
				QSignalBlocker blocker(enableSoftSync_);
				enableSoftSync_->setChecked(true);
			}
			updateEngineSettings();
			syncSoftSyncFilters();
			saveConfig();
			showNotification(enabled ? QStringLiteral("Automatic Protection enabled")
							 : QStringLiteral("Automatic Protection paused; monitoring remains active"), false);
		});
		QObject::connect(autoTuneThresholds_, &QCheckBox::toggled, save);
		const std::array<QCheckBox *, 8> checks = {onlyWhenOutputActive_, requireActiveSources_,
							 enableFreezeDetection_, enableDriftDetection_, autoEscalate_,
							 chapterMarkers_, jsonLogging_, incidentReports_};
		for (QCheckBox *check : checks)
			QObject::connect(check, &QCheckBox::toggled, save);
		const std::array<QSpinBox *, 12> spins = {videoStallMs_, audioStallMs_, driftThresholdMs_,
							 driftPersistenceMs_, cooldownSec_, maxAutoResetsPerHour_,
							 startupGraceSec_, verifyDelaySec_, pulseDurationMs_,
							 softSyncDeadZoneMs_, softSyncMaxPpm_, softSyncSlewPpmPerSec_};
		for (QSpinBox *spin : spins)
			QObject::connect(spin, QOverload<int>::of(&QSpinBox::valueChanged), save);

		QObject::connect(enableSoftSync_, &QCheckBox::toggled, [this, save](bool enabled) {
			save();
			syncSoftSyncFilters();
			appendEvent(enabled ? QStringLiteral("Adaptive Soft Sync enabled; controller will wait for a stable long-term trend")
					    : QStringLiteral("Adaptive Soft Sync disabled; audio returned to untouched pass-through"),
				    false);
		});
		QObject::connect(softSyncLinkMic_, &QCheckBox::toggled, [this, save](bool) {
			save();
			syncSoftSyncFilters();
		});
	}

	void refreshSourceLists()
	{
		QStringList names;
		obs_enum_sources(
			[](void *param, obs_source_t *source) {
				auto *list = static_cast<QStringList *>(param);
				const char *id = obs_source_get_unversioned_id(source);
				if (id && strcmp(id, kDistroAvSourceId) == 0)
					list->append(QString::fromUtf8(obs_source_get_name(source)));
				return true;
			},
			&names);
		names.removeDuplicates();
		names.sort(Qt::CaseInsensitive);

		for (size_t i = 0; i < sourceCombos_.size(); ++i) {
			const QString current = sourceCombos_[i]->currentData().toString();
			QSignalBlocker blocker(sourceCombos_[i]);
			sourceCombos_[i]->clear();
			sourceCombos_[i]->addItem(QStringLiteral("(Not selected)"), QString());
			for (const QString &name : names)
				sourceCombos_[i]->addItem(name, name);
			int index = sourceCombos_[i]->findData(current);
			if (index < 0 && !current.isEmpty()) {
				sourceCombos_[i]->addItem(QStringLiteral("%1 (missing)").arg(current), current);
				index = sourceCombos_[i]->count() - 1;
			}
			if (index >= 0)
				sourceCombos_[i]->setCurrentIndex(index);
		}
	}

	obs_data_t *loadConfig() const
	{
		char *path = obs_module_config_path("sync-guardian.json");
		if (!path)
			return nullptr;
		obs_data_t *data = obs_data_create_from_json_file_safe(path, ".bak");
		bfree(path);
		return data;
	}

	void loadConfiguration()
	{
		loadingConfig_ = true;
		obs_data_t *config = loadConfig();
		firstRun_ = config == nullptr;
		bool migrated = false;
		const std::array<QString, 3> defaults = {QStringLiteral("NDI Video"), QStringLiteral("NDI Desktop Audio"),
							  QStringLiteral("NDI MIC only")};
		const std::array<const char *, 3> sourceKeys = {"video_source", "desktop_source", "mic_source"};

		for (size_t i = 0; i < sourceCombos_.size(); ++i) {
			QString desired;
			if (config)
				desired = QString::fromUtf8(obs_data_get_string(config, sourceKeys[i]));
			if (desired.isEmpty())
				desired = defaults[i];
			QSignalBlocker blocker(sourceCombos_[i]);
			const int index = sourceCombos_[i]->findData(desired);
			if (index >= 0)
				sourceCombos_[i]->setCurrentIndex(index);
		}

		if (config) {
			const long long configVersion = obs_data_has_user_value(config, "config_version")
				? obs_data_get_int(config, "config_version") : 3;
			migrated = configVersion < 5;
			if (obs_data_has_user_value(config, "automation_mode"))
				setComboDataBlocked(automationMode_, static_cast<int>(obs_data_get_int(config, "automation_mode")));
			loadCheckIfPresent(config, "only_when_output_active", onlyWhenOutputActive_);
			loadCheckIfPresent(config, "require_active_sources", requireActiveSources_);
			loadCheckIfPresent(config, "enable_freeze_detection", enableFreezeDetection_);
			loadCheckIfPresent(config, "enable_drift_detection", enableDriftDetection_);
			loadCheckIfPresent(config, "auto_escalate", autoEscalate_);
			loadCheckIfPresent(config, "chapter_markers", chapterMarkers_);
			loadCheckIfPresent(config, "json_logging", jsonLogging_);
			loadCheckIfPresent(config, "incident_reports", incidentReports_);
			loadCheckIfPresent(config, "soft_sync_enabled", enableSoftSync_);
			loadCheckIfPresent(config, "soft_sync_link_mic", softSyncLinkMic_);
			loadCheckIfPresent(config, "auto_tune_thresholds", autoTuneThresholds_);
			setupConfirmed_ = obs_data_has_user_value(config, "setup_confirmed") &&
				obs_data_get_bool(config, "setup_confirmed");
			if (obs_data_has_user_value(config, "advanced_settings_visible")) {
				const bool visible = obs_data_get_bool(config, "advanced_settings_visible");
				{
					QSignalBlocker blocker(advancedToggleButton_);
					advancedToggleButton_->setChecked(visible);
				}
				advancedSettingsWidget_->setVisible(visible);
				advancedToggleButton_->setText(visible ? QStringLiteral("Hide advanced detection settings")
								 : QStringLiteral("Show advanced detection settings"));
			}
			setSpinBlocked(videoStallMs_, obs_data_get_int(config, "video_stall_ms"));
			setSpinBlocked(audioStallMs_, obs_data_get_int(config, "audio_stall_ms"));
			setSpinBlocked(driftThresholdMs_, obs_data_get_int(config, "drift_threshold_ms"));
			setSpinBlocked(driftPersistenceMs_, obs_data_get_int(config, "drift_persistence_ms"));
			setSpinBlocked(cooldownSec_, obs_data_get_int(config, "cooldown_sec"));
			setSpinBlocked(maxAutoResetsPerHour_, obs_data_get_int(config, "max_auto_resets_per_hour"));
			setSpinBlocked(startupGraceSec_, obs_data_get_int(config, "startup_grace_sec"));
			setSpinBlocked(verifyDelaySec_, obs_data_get_int(config, "verify_delay_sec"));
			setSpinBlocked(pulseDurationMs_, obs_data_get_int(config, "pulse_duration_ms"));
			setSpinBlocked(softSyncDeadZoneMs_, obs_data_get_int(config, "soft_sync_dead_zone_ms"));
			setSpinBlocked(softSyncMaxPpm_, obs_data_get_int(config, "soft_sync_max_ppm"));
			setSpinBlocked(softSyncSlewPpmPerSec_, obs_data_get_int(config, "soft_sync_slew_ppm_per_sec"));
			if (obs_data_has_user_value(config, "baseline_offset_ms"))
				baselineOffsetMs_.store(obs_data_get_double(config, "baseline_offset_ms"));
			obs_data_release(config);
		}
		if (firstRun_) {
			setComboDataBlocked(automationMode_, static_cast<int>(AutomationMode::Automatic));
			setCheckBlocked(automaticProtection_, true);
			setCheckBlocked(enableSoftSync_, true);
			setCheckBlocked(autoTuneThresholds_, true);
			setupConfirmed_ = false;
		} else {
			setCheckBlocked(automaticProtection_, currentMode() == AutomationMode::Automatic);
		}
		setupConfirmedAtomic_.store(setupConfirmed_);
		loadingConfig_ = false;
		if (firstRun_)
			suggestSourceMappings(false);
		if (migrated) {
			saveConfig();
			appendEvent(QStringLiteral("Migrated Sync Guardian settings to the v0.5 hands-free configuration model"), false);
		}
	}

	void setComboDataBlocked(QComboBox *combo, int data)
	{
		QSignalBlocker blocker(combo);
		const int index = combo->findData(data);
		if (index >= 0)
			combo->setCurrentIndex(index);
	}

	void loadCheckIfPresent(obs_data_t *config, const char *key, QCheckBox *check)
	{
		if (obs_data_has_user_value(config, key))
			setCheckBlocked(check, obs_data_get_bool(config, key));
	}

	void setCheckBlocked(QCheckBox *check, bool value)
	{
		QSignalBlocker blocker(check);
		check->setChecked(value);
	}

	void setSpinBlocked(QSpinBox *spin, long long value)
	{
		if (value <= 0)
			return;
		QSignalBlocker blocker(spin);
		spin->setValue(static_cast<int>(value));
	}

	void saveConfig() const
	{
		if (!panel_ || loadingConfig_)
			return;
		obs_data_t *data = obs_data_create();
		obs_data_set_int(data, "config_version", 5);
		obs_data_set_string(data, "video_source", sourceCombos_[0]->currentData().toString().toUtf8().constData());
		obs_data_set_string(data, "desktop_source", sourceCombos_[1]->currentData().toString().toUtf8().constData());
		obs_data_set_string(data, "mic_source", sourceCombos_[2]->currentData().toString().toUtf8().constData());
		obs_data_set_int(data, "automation_mode", static_cast<long long>(currentMode()));
		obs_data_set_bool(data, "only_when_output_active", onlyWhenOutputActive_->isChecked());
		obs_data_set_bool(data, "require_active_sources", requireActiveSources_->isChecked());
		obs_data_set_bool(data, "enable_freeze_detection", enableFreezeDetection_->isChecked());
		obs_data_set_bool(data, "enable_drift_detection", enableDriftDetection_->isChecked());
		obs_data_set_bool(data, "auto_escalate", autoEscalate_->isChecked());
		obs_data_set_bool(data, "chapter_markers", chapterMarkers_->isChecked());
		obs_data_set_bool(data, "json_logging", jsonLogging_->isChecked());
		obs_data_set_bool(data, "incident_reports", incidentReports_->isChecked());
		obs_data_set_bool(data, "soft_sync_enabled", enableSoftSync_->isChecked());
		obs_data_set_bool(data, "soft_sync_link_mic", softSyncLinkMic_->isChecked());
		obs_data_set_bool(data, "auto_tune_thresholds", autoTuneThresholds_->isChecked());
		obs_data_set_bool(data, "setup_confirmed", setupConfirmed_);
		obs_data_set_bool(data, "advanced_settings_visible", advancedToggleButton_->isChecked());
		obs_data_set_int(data, "video_stall_ms", videoStallMs_->value());
		obs_data_set_int(data, "audio_stall_ms", audioStallMs_->value());
		obs_data_set_int(data, "drift_threshold_ms", driftThresholdMs_->value());
		obs_data_set_int(data, "drift_persistence_ms", driftPersistenceMs_->value());
		obs_data_set_int(data, "cooldown_sec", cooldownSec_->value());
		obs_data_set_int(data, "max_auto_resets_per_hour", maxAutoResetsPerHour_->value());
		obs_data_set_int(data, "startup_grace_sec", startupGraceSec_->value());
		obs_data_set_int(data, "verify_delay_sec", verifyDelaySec_->value());
		obs_data_set_int(data, "pulse_duration_ms", pulseDurationMs_->value());
		obs_data_set_int(data, "soft_sync_dead_zone_ms", softSyncDeadZoneMs_->value());
		obs_data_set_int(data, "soft_sync_max_ppm", softSyncMaxPpm_->value());
		obs_data_set_int(data, "soft_sync_slew_ppm_per_sec", softSyncSlewPpmPerSec_->value());
		if (std::isfinite(baselineOffsetMs_.load()))
			obs_data_set_double(data, "baseline_offset_ms", baselineOffsetMs_.load());
		char *path = obs_module_config_path("sync-guardian.json");
		if (path) {
			obs_data_save_json_safe(data, path, ".tmp", ".bak");
			bfree(path);
		}
		obs_data_release(data);
	}

	AutomationMode currentMode() const
	{
		return static_cast<AutomationMode>(automationMode_->currentData().toInt());
	}


	void updateEngineSettings()
	{
		if (!panel_)
			return;
		engineSettings_.automationMode.store(static_cast<int>(currentMode()));
		engineSettings_.onlyWhenOutputActive.store(onlyWhenOutputActive_->isChecked());
		engineSettings_.requireActiveSources.store(requireActiveSources_->isChecked());
		engineSettings_.enableFreezeDetection.store(enableFreezeDetection_->isChecked());
		engineSettings_.enableDriftDetection.store(enableDriftDetection_->isChecked());
		engineSettings_.autoEscalate.store(autoEscalate_->isChecked());
		engineSettings_.autoTuneThresholds.store(autoTuneThresholds_->isChecked());
		engineSettings_.videoStallMs.store(videoStallMs_->value());
		engineSettings_.audioStallMs.store(audioStallMs_->value());
		engineSettings_.driftThresholdMs.store(driftThresholdMs_->value());
		engineSettings_.driftPersistenceMs.store(driftPersistenceMs_->value());
		engineSettings_.cooldownSec.store(cooldownSec_->value());
		engineSettings_.maxAutoResetsPerHour.store(maxAutoResetsPerHour_->value());
		engineSettings_.startupGraceSec.store(startupGraceSec_->value());
		engineSettings_.verifyDelaySec.store(verifyDelaySec_->value());
		engineSettings_.pulseDurationMs.store(pulseDurationMs_->value());
		engineSettings_.softSyncEnabled.store(enableSoftSync_->isChecked());
		engineSettings_.softSyncLinkMic.store(softSyncLinkMic_->isChecked());
		engineSettings_.softSyncDeadZoneMs.store(softSyncDeadZoneMs_->value());
		engineSettings_.softSyncMaxPpm.store(softSyncMaxPpm_->value());
		engineSettings_.softSyncSlewPpmPerSec.store(softSyncSlewPpmPerSec_->value());
	}

	void queueBackgroundEvent(const QString &event)
	{
		std::lock_guard<std::mutex> lock(backgroundEventMutex_);
		if (backgroundEvents_.size() >= 100)
			backgroundEvents_.pop_front();
		backgroundEvents_.push_back(event);
	}

	void flushBackgroundEvents()
	{
		std::deque<QString> events;
		{
			std::lock_guard<std::mutex> lock(backgroundEventMutex_);
			events.swap(backgroundEvents_);
		}
		for (const QString &event : events)
			appendEvent(event, false);
	}

	bool sourceMapped(size_t index) const
	{
		obs_source_t *source = sourceForState(states_[index]);
		if (!source)
			return false;
		obs_source_release(source);
		return true;
	}

	void refreshEngineSourceStates()
	{
		for (auto &state : states_) {
			obs_source_t *source = sourceForState(state);
			if (!source) {
				state.enabled.store(false);
				state.active.store(false);
				state.showing.store(false);
				continue;
			}
			state.enabled.store(obs_source_enabled(source));
			state.active.store(obs_source_active(source));
			state.showing.store(obs_source_showing(source));
			obs_source_release(source);
		}
	}

	void restoreDueResets(uint64_t now)
	{
		std::vector<std::shared_ptr<ResetTicket>> due;
		{
			std::lock_guard<std::mutex> lock(resetMutex_);
			for (const auto &ticket : pendingResets_) {
				if (!ticket->restored.load() && now >= ticket->restoreAtNs)
					due.push_back(ticket);
			}
		}

		for (const auto &ticket : due) {
			const bool exactSettingsRestored = ticket->restore();
			if (!exactSettingsRestored) {
				queueBackgroundEvent(QStringLiteral("Warning: %1 reset completed, but one or more original DistroAV settings could not be verified")
							 .arg(ticket->role));
			} else {
				queueBackgroundEvent(QStringLiteral("%1 reset restored all saved source properties (FrameSync %2)")
							 .arg(ticket->role,
							      ticket->expectedFrameSync ? QStringLiteral("On") : QStringLiteral("Off")));
			}
		}

		if (!due.empty()) {
			std::lock_guard<std::mutex> lock(resetMutex_);
			pendingResets_.erase(
				std::remove_if(pendingResets_.begin(), pendingResets_.end(),
					       [](const std::shared_ptr<ResetTicket> &ticket) { return ticket->restored.load(); }),
				pendingResets_.end());
		}
	}

	void processDeferredSnapshotRebuild(uint64_t now)
	{
		uint64_t due = snapshotRebuildAtNs_.load();
		if (!due || now < due)
			return;
		if (!snapshotRebuildAtNs_.compare_exchange_strong(due, 0))
			return;
		const bool succeeded = resetTarget(RecoveryTarget::EntireGroup);
		snapshotRebuildResultReady_.store(true);
		queueBackgroundEvent(succeeded
			? QStringLiteral("Rebuilt NDI sync group after restoring the last known-good settings")
			: QStringLiteral("Could not rebuild the NDI sync group after restoring the last known-good settings"));
	}

	void sampleMeasurements(uint64_t now)
	{
		const double raw = calculateAvOffsetMs();
		rawOffsetMs_.store(raw);
		const double videoAge = packetAgeMs(0, now);
		const double desktopAge = packetAgeMs(1, now);
		const bool fresh = std::isfinite(videoAge) && std::isfinite(desktopAge) &&
			videoAge < static_cast<double>(effectiveStallThresholdMs(0)) &&
			desktopAge < static_cast<double>(effectiveStallThresholdMs(1));

		const bool jumpEvidence = recentJumpEvidence(now);
		if (jumpEvidence)
			quarantineUntilNs_.store(std::max(quarantineUntilNs_.load(), now + kIncidentQuarantineNs));
		const bool quarantined = now < quarantineUntilNs_.load();
		const bool resetActive = anyResetInProgress() || backgroundRecovery_.active;

		std::lock_guard<std::mutex> lock(measurementMutex_);
		if (std::isfinite(raw) && fresh && !resetActive && !quarantined) {
			offsetSamples_.push_back({now, raw});
			lastAcceptedSampleNs_.store(now);
		}
		while (!offsetSamples_.empty() && now >= offsetSamples_.front().wallNs &&
		       now - offsetSamples_.front().wallNs > kControllerRateWindowNs)
			offsetSamples_.pop_front();

		std::vector<double> recent;
		recent.reserve(48);
		for (const auto &sample : offsetSamples_) {
			if (now >= sample.wallNs && now - sample.wallNs <= kOffsetWindowNs)
				recent.push_back(sample.valueMs);
		}
		const double filtered = median(recent);
		filteredOffsetMs_.store(filtered);
		driftRateMsPerMinute_.store(robustSlopeMsPerMinute(offsetSamples_, now, kRateWindowNs));
		controllerRateMsPerMinute_.store(robustSlopeMsPerMinute(offsetSamples_, now, kControllerRateWindowNs));

		MeasurementReliability reliability = MeasurementReliability::Waiting;
		if (quarantined || resetActive || !fresh || !std::isfinite(raw)) {
			reliability = MeasurementReliability::Unreliable;
		} else if (!offsetSamples_.empty()) {
			const uint64_t span = now >= offsetSamples_.front().wallNs ? now - offsetSamples_.front().wallNs : 0;
			reliability = span >= kReliableMinimumSpanNs && recent.size() >= 24
				? MeasurementReliability::Reliable
				: MeasurementReliability::Calibrating;
		}
		measurementReliability_.store(static_cast<int>(reliability));

		if (std::isfinite(baselineOffsetMs_.load()) || now < monitoringGraceUntilAtomicNs_.load() ||
		    reliability != MeasurementReliability::Reliable || quarantined)
			return;
		std::vector<double> baselineValues;
		uint64_t oldest = now;
		for (const auto &sample : offsetSamples_) {
			if (now >= sample.wallNs && now - sample.wallNs <= kBaselineWindowNs) {
				baselineValues.push_back(sample.valueMs);
				oldest = std::min(oldest, sample.wallNs);
			}
		}
		if (baselineValues.size() < 80 || now < oldest || now - oldest < kStableCalibrationMinimumNs - 2ULL * kNsPerSecond)
			return;
		const auto minmax = std::minmax_element(baselineValues.begin(), baselineValues.end());
		const double range = *minmax.second - *minmax.first;
		const double allowedRange = std::max(25.0, static_cast<double>(engineSettings_.driftThresholdMs.load()) * 0.25);
		if (range > allowedRange)
			return;
		const double learned = median(std::move(baselineValues));
		baselineOffsetMs_.store(learned);
		queueBackgroundEvent(QStringLiteral("Automatic A/V baseline calibrated at %1 ms after stable, reliable timing")
					 .arg(learned, 0, 'f', 2));
	}

	void updateSoftSyncController(uint64_t now)
	{
		const bool enabled = engineSettings_.softSyncEnabled.load();
		SoftSyncControl &desktop = states_[1].softSync;
		SoftSyncControl &mic = states_[2].softSync;
		const auto reliability = static_cast<MeasurementReliability>(measurementReliability_.load());
		const uint64_t inputTs = desktop.lastInputTimestampNs.load();
		const uint64_t outputTs = desktop.lastOutputTimestampNs.load();
		const uint32_t sampleRate = std::max<uint32_t>(1, desktop.sampleRate.load());
		if (inputTs && outputTs && desktop.processedBlocks.load() >= 8) {
			const double observedTrimMs = nsToMs(signedDelta(outputTs, inputTs));
			const double expectedTrimMs = static_cast<double>(desktop.netFrameAdjustment.load()) * 1000.0 /
				static_cast<double>(sampleRate);
			const double validationError = observedTrimMs - expectedTrimMs;
			softSyncValidationErrorMs_.store(validationError);
			const uint32_t validationFailures = std::fabs(validationError) > 5.0
				? softSyncValidationFailures_.load() + 1 : 0;
			softSyncValidationFailures_.store(validationFailures);
			if (validationFailures == 8)
				queueBackgroundEvent(QStringLiteral("Drift Controller paused: output timing did not match the requested correction"));
		} else {
			softSyncValidationErrorMs_.store(std::numeric_limits<double>::quiet_NaN());
			softSyncValidationFailures_ = 0;
		}
		const bool incidentPause = now < quarantineUntilNs_.load() || anyResetInProgress() ||
			backgroundRecovery_.active || reliability != MeasurementReliability::Reliable ||
			softSyncValidationFailures_.load() >= 8 || !setupConfirmedAtomic_.load();
		if (!enabled || !desktop.enabled.load() || incidentPause) {
			driftControllerSaturated_.store(false);
			desktop.targetPpm.store(0.0);
			desktop.correctionPpm.store(0.0);
			mic.targetPpm.store(0.0);
			mic.correctionPpm.store(0.0);
			softSyncEstimatedDriftMs_.store(std::numeric_limits<double>::quiet_NaN());
			if (enabled && desktop.enabled.load() && incidentPause && !softSyncControllerPaused_) {
				desktop.generation.fetch_add(1);
				desktop.netFrameAdjustment.store(0);
				mic.generation.fetch_add(1);
				mic.netFrameAdjustment.store(0);
				softSyncControllerPaused_ = true;
			}
			lastSoftSyncControllerNs_ = now;
			return;
		}
		softSyncControllerPaused_ = false;

		double dtSeconds = static_cast<double>(kWatchdogSampleIntervalMs) / 1000.0;
		if (lastSoftSyncControllerNs_ && now > lastSoftSyncControllerNs_)
			dtSeconds = std::clamp(static_cast<double>(now - lastSoftSyncControllerNs_) /
						 static_cast<double>(kNsPerSecond),
					 0.01, 2.0);
		lastSoftSyncControllerNs_ = now;

		const double rawDrift = currentDriftMs();
		const double estimatedDrift = effectiveDriftMs();
		softSyncEstimatedDriftMs_.store(estimatedDrift);
		const double longRate = controllerRateMsPerMinute_.load();
		const int maxPpm = std::max(1, engineSettings_.softSyncMaxPpm.load());
		double target = 0.0;

		const bool timingHealthy = std::isfinite(packetAgeMs(0, now)) && std::isfinite(packetAgeMs(1, now)) &&
			packetAgeMs(0, now) < effectiveStallThresholdMs(0) &&
			packetAgeMs(1, now) < effectiveStallThresholdMs(1) && !anyResetInProgress();
		if (timingHealthy && std::isfinite(rawDrift) && std::isfinite(longRate)) {
			// Feed-forward term: a -1.5 ms/min raw drift needs roughly +25 ppm
			// to stop further separation.
			target = -longRate * kPpmPerMsPerMinute;

			// Smooth catch-up curve for existing corrected error. Close to the
			// dead zone this stays gentle; larger errors progressively unlock more
			// of the configured ceiling. Full-scale catch-up is reached at about
			// 250 ms outside the dead zone. This avoids a hard step while still
			// allowing rapid recovery when the user explicitly raises the cap.
			const double deadZone = static_cast<double>(engineSettings_.softSyncDeadZoneMs.load());
			if (std::isfinite(estimatedDrift) && std::fabs(estimatedDrift) > deadZone) {
				const double outside = std::fabs(estimatedDrift) - deadZone;
				const double normalized = std::clamp(outside / 250.0, 0.0, 1.0);
				const double curved = normalized * std::sqrt(normalized); // x^1.5 soft knee
				const double catchUpPpm = static_cast<double>(maxPpm) * curved;
				target += -std::copysign(catchUpPpm, estimatedDrift);
			}
		}
		target = std::clamp(target, -static_cast<double>(maxPpm), static_cast<double>(maxPpm));
		driftControllerSaturated_.store(std::fabs(target) >= static_cast<double>(maxPpm) * 0.99 &&
			std::isfinite(estimatedDrift) && std::fabs(estimatedDrift) > engineSettings_.softSyncDeadZoneMs.load());
		desktop.targetPpm.store(target);

		const double current = desktop.correctionPpm.load();
		const double configuredSlew = static_cast<double>(std::max(1, engineSettings_.softSyncSlewPpmPerSec.load()));
		// Use the configured slew while accelerating away from neutral. Allow
		// twice that rate when braking toward zero or reversing direction so a
		// temporary catch-up command does not linger and overshoot.
		const bool braking = (std::fabs(target) < std::fabs(current)) || (target * current < 0.0);
		const double maxStep = configuredSlew * (braking ? 2.0 : 1.0) * dtSeconds;
		const double next = current + std::clamp(target - current, -maxStep, maxStep);
		desktop.correctionPpm.store(next);
		desktop.enabled.store(true);

		if (engineSettings_.softSyncLinkMic.load() && mic.enabled.load()) {
			mic.targetPpm.store(target);
			mic.correctionPpm.store(next);
		} else {
			mic.targetPpm.store(0.0);
			mic.correctionPpm.store(0.0);
		}
	}

	void updateSoftSyncStatusLabel()
	{
		if (!softSyncStatusLabel_)
			return;
		if (!engineSettings_.softSyncEnabled.load()) {
			softSyncStatusLabel_->setText(QStringLiteral("Soft Sync: Off — audio passes through unchanged"));
			return;
		}
		if (!states_[1].softSync.enabled.load()) {
			softSyncStatusLabel_->setText(QStringLiteral("Soft Sync: Enabled, waiting for the mapped desktop-audio source/filter"));
			return;
		}
		if (softSyncValidationFailures_.load() >= 8) {
			softSyncStatusLabel_->setText(QStringLiteral("Drift Controller: paused — correction output validation failed"));
			return;
		}
		if (driftControllerSaturated_.load()) {
			softSyncStatusLabel_->setText(QStringLiteral("Drift exceeds correction range — controller is at the configured limit"));
			return;
		}

		const double ppm = states_[1].softSync.correctionPpm.load();
		const double rate = controllerRateMsPerMinute_.load();
		const double delay = softSyncAccumulatedDelayMs();
		const double estimated = softSyncEstimatedDriftMs_.load();
		const double validationError = softSyncValidationErrorMs_.load();
		QString action;
		if (!std::isfinite(rate))
			action = QStringLiteral("learning trend");
		else if (std::fabs(ppm) < 0.25)
			action = QStringLiteral("neutral");
		else if (ppm > 0.0)
			action = QStringLiteral("slowing audio");
		else
			action = QStringLiteral("speeding audio");
		QString correctedText = std::isfinite(estimated)
			? QStringLiteral(" · corrected drift %1 ms").arg(estimated, 0, 'f', 1)
			: QString();
		softSyncStatusLabel_->setText(
			QStringLiteral("Drift Controller: %1 · %2 ppm · trim %3 ms%4%5%6")
				.arg(action)
				.arg(ppm, 0, 'f', 1)
				.arg(delay, 0, 'f', 1)
				.arg(correctedText)
				.arg(engineSettings_.softSyncLinkMic.load() ? QStringLiteral(" · mic linked") : QString())
				.arg(std::isfinite(validationError) ? QStringLiteral(" · verified ±%1 ms").arg(std::fabs(validationError), 0, 'f', 1)
												 : QStringLiteral(" · validation settling")));
	}

	static void updateEngineConditionTimer(bool condition, uint64_t &since, uint64_t now)
	{
		if (condition) {
			if (!since)
				since = now;
		} else {
			since = 0;
		}
	}

	void resetEngineConditionTimers()
	{
		engineVideoStallSinceNs_ = 0;
		engineDesktopStallSinceNs_ = 0;
		engineMicStallSinceNs_ = 0;
		engineBothAudioStallSinceNs_ = 0;
		engineEntireGroupStallSinceNs_ = 0;
		engineDriftSinceNs_ = 0;
	}

	bool backgroundRecoverySucceeded(uint64_t now) const
	{
		const double videoAge = packetAgeMs(0, now);
		const double desktopAge = packetAgeMs(1, now);
		const double micAge = packetAgeMs(2, now);
		const bool videoFresh = std::isfinite(videoAge) &&
			videoAge < effectiveStallThresholdMs(0) * 0.75;
		const bool desktopFresh = std::isfinite(desktopAge) &&
			desktopAge < effectiveStallThresholdMs(1) * 0.75;
		const bool micFresh = std::isfinite(micAge) &&
			micAge < effectiveStallThresholdMs(2) * 0.75;
		switch (backgroundRecovery_.issue) {
		case IssueKind::EntireGroupStall:
			return videoFresh && desktopFresh && (!sourceMapped(2) || micFresh);
		case IssueKind::BothAudioStall:
			return desktopFresh && micFresh;
		case IssueKind::VideoStall:
		case IssueKind::VideoJump:
			return videoFresh;
		case IssueKind::DesktopAudioStall:
		case IssueKind::DesktopAudioJump:
			return desktopFresh;
		case IssueKind::MicStall:
		case IssueKind::MicJump:
			return micFresh;
		case IssueKind::PersistentDrift: {
			const double drift = driftForPersistentDetectionMs();
			if (!videoFresh || !desktopFresh || !std::isfinite(drift))
				return false;
			return std::fabs(drift) < engineSettings_.driftThresholdMs.load() * 0.75 ||
			       (std::isfinite(backgroundRecovery_.preDriftMs) &&
				std::fabs(drift) < std::fabs(backgroundRecovery_.preDriftMs) * 0.5);
		}
		default:
			return videoFresh && desktopFresh;
		}
	}

	void incrementBackgroundRecovery(RecoveryTarget target)
	{
		switch (target) {
		case RecoveryTarget::Video:
			states_[0].recoveryCount.fetch_add(1);
			break;
		case RecoveryTarget::DesktopAudio:
			states_[1].recoveryCount.fetch_add(1);
			break;
		case RecoveryTarget::Mic:
			states_[2].recoveryCount.fetch_add(1);
			break;
		case RecoveryTarget::BothAudio:
			states_[1].recoveryCount.fetch_add(1);
			states_[2].recoveryCount.fetch_add(1);
			break;
		case RecoveryTarget::EntireGroup:
			for (auto &state : states_)
				state.recoveryCount.fetch_add(1);
			break;
		default:
			break;
		}
	}

	bool backgroundResetAllowed(uint64_t now, QString &reason)
	{
		while (!engineResetTimes_.empty() && now - engineResetTimes_.front() > kResetLimitWindowNs)
			engineResetTimes_.pop_front();
		if (engineResetTimes_.size() >= static_cast<size_t>(engineSettings_.maxAutoResetsPerHour.load())) {
			reason = QStringLiteral("hourly reset safety limit reached");
			return false;
		}
		if (engineLastResetNs_ && now - engineLastResetNs_ <
			static_cast<uint64_t>(engineSettings_.cooldownSec.load()) * kNsPerSecond) {
			reason = QStringLiteral("automatic reset cooldown active");
			return false;
		}
		return true;
	}

	void beginBackgroundRecovery(uint64_t now, IssueKind issue, RecoveryTarget target, const QString &reason)
	{
		if (!resetTarget(target))
			return;
		engineResetTimes_.push_back(now);
		engineLastResetNs_ = now;
		backgroundRecovery_.active = true;
		backgroundRecovery_.automated = true;
		backgroundRecovery_.escalationUsed = false;
		backgroundRecovery_.target = target;
		backgroundRecovery_.issue = issue;
		backgroundRecovery_.reason = reason;
		backgroundRecovery_.preDriftMs = driftForPersistentDetectionMs();
		backgroundRecovery_.verifyAtNs = now +
			static_cast<uint64_t>(engineSettings_.pulseDurationMs.load()) * kNsPerMs +
			static_cast<uint64_t>(engineSettings_.verifyDelaySec.load()) * kNsPerSecond;
		engineDetectionSuppressedUntilNs_ = backgroundRecovery_.verifyAtNs;
		queueBackgroundEvent(QStringLiteral("Background watchdog reset %1: %2")
					 .arg(targetName(target), reason));
	}

	void verifyBackgroundRecovery(uint64_t now)
	{
		if (backgroundRecoverySucceeded(now)) {
			consecutiveRecoveryFailures_.store(0);
			incrementBackgroundRecovery(backgroundRecovery_.target);
			verifiedRecoveryCount_.fetch_add(1);
			queueBackgroundEvent(QStringLiteral("Background recovery verified for %1")
						 .arg(targetName(backgroundRecovery_.target)));
			queueUserNotification(QStringLiteral("%1 recovered automatically").arg(targetName(backgroundRecovery_.target)), false);
			backgroundRecovery_ = RecoveryAttempt{};
			engineDetectionSuppressedUntilNs_ = now + 2ULL * kNsPerSecond;
			resetEngineConditionTimers();
			return;
		}

		QString limitReason;
		const bool canEscalate = engineSettings_.autoEscalate.load() &&
			!backgroundRecovery_.escalationUsed && backgroundRecovery_.target != RecoveryTarget::EntireGroup &&
			backgroundResetAllowed(now, limitReason);
		if (canEscalate && resetTarget(RecoveryTarget::EntireGroup)) {
			engineResetTimes_.push_back(now);
			engineLastResetNs_ = now;
			backgroundRecovery_.escalationUsed = true;
			backgroundRecovery_.target = RecoveryTarget::EntireGroup;
			backgroundRecovery_.verifyAtNs = now +
				static_cast<uint64_t>(engineSettings_.pulseDurationMs.load()) * kNsPerMs +
				static_cast<uint64_t>(engineSettings_.verifyDelaySec.load()) * kNsPerSecond;
			engineDetectionSuppressedUntilNs_ = backgroundRecovery_.verifyAtNs;
			queueBackgroundEvent(QStringLiteral("Targeted recovery did not verify; background watchdog rebuilt the entire NDI group"));
			return;
		}

		failedRecoveryCount_.fetch_add(1);
		const uint32_t failures = consecutiveRecoveryFailures_.fetch_add(1) + 1;
		queueBackgroundEvent(QStringLiteral("Background recovery failed verification after %1; automatic actions entered cooldown")
					 .arg(targetName(backgroundRecovery_.target)));
		queueUserNotification(QStringLiteral("Automatic recovery could not verify the fix; protection is in cooldown."), true);
		backgroundRecovery_ = RecoveryAttempt{};
		engineDetectionSuppressedUntilNs_ = now +
			static_cast<uint64_t>(engineSettings_.cooldownSec.load()) * kNsPerSecond;
		resetEngineConditionTimers();
		if (failures >= 2) {
			engineSettings_.automationMode.store(static_cast<int>(AutomationMode::Observe));
			queueBackgroundEvent(QStringLiteral("Fail-safe activated after repeated failed recoveries; protection changed to Monitor"));
			QMetaObject::invokeMethod(lifetimeContext_, [this]() {
				setComboDataBlocked(automationMode_, static_cast<int>(AutomationMode::Observe));
				setCheckBlocked(automaticProtection_, false);
				saveConfig();
			}, Qt::QueuedConnection);
		}
	}

	void backgroundEvaluateAutomation(uint64_t now)
	{
		if (!setupConfirmedAtomic_.load()) {
			backgroundRecovery_ = RecoveryAttempt{};
			resetEngineConditionTimers();
			return;
		}
		if (static_cast<AutomationMode>(engineSettings_.automationMode.load()) != AutomationMode::Automatic) {
			backgroundRecovery_ = RecoveryAttempt{};
			resetEngineConditionTimers();
			return;
		}
		if (backgroundRecovery_.active) {
			if (now >= backgroundRecovery_.verifyAtNs)
				verifyBackgroundRecovery(now);
			return;
		}
		if (anyResetInProgress() || now < monitoringGraceUntilAtomicNs_.load() ||
		    now < engineDetectionSuppressedUntilNs_)
			return;

		outputActiveAtomic_.store(obs_frontend_streaming_active() || obs_frontend_recording_active());
		if (engineSettings_.onlyWhenOutputActive.load() && !outputActiveAtomic_.load()) {
			resetEngineConditionTimers();
			return;
		}
		if (engineSettings_.requireActiveSources.load() &&
		    !(states_[0].enabled.load() && states_[0].active.load() && states_[0].showing.load() &&
		      states_[1].enabled.load() && states_[1].active.load())) {
			resetEngineConditionTimers();
			return;
		}
		const uint64_t videoFirst = states_[0].firstValidSampleWallNs.load();
		const uint64_t desktopFirst = states_[1].firstValidSampleWallNs.load();
		if (!videoFirst || !desktopFirst)
			return;
		const uint64_t firstReady = std::max(videoFirst, desktopFirst);
		if (now < firstReady || now - firstReady < kFirstSampleGraceNs)
			return;

		const double videoAge = packetAgeMs(0, now);
		const double desktopAge = packetAgeMs(1, now);
		const double micAge = packetAgeMs(2, now);
		const int videoThreshold = effectiveStallThresholdMs(0);
		const int audioThreshold = effectiveStallThresholdMs(1);
		const bool videoFresh = std::isfinite(videoAge) && videoAge < videoThreshold;
		const bool desktopFresh = std::isfinite(desktopAge) && desktopAge < audioThreshold;
		const bool micMapped = sourceMapped(2) && states_[2].enabled.load() && states_[2].active.load();
		const uint64_t micFirst = states_[2].firstValidSampleWallNs.load();
		const bool micExpected = micMapped && micFirst && now >= micFirst && now - micFirst >= kFirstSampleGraceNs;
		const bool micStale = micExpected && (!std::isfinite(micAge) || micAge >= audioThreshold);
		const bool freeze = engineSettings_.enableFreezeDetection.load();

		updateEngineConditionTimer(freeze && !videoFresh && !desktopFresh && (!micExpected || micStale),
					 engineEntireGroupStallSinceNs_, now);
		updateEngineConditionTimer(freeze && videoFresh && micExpected && !desktopFresh && micStale,
					 engineBothAudioStallSinceNs_, now);
		updateEngineConditionTimer(freeze && desktopFresh && !videoFresh, engineVideoStallSinceNs_, now);
		updateEngineConditionTimer(freeze && videoFresh && !desktopFresh, engineDesktopStallSinceNs_, now);
		updateEngineConditionTimer(freeze && micStale && (videoFresh || desktopFresh), engineMicStallSinceNs_, now);

		// When Adaptive Soft Sync is active, the ordinary persistent-drift threshold
		// follows the estimated corrected output relationship. With Soft Sync off,
		// it follows the uncorrected transport drift exactly as before.
		const double drift = driftForPersistentDetectionMs();
		updateEngineConditionTimer(engineSettings_.enableDriftDetection.load() && videoFresh && desktopFresh &&
					 std::isfinite(drift) && std::fabs(drift) >= engineSettings_.driftThresholdMs.load(),
					 engineDriftSinceNs_, now);

		IssueKind issue = IssueKind::None;
		RecoveryTarget target = RecoveryTarget::None;
		QString reason;
		const double baseline = baselineOffsetMs_.load();
		const double rawIncidentDrift = std::isfinite(rawOffsetMs_.load()) && std::isfinite(baseline)
			? rawOffsetMs_.load() - baseline : std::numeric_limits<double>::quiet_NaN();
		const double jumpConfirmMs = std::max(50.0, engineSettings_.driftThresholdMs.load() * 0.5);
		const uint64_t videoJump = states_[0].lastVideoJumpWallNs.load();
		const uint64_t desktopJump = states_[1].lastAudioJumpWallNs.load();
		const uint64_t micJump = states_[2].lastAudioJumpWallNs.load();
		if (videoJump && now >= videoJump + kStallConfirmNs && now - videoJump <= kJumpEvidenceWindowNs &&
		    std::isfinite(rawIncidentDrift) && std::fabs(rawIncidentDrift) >= jumpConfirmMs) {
			issue = IssueKind::VideoJump;
			target = RecoveryTarget::Video;
			reason = QStringLiteral("video timestamp discontinuity produced a persistent %1 ms A/V jump").arg(rawIncidentDrift, 0, 'f', 1);
		} else if (desktopJump && now >= desktopJump + kStallConfirmNs && now - desktopJump <= kJumpEvidenceWindowNs &&
			   std::isfinite(rawIncidentDrift) && std::fabs(rawIncidentDrift) >= jumpConfirmMs) {
			issue = IssueKind::DesktopAudioJump;
			target = RecoveryTarget::DesktopAudio;
			reason = QStringLiteral("desktop-audio timestamp discontinuity produced a persistent %1 ms A/V jump").arg(rawIncidentDrift, 0, 'f', 1);
		} else if (micJump && now >= micJump + kStallConfirmNs && now - micJump <= kJumpEvidenceWindowNs) {
			issue = IssueKind::MicJump;
			target = RecoveryTarget::Mic;
			reason = QStringLiteral("microphone timestamp discontinuity persisted after confirmation");
		} else if (engineEntireGroupStallSinceNs_ && now - engineEntireGroupStallSinceNs_ >= kStallConfirmNs) {
			issue = IssueKind::EntireGroupStall;
			target = RecoveryTarget::EntireGroup;
			reason = QStringLiteral("all mapped NDI streams remained stale");
		} else if (engineBothAudioStallSinceNs_ && now - engineBothAudioStallSinceNs_ >= kStallConfirmNs) {
			issue = IssueKind::BothAudioStall;
			target = RecoveryTarget::BothAudio;
			reason = QStringLiteral("both NDI audio streams remained stale while video stayed fresh");
		} else if (engineVideoStallSinceNs_ && now - engineVideoStallSinceNs_ >= kStallConfirmNs) {
			issue = IssueKind::VideoStall;
			target = RecoveryTarget::Video;
			reason = QStringLiteral("video timestamp remained stale while desktop audio stayed fresh");
		} else if (engineDesktopStallSinceNs_ && now - engineDesktopStallSinceNs_ >= kStallConfirmNs) {
			issue = IssueKind::DesktopAudioStall;
			target = RecoveryTarget::DesktopAudio;
			reason = QStringLiteral("desktop-audio timestamp remained stale while video stayed fresh");
		} else if (engineMicStallSinceNs_ && now - engineMicStallSinceNs_ >= kStallConfirmNs) {
			issue = IssueKind::MicStall;
			target = RecoveryTarget::Mic;
			reason = QStringLiteral("microphone timestamp remained stale while companion streams stayed fresh");
		} else if (engineDriftSinceNs_ && now - engineDriftSinceNs_ >=
			   static_cast<uint64_t>(engineSettings_.driftPersistenceMs.load()) * kNsPerMs) {
			issue = IssueKind::PersistentDrift;
			target = drift < 0.0 ? RecoveryTarget::Video : RecoveryTarget::DesktopAudio;
			reason = softSyncCorrectionActive()
				? QStringLiteral("Soft Sync-corrected A/V drift remained at %1 ms beyond the configured threshold")
					  .arg(drift, 0, 'f', 1)
				: QStringLiteral("A/V drift remained at %1 ms beyond the configured threshold")
					  .arg(drift, 0, 'f', 1);
		}
		if (issue == IssueKind::None)
			return;
		if (issue == IssueKind::PersistentDrift) {
			monitorState_.store(static_cast<int>(MonitorState::Drifting));
			// Clock drift is a controller problem, not a receiver-lifecycle problem.
			// Never pulse or rebuild a receiver merely because a gradual slope persisted.
			return;
		}

		QString blocked;
		if (!backgroundResetAllowed(now, blocked))
			return;
		beginBackgroundRecovery(now, issue, target, reason);
		resetEngineConditionTimers();
	}

	void watchdogLoop()
	{
		while (!stopWatchdog_.load()) {
			const uint64_t now = os_gettime_ns();
			restoreDueResets(now);
			processDeferredSnapshotRebuild(now);
			refreshEngineSourceStates();
			sampleMeasurements(now);
			updateSoftSyncController(now);
			backgroundEvaluateAutomation(now);

			std::unique_lock<std::mutex> lock(watchdogWaitMutex_);
			watchdogCv_.wait_for(lock, std::chrono::milliseconds(kWatchdogSampleIntervalMs),
					      [this]() { return stopWatchdog_.load(); });
		}
		restoreDueResets(std::numeric_limits<uint64_t>::max());
	}

	void bindAllSources()
	{
		for (size_t i = 0; i < sourceCombos_.size(); ++i)
			bindSource(i, sourceCombos_[i]->currentData().toString());
	}

	void bindSource(size_t index, const QString &name)
	{
		SourceState &state = states_[index];
		// Always rebuild the binding when the source list is rescanned. This also
		// reattaches private probes/filters if OBS or DistroAV recreated the source.
		detachSource(state);
		state.sourceName = name;
		state.wasFresh.store(false);
		state.hasEverBeenFresh.store(false);
		state.lastAudioTimestampNs.store(0);
		state.lastAudioWallNs.store(0);
		state.lastAudioJumpWallNs.store(0);
		state.lastAudioJumpErrorNs.store(0);
		state.lastVideoTimestampNs.store(0);
		state.lastVideoWallNs.store(0);
		state.lastVideoArrivalWallNs.store(0);
		state.lastVideoJumpWallNs.store(0);
		state.lastVideoJumpErrorNs.store(0);
		state.firstValidSampleWallNs.store(0);
		state.observedHealthyGapNs.store(0);
		state.enabled.store(false);
		state.active.store(false);
		state.showing.store(false);
		state.softSync.netFrameAdjustment.store(0);
		state.softSync.generation.fetch_add(1);
		if (name.isEmpty())
			return;

		obs_source_t *source = obs_get_source_by_name(name.toUtf8().constData());
		if (!source)
			return;
		{
			std::lock_guard<std::mutex> lock(sourceMutex_);
			state.weak = obs_source_get_weak_source(source);
		}
		if (index == 0)
			attachVideoProbe(state, source);
		if (state.monitorAudio)
			obs_source_add_audio_capture_callback(source, audioCapture, &state);
		obs_source_release(source);

		if (state.monitorAudio)
			syncSoftSyncFilters();
	}

	void attachVideoProbe(SourceState &state, obs_source_t *source)
	{
		if (!source)
			return;

		obs_source_t *existing = obs_source_get_filter_by_name(source, kVideoProbeFilterName);
		if (existing) {
			const char *existingId = obs_source_get_unversioned_id(existing);
			if (existingId && strcmp(existingId, kVideoProbeFilterId) == 0)
				obs_source_filter_remove(source, existing);
			obs_source_release(existing);
		}

		const uint64_t token = g_nextVideoProbeToken.fetch_add(1);
		{
			std::lock_guard<std::mutex> lock(g_videoProbeRegistryMutex);
			g_videoProbeRegistry[token] = &state;
		}
		state.videoProbeToken = token;

		obs_data_t *settings = obs_data_create();
		obs_data_set_int(settings, kVideoProbeTokenKey, static_cast<long long>(token));
		obs_source_t *probe = obs_source_create_private(kVideoProbeFilterId, kVideoProbeFilterName, settings);
		obs_data_release(settings);
		if (!probe) {
			std::lock_guard<std::mutex> lock(g_videoProbeRegistryMutex);
			g_videoProbeRegistry.erase(token);
			state.videoProbeToken = 0;
			appendEvent(QStringLiteral("Video timestamp probe could not be created; video monitoring is unavailable"), false);
			return;
		}

		obs_source_filter_add(source, probe);
		state.videoProbeWeak = obs_source_get_weak_source(probe);
		obs_source_release(probe);
	}

	void attachSoftSyncFilter(SourceState &state, obs_source_t *source)
	{
		if (!source || !state.monitorAudio || state.softSyncFilterWeak)
			return;

		// Remove a stale private filter left by a previous plugin load before
		// attaching the new runtime-controlled instance.
		obs_source_t *existing = obs_source_get_filter_by_name(source, kSoftSyncFilterName);
		if (existing) {
			const char *existingId = obs_source_get_unversioned_id(existing);
			if (existingId && strcmp(existingId, kSoftSyncFilterId) == 0)
				obs_source_filter_remove(source, existing);
			obs_source_release(existing);
		}

		const uint64_t token = g_nextSoftSyncToken.fetch_add(1);
		{
			std::lock_guard<std::mutex> lock(g_softSyncRegistryMutex);
			g_softSyncRegistry[token] = &state.softSync;
		}
		state.softSyncToken = token;
		state.softSync.correctionPpm.store(0.0);
		state.softSync.targetPpm.store(0.0);
		state.softSync.netFrameAdjustment.store(0);
		state.softSync.lastInputTimestampNs.store(0);
		state.softSync.lastInputWallNs.store(0);
		state.softSync.lastOutputTimestampNs.store(0);
		state.softSync.lastOutputWallNs.store(0);
		state.softSync.generation.fetch_add(1);

		obs_data_t *settings = obs_data_create();
		obs_data_set_int(settings, kSoftSyncTokenKey, static_cast<long long>(token));
		obs_source_t *filter = obs_source_create_private(kSoftSyncFilterId, kSoftSyncFilterName, settings);
		obs_data_release(settings);
		if (!filter) {
			std::lock_guard<std::mutex> lock(g_softSyncRegistryMutex);
			g_softSyncRegistry.erase(token);
			state.softSyncToken = 0;
			state.softSync.enabled.store(false);
			appendEvent(QStringLiteral("Adaptive Soft Sync filter could not be created for %1").arg(state.role), false);
			return;
		}

		obs_source_filter_add(source, filter);
		state.softSyncFilterWeak = obs_source_get_weak_source(filter);
		state.softSync.enabled.store(true);
		obs_source_release(filter);
	}

	void detachSoftSyncFilter(SourceState &state, obs_source_t *source)
	{
		state.softSync.enabled.store(false);
		state.softSync.correctionPpm.store(0.0);
		state.softSync.targetPpm.store(0.0);
		state.softSync.netFrameAdjustment.store(0);
		state.softSync.lastInputTimestampNs.store(0);
		state.softSync.lastInputWallNs.store(0);
		state.softSync.lastOutputTimestampNs.store(0);
		state.softSync.lastOutputWallNs.store(0);
		state.softSync.generation.fetch_add(1);

		if (source && state.softSyncFilterWeak) {
			obs_source_t *filter = obs_weak_source_get_source(state.softSyncFilterWeak);
			if (filter) {
				obs_source_filter_remove(source, filter);
				obs_source_release(filter);
			}
		}
		if (state.softSyncFilterWeak) {
			obs_weak_source_release(state.softSyncFilterWeak);
			state.softSyncFilterWeak = nullptr;
		}
		if (state.softSyncToken) {
			std::lock_guard<std::mutex> lock(g_softSyncRegistryMutex);
			g_softSyncRegistry.erase(state.softSyncToken);
			state.softSyncToken = 0;
		}
	}

	void syncSoftSyncFilters()
	{
		const bool enabled = engineSettings_.softSyncEnabled.load();
		const bool linkMic = engineSettings_.softSyncLinkMic.load();
		for (size_t index = 1; index < states_.size(); ++index) {
			SourceState &state = states_[index];
			const bool desired = enabled && (index == 1 || linkMic);
			obs_source_t *source = sourceForState(state);
			if (desired && source)
				attachSoftSyncFilter(state, source);
			else if (!desired)
				detachSoftSyncFilter(state, source);
			if (source)
				obs_source_release(source);
		}
		if (!enabled)
			softSyncEstimatedDriftMs_.store(std::numeric_limits<double>::quiet_NaN());
	}

	void detachSource(SourceState &state)
	{
		obs_source_t *source = sourceForState(state);
		if (source) {
			detachSoftSyncFilter(state, source);
			if (state.videoProbeWeak) {
				obs_source_t *probe = obs_weak_source_get_source(state.videoProbeWeak);
				if (probe) {
					obs_source_filter_remove(source, probe);
					obs_source_release(probe);
				}
			}
			if (state.monitorAudio)
				obs_source_remove_audio_capture_callback(source, audioCapture, &state);
			obs_source_release(source);
		} else {
			detachSoftSyncFilter(state, nullptr);
		}

		if (state.videoProbeWeak) {
			obs_weak_source_release(state.videoProbeWeak);
			state.videoProbeWeak = nullptr;
		}
		if (state.videoProbeToken) {
			std::lock_guard<std::mutex> lock(g_videoProbeRegistryMutex);
			g_videoProbeRegistry.erase(state.videoProbeToken);
			state.videoProbeToken = 0;
		}
		{
			std::lock_guard<std::mutex> lock(sourceMutex_);
			if (state.weak) {
				obs_weak_source_release(state.weak);
				state.weak = nullptr;
			}
		}
	}

	static void audioCapture(void *param, obs_source_t *, const struct audio_data *audio, bool)
	{
		auto *state = static_cast<SourceState *>(param);
		if (!state || !audio)
			return;
		const uint64_t now = os_gettime_ns();
		const uint64_t timestamp = audio->timestamp;
		if (timestamp) {
			uint64_t expectedFirst = 0;
			state->firstValidSampleWallNs.compare_exchange_strong(expectedFirst, now);
		}
		const uint64_t previousTimestamp = state->lastAudioTimestampNs.exchange(timestamp);
		const uint64_t previousWall = state->lastAudioWallNs.exchange(now);
		if (!previousTimestamp || !previousWall || timestamp == 0)
			return;

		const int64_t timestampDelta = signedDelta(timestamp, previousTimestamp);
		const int64_t wallDelta = signedDelta(now, previousWall);
		const int64_t error = timestampDelta - wallDelta;
		if (std::llabs(error) < static_cast<int64_t>(kJumpThresholdNs)) {
			if (wallDelta > 0 && wallDelta <= static_cast<int64_t>(500ULL * kNsPerMs))
				updateAtomicMaximum(state->observedHealthyGapNs, static_cast<uint64_t>(wallDelta));
			return;
		}
		const uint64_t previousJump = state->lastAudioJumpWallNs.load();
		if (previousJump && now - previousJump < kJumpCooldownNs)
			return;
		state->lastAudioJumpWallNs.store(now);
		state->lastAudioJumpErrorNs.store(error);
		state->audioJumpCount.fetch_add(1);
	}

	obs_source_t *sourceForState(const SourceState &state) const
	{
		std::lock_guard<std::mutex> lock(sourceMutex_);
		return state.weak ? obs_weak_source_get_source(state.weak) : nullptr;
	}

	bool videoProbeAttached() const
	{
		obs_weak_source_t *weak = nullptr;
		{
			std::lock_guard<std::mutex> lock(sourceMutex_);
			weak = states_[0].videoProbeWeak;
		}
		if (!weak)
			return false;
		obs_source_t *probe = obs_weak_source_get_source(weak);
		if (!probe)
			return false;
		obs_source_release(probe);
		return true;
	}

	bool pulseReset(SourceState &state)
	{
		if (state.resetInProgress->exchange(true))
			return false;

		obs_source_t *source = sourceForState(state);
		if (!source) {
			state.resetInProgress->store(false);
			return false;
		}
		const char *id = obs_source_get_unversioned_id(source);
		if (!id || strcmp(id, kDistroAvSourceId) != 0) {
			obs_source_release(source);
			state.resetInProgress->store(false);
			return false;
		}
		obs_properties_t *properties = obs_source_properties(source);
		const bool compatible = properties && obs_properties_get(properties, kPropFrameSync) &&
			obs_properties_get(properties, kPropLatency) && obs_properties_get(properties, kPropSync) &&
			obs_properties_get(properties, kPropSource);
		if (properties)
			obs_properties_destroy(properties);
		if (!compatible) {
			queueBackgroundEvent(QStringLiteral("Recovery blocked for %1: this DistroAV build does not expose the expected receiver properties")
						 .arg(state.role));
			obs_source_release(source);
			state.resetInProgress->store(false);
			return false;
		}

		auto ticket = std::make_shared<ResetTicket>();
		ticket->original = obs_source_get_settings(source);
		makeDistroAvSettingsExplicit(ticket->original);
		ticket->runtime.capture(source);
		ticket->weak = obs_source_get_weak_source(source);
		ticket->resetFlag = state.resetInProgress;
		ticket->role = state.role;
		ticket->expectedFrameSync = obs_data_get_bool(ticket->original, kPropFrameSync);
		ticket->expectedLatency = obs_data_get_int(ticket->original, kPropLatency);
		ticket->expectedSyncMode = obs_data_get_int(ticket->original, kPropSync);
		ticket->expectedNdiTarget = QString::fromUtf8(obs_data_get_string(ticket->original, kPropSource));

		obs_data_t *pulse = obs_data_create();
		obs_data_apply(pulse, ticket->original);
		obs_data_set_bool(pulse, kPropFrameSync, !ticket->expectedFrameSync);
		obs_source_update(source, pulse);
		obs_data_release(pulse);
		obs_source_release(source);
		state.resetCount.fetch_add(1);

		const int delay = std::clamp(engineSettings_.pulseDurationMs.load(), 50, 1500);
		ticket->restoreAtNs = os_gettime_ns() + static_cast<uint64_t>(delay) * kNsPerMs;
		{
			std::lock_guard<std::mutex> lock(resetMutex_);
			pendingResets_.push_back(ticket);
		}
		watchdogCv_.notify_all();
		return true;
	}

	void recenterSoftSyncState(SourceState &state)
	{
		state.softSync.netFrameAdjustment.store(0);
		state.softSync.lastInputTimestampNs.store(0);
		state.softSync.lastInputWallNs.store(0);
		state.softSync.lastOutputTimestampNs.store(0);
		state.softSync.lastOutputWallNs.store(0);
		state.softSync.generation.fetch_add(1);
	}

	void recenterSoftSyncForTarget(RecoveryTarget target)
	{
		switch (target) {
		case RecoveryTarget::Video:
			// The video receiver is the usual target for negative transport drift.
			// Its rebuild recenters the A/V pair, so discard accumulated audio trim.
			recenterSoftSyncState(states_[1]);
			recenterSoftSyncState(states_[2]);
			break;
		case RecoveryTarget::DesktopAudio:
			recenterSoftSyncState(states_[1]);
			if (engineSettings_.softSyncLinkMic.load())
				recenterSoftSyncState(states_[2]);
			break;
		case RecoveryTarget::Mic:
			recenterSoftSyncState(states_[2]);
			break;
		case RecoveryTarget::BothAudio:
		case RecoveryTarget::EntireGroup:
			recenterSoftSyncState(states_[1]);
			recenterSoftSyncState(states_[2]);
			break;
		default:
			break;
		}
	}

	bool resetTarget(RecoveryTarget target)
	{
		bool resetAny = false;
		switch (target) {
		case RecoveryTarget::Video:
			resetAny = pulseReset(states_[0]);
			break;
		case RecoveryTarget::DesktopAudio:
			resetAny = pulseReset(states_[1]);
			break;
		case RecoveryTarget::Mic:
			resetAny = pulseReset(states_[2]);
			break;
		case RecoveryTarget::BothAudio:
			resetAny = pulseReset(states_[1]) || resetAny;
			resetAny = pulseReset(states_[2]) || resetAny;
			break;
		case RecoveryTarget::EntireGroup:
			for (auto &state : states_)
				resetAny = pulseReset(state) || resetAny;
			break;
		default:
			break;
		}
		if (resetAny)
			recenterSoftSyncForTarget(target);
		return resetAny;
	}

	void manualReset(RecoveryTarget target, const QString &action)
	{
		if (recovery_.active || anyResetInProgress()) {
			appendEvent(action + QStringLiteral(" blocked: another recovery is active"), false);
			return;
		}
		if (!resetTarget(target)) {
			appendEvent(action + QStringLiteral(" failed: no selected DistroAV source"), false);
			return;
		}
		const uint64_t now = os_gettime_ns();
		detectionSuppressedUntilNs_ = now + static_cast<uint64_t>(verifyDelaySec_->value()) * kNsPerSecond;
		showSuggestedRecovery(target, QStringLiteral("Manual reset pulse started; wait for the streams to settle."),
				      currentConfidence_);
		appendEvent(action);
	}

	bool anyResetInProgress() const
	{
		for (const auto &state : states_) {
			if (state.resetInProgress->load())
				return true;
		}
		return false;
	}

	void captureSnapshotState()
	{
		if (anyResetInProgress() || recovery_.active) {
			appendEvent(QStringLiteral("Capture Known-Good State blocked: recovery is active"), false);
			return;
		}
		int captured = 0;
		for (auto &state : states_) {
			if (state.snapshot) {
				obs_data_release(state.snapshot);
				state.snapshot = nullptr;
			}
			state.snapshotRuntime = SourceRuntimeSnapshot{};
			obs_source_t *source = sourceForState(state);
			if (!source)
				continue;
			state.snapshot = obs_source_get_settings(source);
			makeDistroAvSettingsExplicit(state.snapshot);
			state.snapshotRuntime.capture(source);
			obs_source_release(source);
			captured++;
		}
		const double filtered = filteredOffsetMs_.load();
		if (std::isfinite(filtered))
			baselineOffsetMs_.store(filtered);
		appendEvent(QStringLiteral("Captured known-good state for %1 source(s)").arg(captured));
	}

	void restoreSnapshotState()
	{
		if (restoringSnapshot_)
			return;
		if (anyResetInProgress() || recovery_.active) {
			appendEvent(QStringLiteral("Restore Last Known-Good State blocked: recovery is active"), false);
			return;
		}
		restoringSnapshot_ = true;
		int restored = 0;
		for (auto &state : states_) {
			if (!state.snapshot)
				continue;
			obs_source_t *source = sourceForState(state);
			if (!source)
				continue;
			// Restore non-destructively so properties that are absent from the saved
			// JSON (usually values equal to defaults) are not erased. Then restore
			// OBS-level audio routing, sync offset, monitoring and presentation state.
			obs_source_update(source, state.snapshot);
			state.snapshotRuntime.restore(source);
			obs_source_release(source);
			restored++;
		}
		if (restored == 0) {
			appendEvent(QStringLiteral("Restore Last Known-Good State failed: no snapshot"), false);
			restoringSnapshot_ = false;
			return;
		}
		appendEvent(QStringLiteral("Restored last known-good settings and OBS source properties for %1 source(s)").arg(restored));
		snapshotRebuildResultReady_.store(false);
		snapshotRebuildAtNs_.store(os_gettime_ns() + 100ULL * kNsPerMs);
		watchdogCv_.notify_all();
	}

	void clearCalibration(const QString &reason, bool log)
	{
		{
			std::lock_guard<std::mutex> lock(measurementMutex_);
			offsetSamples_.clear();
		}
		rawOffsetMs_.store(std::numeric_limits<double>::quiet_NaN());
		baselineOffsetMs_.store(std::numeric_limits<double>::quiet_NaN());
		filteredOffsetMs_.store(std::numeric_limits<double>::quiet_NaN());
		driftRateMsPerMinute_.store(std::numeric_limits<double>::quiet_NaN());
		controllerRateMsPerMinute_.store(std::numeric_limits<double>::quiet_NaN());
		softSyncEstimatedDriftMs_.store(std::numeric_limits<double>::quiet_NaN());
		driftSinceNs_ = 0;
		const uint64_t graceUntil = os_gettime_ns() +
			static_cast<uint64_t>(engineSettings_.startupGraceSec.load()) * kNsPerSecond;
		monitoringGraceUntilNs_ = graceUntil;
		monitoringGraceUntilAtomicNs_.store(graceUntil);
		for (auto &state : states_) {
			state.softSync.netFrameAdjustment.store(0);
			state.softSync.generation.fetch_add(1);
		}
		if (log)
			appendEvent(QStringLiteral("A/V auto calibration restarted: %1").arg(reason), false);
	}

	void refreshDiagnostics()
	{
		const uint64_t now = os_gettime_ns();
		if (!lastLifecycleScanNs_ || now - lastLifecycleScanNs_ >= 5ULL * kNsPerSecond) {
			lastLifecycleScanNs_ = now;
			refreshSourceLists();
			for (size_t i = 0; i < states_.size(); ++i) {
				if (states_[i].sourceName.isEmpty())
					continue;
				obs_source_t *bound = sourceForState(states_[i]);
				if (bound) {
					obs_source_release(bound);
					continue;
				}
				obs_source_t *replacement = obs_get_source_by_name(states_[i].sourceName.toUtf8().constData());
				if (replacement) {
					obs_source_release(replacement);
					bindSource(i, states_[i].sourceName);
					quarantineUntilNs_.store(now + kIncidentQuarantineNs);
					appendEvent(QStringLiteral("Reattached recreated source: %1").arg(states_[i].sourceName), false);
				}
			}
		}
		flushBackgroundEvents();
		if (snapshotRebuildResultReady_.exchange(false))
			restoringSnapshot_ = false;
		for (size_t i = 0; i < states_.size(); ++i)
			refreshSourceRow(i, now);
		logNewTimestampJumps();

		const double rawOffset = rawOffsetMs_.load();
		const double filtered = filteredOffsetMs_.load();
		const double baseline = baselineOffsetMs_.load();
		const double rate = driftRateMsPerMinute_.load();
		const double micOffset = calculateMicOffsetMs();
		const double drift = currentDriftMs();

		if (std::isfinite(filtered)) {
			avOffsetLabel_->setText(QStringLiteral("Video − Desktop Audio timestamp: %1 ms filtered (%2 ms raw)")
							.arg(filtered, 0, 'f', 2)
							.arg(rawOffset, 0, 'f', 2));
		} else if (!states_[0].firstValidSampleWallNs.load()) {
			avOffsetLabel_->setText(states_[0].lastVideoArrivalWallNs.load()
						 ? QStringLiteral("Video − Desktop Audio timestamp: video frame seen, waiting for a valid timestamp")
						 : QStringLiteral("Video − Desktop Audio timestamp: waiting for first video frame"));
		} else if (!states_[1].firstValidSampleWallNs.load()) {
			avOffsetLabel_->setText(QStringLiteral("Video − Desktop Audio timestamp: waiting for desktop audio"));
		} else {
			avOffsetLabel_->setText(QStringLiteral("Video − Desktop Audio timestamp: collecting stable samples"));
		}

		if (std::isfinite(drift)) {
			const bool correctedActive = softSyncCorrectionActive();
			const double correctedDrift = correctedActive ? effectiveDriftMs() : drift;
			const double displaySeverity = std::isfinite(correctedDrift) ? std::fabs(correctedDrift)
										      : std::fabs(drift);
			const double warningPoint = std::max(static_cast<double>(softSyncDeadZoneMs_->value()),
										  static_cast<double>(driftThresholdMs_->value()) * 0.5);
			QString driftColor = QStringLiteral("#8bd49c");
			if (displaySeverity >= static_cast<double>(driftThresholdMs_->value()))
				driftColor = QStringLiteral("#ff7b72");
			else if (displaySeverity >= warningPoint)
				driftColor = QStringLiteral("#f2c66d");

			if (correctedActive && std::isfinite(correctedDrift)) {
				driftLabel_->setText(QStringLiteral("Drift from baseline: %1 ms  →  Adaptive Sync corrected to %2 ms")
							    .arg(drift, 0, 'f', 1)
							    .arg(correctedDrift, 0, 'f', 1));
			} else {
				driftLabel_->setText(QStringLiteral("Drift from baseline: %1 ms").arg(drift, 0, 'f', 1));
			}
			driftLabel_->setStyleSheet(QStringLiteral("font-size: 14px; font-weight: 700; color: %1;")
							 .arg(driftColor));

			const QString rateText = std::isfinite(rate) ? QStringLiteral("%1 ms/min").arg(rate, 0, 'f', 2)
									 : QStringLiteral("settling");
			driftTrendLabel_->setText(QStringLiteral("Trend: %1 · %2 · baseline %3 ms")
							      .arg(rateText)
							      .arg(driftDirectionShort(rate))
							      .arg(baseline, 0, 'f', 1));
		} else if (states_[0].firstValidSampleWallNs.load() && states_[1].firstValidSampleWallNs.load()) {
			driftLabel_->setText(QStringLiteral("Drift from baseline: calibrating"));
			driftLabel_->setStyleSheet(QStringLiteral("font-size: 14px; font-weight: 700; color: #cbd3df;"));
			driftTrendLabel_->setText(QStringLiteral("Learning %1 seconds of stable timing data")
								 .arg(static_cast<int>(kBaselineWindowNs / kNsPerSecond)));
		} else {
			driftLabel_->setText(QStringLiteral("Drift from baseline: waiting for timestamps"));
			driftLabel_->setStyleSheet(QStringLiteral("font-size: 14px; font-weight: 700; color: #cbd3df;"));
			driftTrendLabel_->setText(QStringLiteral("Waiting for valid video and desktop-audio timing"));
		}

		micOffsetLabel_->setText(std::isfinite(micOffset)
						 ? QStringLiteral("Mic − Desktop Audio timestamp: %1 ms").arg(micOffset, 0, 'f', 2)
						 : QStringLiteral("Mic − Desktop Audio timestamp: —"));

		updateSoftSyncStatusLabel();
		updateObsStats();
		evaluateAutomation(now);
		const auto reliability = static_cast<MeasurementReliability>(measurementReliability_.load());
		const double corrected = softSyncCorrectionActive() ? effectiveDriftMs() : drift;
		if (compactStatusLabel_) {
			QString status = QStringLiteral("In sync");
			if (recovery_.active || backgroundRecovery_.active)
				status = QStringLiteral("Recovering");
			else if (reliability == MeasurementReliability::Unreliable)
				status = QStringLiteral("Measurement unreliable");
			else if (reliability == MeasurementReliability::Waiting || reliability == MeasurementReliability::Calibrating)
				status = reliabilityName(reliability);
			else if (activeSummaryIssue_ == IssueKind::PersistentDrift)
				status = QStringLiteral("Slow drift");
			else if (activeSummaryIssue_ != IssueKind::None)
				status = QStringLiteral("Sync jump or stall");
			compactStatusLabel_->setText(status);
		}
		if (compactOffsetLabel_) {
			compactOffsetLabel_->setText(std::isfinite(corrected)
				? QStringLiteral("Corrected offset: %1 ms · Measurement: %2")
					  .arg(corrected, 0, 'f', 1).arg(reliabilityName(reliability))
				: QStringLiteral("Corrected offset: — · Measurement: %1").arg(reliabilityName(reliability)));
		}
		const bool requiredMapped = sourceMapped(0) && sourceMapped(1);
		const bool controllerReady = !enableSoftSync_->isChecked() || states_[1].softSync.enabled.load();
		if (readinessLabel_) {
			QString readiness;
			if (!requiredMapped)
				readiness = QStringLiteral("Setup needed: select video and desktop audio");
			else if (!setupConfirmed_)
				readiness = QStringLiteral("Detected: video ‘%1’ · audio ‘%2’%3")
					.arg(states_[0].sourceName, states_[1].sourceName,
					     states_[2].sourceName.isEmpty() ? QString() : QStringLiteral(" · mic ‘%1’").arg(states_[2].sourceName));
			else if (!videoProbeAttached() || !controllerReady)
				readiness = QStringLiteral("Preparing protection components…");
			else if (reliability != MeasurementReliability::Reliable)
				readiness = QStringLiteral("Protection learning: %1").arg(reliabilityName(reliability));
			else if (automaticProtection_->isChecked())
				readiness = QStringLiteral("Protected — automatic fixing is ready");
			else
				readiness = QStringLiteral("Monitoring only — automatic fixing is paused");
			readinessLabel_->setText(readiness);
		}
		if (confirmSetupButton_)
			confirmSetupButton_->setVisible(!setupConfirmed_ && requiredMapped);
		if (effectiveThresholdsLabel_) {
			effectiveThresholdsLabel_->setText(QStringLiteral("Effective thresholds: video %1 ms · desktop audio %2 ms · mic %3 ms%4")
				.arg(effectiveStallThresholdMs(0)).arg(effectiveStallThresholdMs(1)).arg(effectiveStallThresholdMs(2))
				.arg(autoTuneThresholds_->isChecked() ? QStringLiteral(" · automatically tuned") : QStringLiteral(" · manual")));
		}
		if (notificationLabel_ && notificationLabel_->isVisible() && notificationHideAtNs_ && now >= notificationHideAtNs_) {
			notificationLabel_->setVisible(false);
			notificationHideAtNs_ = 0;
		}
		updateOverallSummary();
		addDiagnosticSample(now, rawOffset);
		updateIncidentCapture(now);
	}

	void refreshSourceRow(size_t index, uint64_t now)
	{
		SourceState &state = states_[index];
		obs_source_t *source = sourceForState(state);
		QString stateText = QStringLiteral("Missing");
		QString packetAgeText = QStringLiteral("—");
		QString settingsText = QStringLiteral("—");
		bool fresh = false;

		if (source) {
			state.active.store(obs_source_active(source));
			state.showing.store(obs_source_showing(source));
			state.enabled.store(obs_source_enabled(source));
			stateText = QStringLiteral("%1 / %2 / %3%4%5")
					    .arg(state.enabled.load() ? QStringLiteral("Enabled") : QStringLiteral("Disabled"),
						 state.active.load() ? QStringLiteral("Active") : QStringLiteral("Inactive"),
						 state.showing.load() ? QStringLiteral("Showing") : QStringLiteral("Hidden"),
						 state.resetInProgress->load() ? QStringLiteral(" / Resetting") : QString(),
						 index == 0 ? (videoProbeAttached() ? QStringLiteral(" / Probe ready")
										      : QStringLiteral(" / Probe missing"))
							    : QString());

			const uint64_t packetWall = packetWallNs(index);
			if (packetWall && now >= packetWall) {
				const double ageMs = static_cast<double>(now - packetWall) / static_cast<double>(kNsPerMs);
				packetAgeText = QStringLiteral("%1 ms").arg(ageMs, 0, 'f', 1);
				const int threshold = effectiveStallThresholdMs(index);
				fresh = ageMs < static_cast<double>(threshold);
			} else if (index == 0) {
				const uint64_t arrival = state.lastVideoArrivalWallNs.load();
				packetAgeText = arrival ? QStringLiteral("Frame seen; waiting for timestamp")
							: QStringLiteral("Waiting for first video frame");
			} else {
				packetAgeText = QStringLiteral("Waiting for first audio block");
			}

			obs_data_t *settings = obs_source_get_settings(source);
			const QString ndiTarget = QString::fromUtf8(obs_data_get_string(settings, kPropSource));
			const long long latency = obs_data_get_int(settings, kPropLatency);
			const bool frameSync = obs_data_get_bool(settings, kPropFrameSync);
			const long long syncMode = obs_data_get_int(settings, kPropSync);
			settingsText = QStringLiteral("%1 | %2 | FrameSync %3 | %4")
					       .arg(ndiTarget.isEmpty() ? QStringLiteral("No NDI target") : ndiTarget,
						    latencyName(latency), frameSync ? QStringLiteral("On") : QStringLiteral("Off"),
						    syncModeName(syncMode));
			obs_data_release(settings);
			obs_source_release(source);
		} else {
			state.active.store(false);
			state.showing.store(false);
			state.enabled.store(false);
		}

		if (fresh)
			state.hasEverBeenFresh.store(true);
		state.wasFresh.store(fresh);

		const uint64_t jumps = index == 0 ? state.videoJumpCount.load() : state.audioJumpCount.load();
		setCell(index, 0, state.role);
		setCell(index, 1, state.sourceName.isEmpty() ? QStringLiteral("(Not selected)") : state.sourceName);
		setCell(index, 2, stateText);
		setCell(index, 3, packetAgeText);
		setCell(index, 4, QString::number(static_cast<unsigned long long>(jumps)));
		setCell(index, 5, QString::number(static_cast<unsigned long long>(state.resetCount.load())));
		setCell(index, 6, QString::number(static_cast<unsigned long long>(state.recoveryCount.load())));
		setCell(index, 7, settingsText);
	}

	uint64_t packetWallNs(size_t index) const
	{
		return index == 0 ? states_[0].lastVideoWallNs.load() : states_[index].lastAudioWallNs.load();
	}

	double packetAgeMs(size_t index, uint64_t now) const
	{
		const uint64_t wall = packetWallNs(index);
		if (!wall || now < wall)
			return std::numeric_limits<double>::quiet_NaN();
		return static_cast<double>(now - wall) / static_cast<double>(kNsPerMs);
	}

	void audioTransportClock(size_t index, uint64_t &timestamp, uint64_t &wall) const
	{
		timestamp = states_[index].lastAudioTimestampNs.load();
		wall = states_[index].lastAudioWallNs.load();
		if (!states_[index].softSync.enabled.load())
			return;
		const uint64_t rawTimestamp = states_[index].softSync.lastInputTimestampNs.load();
		const uint64_t rawWall = states_[index].softSync.lastInputWallNs.load();
		if (rawTimestamp && rawWall) {
			timestamp = rawTimestamp;
			wall = rawWall;
		}
	}

	double calculateAvOffsetMs() const
	{
		const uint64_t videoTs = states_[0].lastVideoTimestampNs.load();
		const uint64_t videoWall = states_[0].lastVideoWallNs.load();
		uint64_t audioTs = 0;
		uint64_t audioWall = 0;
		audioTransportClock(1, audioTs, audioWall);
		if (!videoTs || !videoWall || !audioTs || !audioWall)
			return std::numeric_limits<double>::quiet_NaN();
		const uint64_t now = os_gettime_ns();
		const int64_t videoProjected = static_cast<int64_t>(videoTs) + signedDelta(now, videoWall);
		const int64_t audioProjected = static_cast<int64_t>(audioTs) + signedDelta(now, audioWall);
		return nsToMs(videoProjected - audioProjected);
	}

	double calculateMicOffsetMs() const
	{
		uint64_t desktopTs = 0;
		uint64_t desktopWall = 0;
		uint64_t micTs = 0;
		uint64_t micWall = 0;
		audioTransportClock(1, desktopTs, desktopWall);
		audioTransportClock(2, micTs, micWall);
		if (!desktopTs || !desktopWall || !micTs || !micWall)
			return std::numeric_limits<double>::quiet_NaN();
		const uint64_t now = os_gettime_ns();
		const int64_t desktopProjected = static_cast<int64_t>(desktopTs) + signedDelta(now, desktopWall);
		const int64_t micProjected = static_cast<int64_t>(micTs) + signedDelta(now, micWall);
		return nsToMs(micProjected - desktopProjected);
	}


	double currentDriftMs() const
	{
		const double baseline = baselineOffsetMs_.load();
		const double filtered = filteredOffsetMs_.load();
		if (!std::isfinite(baseline) || !std::isfinite(filtered))
			return std::numeric_limits<double>::quiet_NaN();
		return filtered - baseline;
	}

	bool softSyncCorrectionActive() const
	{
		return engineSettings_.softSyncEnabled.load() && states_[1].softSync.enabled.load();
	}

	double softSyncAccumulatedDelayMs() const
	{
		if (!softSyncCorrectionActive())
			return 0.0;
		const uint32_t sampleRate = states_[1].softSync.sampleRate.load();
		if (!sampleRate)
			return 0.0;
		return static_cast<double>(states_[1].softSync.netFrameAdjustment.load()) * 1000.0 /
		       static_cast<double>(sampleRate);
	}

	double effectiveDriftMs() const
	{
		const double rawDrift = currentDriftMs();
		if (!std::isfinite(rawDrift))
			return rawDrift;
		return rawDrift + softSyncAccumulatedDelayMs();
	}

	double driftForPersistentDetectionMs() const
	{
		if (softSyncCorrectionActive()) {
			const double corrected = effectiveDriftMs();
			if (std::isfinite(corrected))
				return corrected;
		}
		return currentDriftMs();
	}

	void updateObsStats()
	{
		outputActiveAtomic_.store(outputActive());
		const uint32_t totalFrames = obs_get_total_frames();
		const uint32_t laggedFrames = obs_get_lagged_frames();
		const double lagPercent = totalFrames ? (100.0 * laggedFrames / totalFrames) : 0.0;
		QString outputStats = QStringLiteral("stream output inactive");
		obs_output_t *streamOutput = obs_frontend_get_streaming_output();
		if (streamOutput) {
			if (obs_output_active(streamOutput)) {
				const int total = obs_output_get_total_frames(streamOutput);
				const int dropped = obs_output_get_frames_dropped(streamOutput);
				const double droppedPercent = total ? (100.0 * dropped / total) : 0.0;
				outputStats = QStringLiteral("stream dropped %1/%2 (%3%)")
						      .arg(dropped)
						      .arg(total)
						      .arg(droppedPercent, 0, 'f', 3);
			}
			obs_output_release(streamOutput);
		}
		obsStatsLabel_->setText(QStringLiteral("OBS: %1 FPS | render lag %2/%3 (%4%) | %5")
						.arg(obs_get_active_fps(), 0, 'f', 2)
						.arg(laggedFrames)
						.arg(totalFrames)
						.arg(lagPercent, 0, 'f', 4)
						.arg(outputStats));
	}

	bool outputActive() const
	{
		return obs_frontend_streaming_active() || obs_frontend_recording_active();
	}

	bool sourceEligibilitySatisfied() const
	{
		if (!requireActiveSources_->isChecked())
			return true;
		return states_[0].enabled.load() && states_[0].active.load() && states_[0].showing.load() &&
		       states_[1].enabled.load() && states_[1].active.load();
	}

	bool requiredStreamsReady(uint64_t now, QString &reason) const
	{
		if (!videoProbeAttached()) {
			reason = QStringLiteral("video timestamp probe is not attached; rescan sources or remap NDI Video");
			return false;
		}

		const uint64_t videoFirst = states_[0].firstValidSampleWallNs.load();
		const uint64_t desktopFirst = states_[1].firstValidSampleWallNs.load();
		if (!videoFirst) {
			reason = states_[0].lastVideoArrivalWallNs.load()
				 ? QStringLiteral("video frames are arriving, but no valid video timestamp has been received yet")
				 : QStringLiteral("waiting for the first NDI video frame");
			return false;
		}
		if (!desktopFirst) {
			reason = QStringLiteral("waiting for the first NDI desktop-audio block");
			return false;
		}

		const uint64_t firstReady = std::max(videoFirst, desktopFirst);
		if (now < firstReady || now - firstReady < kFirstSampleGraceNs) {
			reason = QStringLiteral("initial video/audio timestamps are settling");
			return false;
		}
		return true;
	}

	void evaluateAutomation(uint64_t now)
	{
		if (!setupConfirmed_) {
			setSummaryIssue(IssueKind::None, RecoveryTarget::None);
			currentSummaryState_ = QStringLiteral("source setup needs confirmation");
			clearSuggestedRecovery();
			automationStatusLabel_->setText(QStringLiteral("Automation: waiting for source setup confirmation"));
			resetConditionTimers();
			return;
		}
		if (recovery_.active) {
			currentSummaryState_ = QStringLiteral("recovery in progress for %1").arg(targetName(recovery_.target));
			showSuggestedRecovery(recovery_.target,
					      QStringLiteral("Recovery is in progress; wait for verification before pressing another reset."),
					      currentConfidence_);
			if (now >= recovery_.verifyAtNs)
				verifyRecovery(now);
			else
				automationStatusLabel_->setText(QStringLiteral("Automation: verifying recovery in %1 sec")
								.arg(static_cast<unsigned long long>((recovery_.verifyAtNs - now) / kNsPerSecond)));
			return;
		}

		if (anyResetInProgress()) {
			currentSummaryState_ = QStringLiteral("manual or automatic reset pulse active");
			automationStatusLabel_->setText(QStringLiteral("Automation: reset pulse active"));
			return;
		}
		if (now < monitoringGraceUntilNs_) {
			setSummaryIssue(IssueKind::None, RecoveryTarget::None);
			currentSummaryState_ = QStringLiteral("calibrating and waiting for stable timing data");
			clearSuggestedRecovery();
			automationStatusLabel_->setText(QStringLiteral("Automation: startup/calibration grace (%1 sec remaining)")
							.arg(static_cast<unsigned long long>((monitoringGraceUntilNs_ - now) / kNsPerSecond)));
			currentConfidence_ = 0;
			healthLabel_->setText(QStringLiteral("Measurement: Calibrating"));
			return;
		}
		if (now < detectionSuppressedUntilNs_) {
			currentSummaryState_ = QStringLiteral("monitoring temporarily suppressed after an action");
			automationStatusLabel_->setText(QStringLiteral("Automation: temporarily suppressed (%1 sec remaining)")
							.arg(static_cast<unsigned long long>((detectionSuppressedUntilNs_ - now) / kNsPerSecond)));
			return;
		}
		if (onlyWhenOutputActive_->isChecked() && !outputActive()) {
			setSummaryIssue(IssueKind::None, RecoveryTarget::None);
			currentSummaryState_ = QStringLiteral("waiting for streaming or recording to start");
			clearSuggestedRecovery();
			automationStatusLabel_->setText(QStringLiteral("Automation: waiting for streaming or recording"));
			resetConditionTimers();
			currentConfidence_ = 0;
			healthLabel_->setText(QStringLiteral("Measurement: Waiting"));
			return;
		}
		if (!sourceEligibilitySatisfied()) {
			setSummaryIssue(IssueKind::None, RecoveryTarget::None);
			currentSummaryState_ = QStringLiteral("waiting for mapped NDI sources to become active");
			clearSuggestedRecovery();
			automationStatusLabel_->setText(QStringLiteral("Automation: waiting for mapped sources to become active/showing"));
			resetConditionTimers();
			currentConfidence_ = 0;
			healthLabel_->setText(QStringLiteral("Measurement: Waiting"));
			return;
		}

		QString readinessReason;
		if (!requiredStreamsReady(now, readinessReason)) {
			setSummaryIssue(IssueKind::None, RecoveryTarget::None);
			currentSummaryState_ = QStringLiteral("waiting for valid NDI timing data");
			clearSuggestedRecovery();
			automationStatusLabel_->setText(QStringLiteral("Automation: %1").arg(readinessReason));
			resetConditionTimers();
			currentConfidence_ = 0;
			healthLabel_->setText(QStringLiteral("Measurement: Waiting for valid timestamps"));
			return;
		}

		const double videoAge = packetAgeMs(0, now);
		const double desktopAge = packetAgeMs(1, now);
		const double micAge = packetAgeMs(2, now);
		const int videoThreshold = effectiveStallThresholdMs(0);
		const int desktopThreshold = effectiveStallThresholdMs(1);
		const int micThreshold = effectiveStallThresholdMs(2);
		const bool videoFresh = videoAge < videoThreshold;
		const bool desktopFresh = desktopAge < desktopThreshold;
		const bool micMapped = !states_[2].sourceName.isEmpty() && states_[2].enabled.load() && states_[2].active.load();
		const uint64_t micFirst = states_[2].firstValidSampleWallNs.load();
		const bool micExpected = micMapped && micFirst && now >= micFirst && now - micFirst >= kFirstSampleGraceNs;
		const bool micStaleIfExpected = !micExpected || micAge >= micThreshold;
		const bool bothAudioStalled = enableFreezeDetection_->isChecked() && videoFresh && micExpected &&
					      desktopAge >= desktopThreshold && micAge >= micThreshold;
		const bool entireGroupStalled = enableFreezeDetection_->isChecked() && !videoFresh && !desktopFresh &&
					       micStaleIfExpected;

		const bool videoStalled = enableFreezeDetection_->isChecked() && desktopFresh &&
					  videoAge >= videoThreshold;
		const bool desktopStalled = enableFreezeDetection_->isChecked() && videoFresh &&
					    desktopAge >= desktopThreshold;
		const bool micStalled = enableFreezeDetection_->isChecked() && micExpected && (videoFresh || desktopFresh) &&
					micAge >= micThreshold;
		updateConditionTimer(videoStalled, videoStallSinceNs_, now);
		updateConditionTimer(desktopStalled, desktopStallSinceNs_, now);
		updateConditionTimer(micStalled, micStallSinceNs_, now);
		updateConditionTimer(bothAudioStalled, bothAudioStallSinceNs_, now);
		updateConditionTimer(entireGroupStalled, entireGroupStallSinceNs_, now);

		const double drift = driftForPersistentDetectionMs();
		const bool driftExceeded = enableDriftDetection_->isChecked() && std::isfinite(drift) &&
					   std::abs(drift) >= driftThresholdMs_->value() && videoFresh && desktopFresh;
		updateConditionTimer(driftExceeded, driftSinceNs_, now);

		IssueKind issue = IssueKind::None;
		RecoveryTarget target = RecoveryTarget::None;
		QString reason;
		int confidence = 0;

		const double baselineNow = baselineOffsetMs_.load();
		const double rawIncidentDrift = std::isfinite(rawOffsetMs_.load()) && std::isfinite(baselineNow)
			? rawOffsetMs_.load() - baselineNow : std::numeric_limits<double>::quiet_NaN();
		const double jumpConfirmMs = std::max(50.0, driftThresholdMs_->value() * 0.5);
		const uint64_t videoJump = states_[0].lastVideoJumpWallNs.load();
		const uint64_t desktopJump = states_[1].lastAudioJumpWallNs.load();
		const uint64_t micJump = states_[2].lastAudioJumpWallNs.load();
		if (videoJump && now >= videoJump + kStallConfirmNs && now - videoJump <= kJumpEvidenceWindowNs &&
		    std::isfinite(rawIncidentDrift) && std::fabs(rawIncidentDrift) >= jumpConfirmMs) {
			issue = IssueKind::VideoJump;
			target = RecoveryTarget::Video;
			confidence = 95;
			reason = QStringLiteral("Confirmed video timestamp jump changed A/V timing by %1 ms").arg(rawIncidentDrift, 0, 'f', 1);
		} else if (desktopJump && now >= desktopJump + kStallConfirmNs && now - desktopJump <= kJumpEvidenceWindowNs &&
			   std::isfinite(rawIncidentDrift) && std::fabs(rawIncidentDrift) >= jumpConfirmMs) {
			issue = IssueKind::DesktopAudioJump;
			target = RecoveryTarget::DesktopAudio;
			confidence = 95;
			reason = QStringLiteral("Confirmed desktop-audio timestamp jump changed A/V timing by %1 ms").arg(rawIncidentDrift, 0, 'f', 1);
		} else if (micJump && now >= micJump + kStallConfirmNs && now - micJump <= kJumpEvidenceWindowNs) {
			issue = IssueKind::MicJump;
			target = RecoveryTarget::Mic;
			confidence = 90;
			reason = QStringLiteral("Confirmed microphone timestamp discontinuity");
		} else if (entireGroupStallSinceNs_ && now - entireGroupStallSinceNs_ >= kStallConfirmNs) {
			issue = IssueKind::EntireGroupStall;
			target = RecoveryTarget::EntireGroup;
			confidence = 99;
			reason = QStringLiteral("All mapped NDI streams are stale (video %1 ms, desktop audio %2 ms%3)")
					 .arg(videoAge, 0, 'f', 0)
					 .arg(desktopAge, 0, 'f', 0)
					 .arg(micExpected ? QStringLiteral(", mic %1 ms").arg(micAge, 0, 'f', 0) : QString());
		} else if (bothAudioStallSinceNs_ && now - bothAudioStallSinceNs_ >= kStallConfirmNs) {
			issue = IssueKind::BothAudioStall;
			target = RecoveryTarget::BothAudio;
			confidence = 98;
			reason = QStringLiteral("Both NDI audio sources are stale while video remains fresh (desktop %1 ms, mic %2 ms)")
					 .arg(desktopAge, 0, 'f', 0)
					 .arg(micAge, 0, 'f', 0);
		} else if (videoStallSinceNs_ && now - videoStallSinceNs_ >= kStallConfirmNs) {
			issue = IssueKind::VideoStall;
			target = RecoveryTarget::Video;
			confidence = 95;
			reason = QStringLiteral("NDI video timestamp has not advanced for %1 ms while desktop audio remains fresh")
					 .arg(videoAge, 0, 'f', 0);
		} else if (desktopStallSinceNs_ && now - desktopStallSinceNs_ >= kStallConfirmNs) {
			issue = IssueKind::DesktopAudioStall;
			target = RecoveryTarget::DesktopAudio;
			confidence = 95;
			reason = QStringLiteral("NDI desktop audio has not delivered a block for %1 ms while video remains fresh")
					 .arg(desktopAge, 0, 'f', 0);
		} else if (micStallSinceNs_ && now - micStallSinceNs_ >= kStallConfirmNs) {
			issue = IssueKind::MicStall;
			target = RecoveryTarget::Mic;
			confidence = 90;
			reason = QStringLiteral("NDI microphone has not delivered a block for %1 ms while companion streams remain fresh")
					 .arg(micAge, 0, 'f', 0);
		} else if (driftSinceNs_ && now - driftSinceNs_ >= static_cast<uint64_t>(driftPersistenceMs_->value()) * kNsPerMs) {
			issue = IssueKind::PersistentDrift;
			target = drift < 0.0 ? RecoveryTarget::Video : RecoveryTarget::DesktopAudio;
			confidence = 75;
			reason = softSyncCorrectionActive()
				? QStringLiteral("Adaptive Sync-corrected drift remained %1 ms from baseline for %2 ms; likely %3 lag")
					  .arg(drift, 0, 'f', 1)
					  .arg(driftPersistenceMs_->value())
					  .arg(drift < 0.0 ? QStringLiteral("video") : QStringLiteral("desktop-audio"))
				: QStringLiteral("Filtered A/V offset moved %1 ms from baseline for %2 ms; likely %3 lag")
					  .arg(drift, 0, 'f', 1)
					  .arg(driftPersistenceMs_->value())
					  .arg(drift < 0.0 ? QStringLiteral("video") : QStringLiteral("desktop-audio"));
		}

		if (issue != IssueKind::None) {
			if (recentJumpEvidence(now))
				confidence = std::min(100, confidence + 15);
			const double displayedRate = driftRateMsPerMinute_.load();
			if (std::isfinite(displayedRate) && std::isfinite(drift) &&
			    std::abs(displayedRate) >= 30.0 && displayedRate * drift > 0.0)
				confidence = std::min(100, confidence + 10);
		}
		currentConfidence_ = confidence; // retained in incident files for v0.3.x report compatibility
		const auto reliability = static_cast<MeasurementReliability>(measurementReliability_.load());
		healthLabel_->setText(QStringLiteral("Measurement: %1%2")
					  .arg(reliabilityName(reliability))
					  .arg(recentJumpEvidence(now) ? QStringLiteral(" | incident samples quarantined") : QString()));

		if (issue == IssueKind::None) {
			monitorState_.store(static_cast<int>(MonitorState::Stable));
			setSummaryIssue(IssueKind::None, RecoveryTarget::None);
			currentSummaryState_ = QStringLiteral("healthy now");
			clearSuggestedRecovery();
			automationStatusLabel_->setText(QStringLiteral("Automation: %1 | healthy")
							.arg(modeName(currentMode())));
			lastObservedIssueKey_.clear();
			return;
		}
		if (issue == IssueKind::PersistentDrift) {
			monitorState_.store(static_cast<int>(MonitorState::Drifting));
			setSummaryIssue(issue, RecoveryTarget::None);
			currentSummaryState_ = engineSettings_.softSyncEnabled.load()
				? QStringLiteral("slow drift correction active")
				: QStringLiteral("slow drift detected; enable Drift Controller to correct it");
			clearSuggestedRecovery();
			automationStatusLabel_->setText(QStringLiteral("Drift Controller: %1")
							.arg(engineSettings_.softSyncEnabled.load() ? QStringLiteral("correcting")
													 : QStringLiteral("off — monitoring only")));
			handleDetectedIssue(now, issue, RecoveryTarget::None, reason, confidence);
			return;
		}

		setSummaryIssue(issue, target);
		currentSummaryState_ = QStringLiteral("current issue: %1; %2 suggested")
					.arg(issueSummaryName(issue), targetName(target));
		automationStatusLabel_->setText(QStringLiteral("Automation: %1 | issue detected: %2")
						.arg(modeName(currentMode()), reason));
		showSuggestedRecovery(target, reason, confidence);
		handleDetectedIssue(now, issue, target, reason, confidence);
	}

	void updateConditionTimer(bool condition, uint64_t &timer, uint64_t now)
	{
		if (condition) {
			if (!timer)
				timer = now;
		} else {
			timer = 0;
		}
	}

	void resetConditionTimers()
	{
		videoStallSinceNs_ = 0;
		desktopStallSinceNs_ = 0;
		micStallSinceNs_ = 0;
		bothAudioStallSinceNs_ = 0;
		entireGroupStallSinceNs_ = 0;
		driftSinceNs_ = 0;
	}

	bool recentJumpEvidence(uint64_t now) const
	{
		const uint64_t videoJump = states_[0].lastVideoJumpWallNs.load();
		const uint64_t desktopJump = states_[1].lastAudioJumpWallNs.load();
		return (videoJump && now - videoJump <= kJumpEvidenceWindowNs) ||
		       (desktopJump && now - desktopJump <= kJumpEvidenceWindowNs);
	}

	void handleDetectedIssue(uint64_t now, IssueKind issue, RecoveryTarget target, const QString &reason, int confidence)
	{
		const QString key = QStringLiteral("%1:%2").arg(static_cast<int>(issue)).arg(static_cast<int>(target));
		startIncident(reason, target, confidence);
		if (issue == IssueKind::PersistentDrift) {
			if (key != lastObservedIssueKey_ || !lastObservedIssueNs_ || now - lastObservedIssueNs_ >= kObserveRepeatNs) {
				appendEvent(engineSettings_.softSyncEnabled.load()
					? QStringLiteral("Slow drift detected; Drift Controller is correcting it: %1").arg(reason)
					: QStringLiteral("Slow drift detected; no receiver reset will be attempted: %1").arg(reason), false);
				lastObservedIssueKey_ = key;
				lastObservedIssueNs_ = now;
			}
			return;
		}
		const AutomationMode mode = currentMode();

		if (mode == AutomationMode::Observe) {
			if (key != lastObservedIssueKey_ || !lastObservedIssueNs_ || now - lastObservedIssueNs_ >= kObserveRepeatNs) {
				appendEvent(QStringLiteral("Observe-only detection (%1/100): %2").arg(confidence).arg(reason), false);
				lastObservedIssueKey_ = key;
				lastObservedIssueNs_ = now;
			}
			return;
		}

		if (mode == AutomationMode::Automatic) {
			// The independent 250 ms watchdog owns automatic recovery. It does not
			// depend on Qt hover, repaint, focus, or dock visibility.
			return;
		}

		QString blockedReason;
		if (!automaticResetAllowed(now, blockedReason)) {
			if (key != lastObservedIssueKey_ || !lastObservedIssueNs_ || now - lastObservedIssueNs_ >= kObserveRepeatNs) {
				appendEvent(QStringLiteral("Suggested recovery blocked: %1. Detected: %2")
						    .arg(blockedReason, reason),
					    false);
				lastObservedIssueKey_ = key;
				lastObservedIssueNs_ = now;
			}
			return;
		}

		if (promptActive_)
			return;
		promptActive_ = true;
		const QMessageBox::StandardButton answer = QMessageBox::question(
			panel_, QStringLiteral("Sync Guardian detected an NDI sync problem"),
			QStringLiteral("%1\n\nConfidence: %2/100\nSuggested action: reset %3.\n\nPerform the reset now?")
				.arg(reason)
				.arg(confidence)
				.arg(targetName(target)),
			QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
		promptActive_ = false;
		if (answer != QMessageBox::Yes) {
			detectionSuppressedUntilNs_ = now + 60ULL * kNsPerSecond;
			appendEvent(QStringLiteral("User declined suggested reset of %1: %2").arg(targetName(target), reason), false);
			return;
		}

		beginAutomatedRecovery(os_gettime_ns(), issue, target, reason);
	}

	bool automaticResetAllowed(uint64_t now, QString &blockedReason)
	{
		while (!automatedResetTimes_.empty() && now - automatedResetTimes_.front() > kResetLimitWindowNs)
			automatedResetTimes_.pop_front();
		if (automatedResetTimes_.size() >= static_cast<size_t>(maxAutoResetsPerHour_->value())) {
			blockedReason = QStringLiteral("maximum of %1 automatic resets per hour reached")
						.arg(maxAutoResetsPerHour_->value());
			return false;
		}
		const uint64_t cooldownNs = static_cast<uint64_t>(cooldownSec_->value()) * kNsPerSecond;
		if (lastAutomatedResetNs_ && now - lastAutomatedResetNs_ < cooldownNs) {
			blockedReason = QStringLiteral("%1-second reset cooldown is active")
						.arg(cooldownSec_->value());
			return false;
		}
		return true;
	}

	void beginAutomatedRecovery(uint64_t now, IssueKind issue, RecoveryTarget target, const QString &reason)
	{
		if (recovery_.active || anyResetInProgress())
			return;
		if (!resetTarget(target)) {
			appendEvent(QStringLiteral("Automated reset failed to start for %1: mapped source unavailable")
					    .arg(targetName(target)),
				    false);
			return;
		}

		automatedResetTimes_.push_back(now);
		lastAutomatedResetNs_ = now;
		recovery_.active = true;
		recovery_.automated = true;
		recovery_.escalationUsed = false;
		recovery_.target = target;
		recovery_.issue = issue;
		recovery_.reason = reason;
		recovery_.preDriftMs = driftForPersistentDetectionMs();
		recovery_.verifyAtNs = now + static_cast<uint64_t>(pulseDurationMs_->value()) * kNsPerMs +
				       static_cast<uint64_t>(verifyDelaySec_->value()) * kNsPerSecond;
		detectionSuppressedUntilNs_ = recovery_.verifyAtNs;
		resetConditionTimers();
		showSuggestedRecovery(target, QStringLiteral("Automatic recovery is running: %1").arg(reason), currentConfidence_);
		appendEvent(QStringLiteral("Automated recovery started: reset %1 because %2")
				    .arg(targetName(target), reason));
	}

	void incrementVerifiedSourceRecoveries(RecoveryTarget target)
	{
		switch (target) {
		case RecoveryTarget::Video:
			states_[0].recoveryCount++;
			break;
		case RecoveryTarget::DesktopAudio:
			states_[1].recoveryCount++;
			break;
		case RecoveryTarget::Mic:
			states_[2].recoveryCount++;
			break;
		case RecoveryTarget::BothAudio:
			states_[1].recoveryCount++;
			states_[2].recoveryCount++;
			break;
		case RecoveryTarget::EntireGroup:
			for (auto &state : states_)
				state.recoveryCount++;
			break;
		default:
			break;
		}
	}

	void verifyRecovery(uint64_t now)
	{
		const bool recovered = recoverySucceeded(now);
		if (recovered) {
			consecutiveRecoveryFailures_.store(0);
			const bool wasSelfTest = recovery_.issue == IssueKind::None &&
				recovery_.reason == QStringLiteral("user-requested protection self-test");
			const RecoveryTarget recoveredTarget = recovery_.target;
			incrementVerifiedSourceRecoveries(recovery_.target);
			verifiedRecoveryCount_++;
			appendEvent(QStringLiteral("Recovery verified: %1 is fresh and the triggering condition cleared")
					    .arg(targetName(recovery_.target)));
			recovery_ = RecoveryAttempt{};
			setSummaryIssue(IssueKind::None, RecoveryTarget::None);
			currentSummaryState_ = QStringLiteral("healthy after a verified recovery");
			detectionSuppressedUntilNs_ = now + 2ULL * kNsPerSecond;
			clearSuggestedRecovery();
			showNotification(wasSelfTest
				? QStringLiteral("Protection test passed: receivers recovered and settings were restored.")
				: QStringLiteral("%1 recovered successfully").arg(targetName(recoveredTarget)), false);
			return;
		}

		while (!automatedResetTimes_.empty() && now - automatedResetTimes_.front() > kResetLimitWindowNs)
			automatedResetTimes_.pop_front();
		const bool escalationWithinLimit = automatedResetTimes_.size() < static_cast<size_t>(maxAutoResetsPerHour_->value());
		if (autoEscalate_->isChecked() && escalationWithinLimit && !recovery_.escalationUsed &&
		    recovery_.target != RecoveryTarget::EntireGroup) {
			if (resetTarget(RecoveryTarget::EntireGroup)) {
				automatedResetTimes_.push_back(now);
				lastAutomatedResetNs_ = now;
				recovery_.escalationUsed = true;
				recovery_.target = RecoveryTarget::EntireGroup;
				recovery_.verifyAtNs = now + static_cast<uint64_t>(pulseDurationMs_->value()) * kNsPerMs +
						       static_cast<uint64_t>(verifyDelaySec_->value()) * kNsPerSecond;
				detectionSuppressedUntilNs_ = recovery_.verifyAtNs;
				showSuggestedRecovery(RecoveryTarget::EntireGroup,
						      QStringLiteral("The targeted reset did not verify; a full-group rebuild is now running."),
						      currentConfidence_);
				appendEvent(QStringLiteral("Initial recovery did not verify; escalated to full NDI sync-group rebuild"));
				return;
			}
		}

		const RecoveryTarget failedTarget = recovery_.target;
		failedRecoveryCount_++;
		const uint32_t failures = consecutiveRecoveryFailures_.fetch_add(1) + 1;
		currentSummaryState_ = QStringLiteral("recovery verification failed; manual review suggested");
		appendEvent(QStringLiteral("Recovery failed verification after %1. Automatic actions are now in cooldown; manual review required")
				    .arg(targetName(failedTarget)));
		recovery_ = RecoveryAttempt{};
		detectionSuppressedUntilNs_ = now + static_cast<uint64_t>(cooldownSec_->value()) * kNsPerSecond;
		showSuggestedRecovery(failedTarget == RecoveryTarget::EntireGroup ? RecoveryTarget::EntireGroup : failedTarget,
				      QStringLiteral("Automatic verification failed. Review the live diagnostics, then use the highlighted recovery action if the problem remains."),
				      100);
		if (failures >= 2) {
			setComboDataBlocked(automationMode_, static_cast<int>(AutomationMode::Observe));
			setCheckBlocked(automaticProtection_, false);
			updateEngineSettings();
			saveConfig();
			appendEvent(QStringLiteral("Fail-safe activated after repeated failed recoveries; protection changed to Monitor"), false);
			showNotification(QStringLiteral("Automatic Protection paused after repeated failed recoveries. Details are available in diagnostics."), true);
		}
	}

	bool recoverySucceeded(uint64_t now) const
	{
		const bool videoFresh = packetAgeMs(0, now) < effectiveStallThresholdMs(0) * 0.75;
		const bool desktopFresh = packetAgeMs(1, now) < effectiveStallThresholdMs(1) * 0.75;
		const bool micFresh = packetAgeMs(2, now) < effectiveStallThresholdMs(2) * 0.75;
		switch (recovery_.issue) {
		case IssueKind::EntireGroupStall:
			return videoFresh && desktopFresh && (states_[2].sourceName.isEmpty() || micFresh);
		case IssueKind::BothAudioStall:
			return desktopFresh && micFresh;
		case IssueKind::VideoStall:
		case IssueKind::VideoJump:
			return videoFresh;
		case IssueKind::DesktopAudioStall:
		case IssueKind::DesktopAudioJump:
			return desktopFresh;
		case IssueKind::MicStall:
		case IssueKind::MicJump:
			return micFresh;
		case IssueKind::PersistentDrift: {
			const double drift = driftForPersistentDetectionMs();
			if (!videoFresh || !desktopFresh || !std::isfinite(drift))
				return false;
			const bool belowThreshold = std::abs(drift) < driftThresholdMs_->value() * 0.75;
			const bool materiallyImproved = std::isfinite(recovery_.preDriftMs) &&
						      std::abs(drift) < std::abs(recovery_.preDriftMs) * 0.5;
			return belowThreshold || materiallyImproved;
		}
		default:
			return videoFresh && desktopFresh;
		}
	}

	void addDiagnosticSample(uint64_t now, double rawOffset)
	{
		DiagnosticSample sample;
		sample.wallNs = now;
		sample.time = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
		sample.rawOffsetMs = rawOffset;
		sample.filteredOffsetMs = filteredOffsetMs_.load();
		sample.driftMs = currentDriftMs();
		sample.correctedDriftMs = softSyncCorrectionActive() ? effectiveDriftMs() : sample.driftMs;
		sample.driftRateMsPerMinute = driftRateMsPerMinute_.load();
		sample.videoAgeMs = packetAgeMs(0, now);
		sample.desktopAgeMs = packetAgeMs(1, now);
		sample.micAgeMs = packetAgeMs(2, now);
		sample.confidence = currentConfidence_;
		diagnosticHistory_.push_back(sample);
		while (!diagnosticHistory_.empty() && now - diagnosticHistory_.front().wallNs > kDiagnosticHistoryNs)
			diagnosticHistory_.pop_front();
		if (incident_.active)
			incident_.samples.append(diagnosticSampleJson(sample));
	}

	QJsonObject diagnosticSampleJson(const DiagnosticSample &sample) const
	{
		QJsonObject object;
		object.insert(QStringLiteral("time"), sample.time);
		object.insert(QStringLiteral("monotonic_ns"), static_cast<double>(sample.wallNs));
		insertFinite(object, QStringLiteral("raw_av_offset_ms"), sample.rawOffsetMs);
		insertFinite(object, QStringLiteral("filtered_av_offset_ms"), sample.filteredOffsetMs);
		insertFinite(object, QStringLiteral("drift_from_baseline_ms"), sample.driftMs);
		insertFinite(object, QStringLiteral("corrected_drift_from_baseline_ms"), sample.correctedDriftMs);
		insertFinite(object, QStringLiteral("drift_rate_ms_per_minute"), sample.driftRateMsPerMinute);
		insertFinite(object, QStringLiteral("video_packet_age_ms"), sample.videoAgeMs);
		insertFinite(object, QStringLiteral("desktop_packet_age_ms"), sample.desktopAgeMs);
		insertFinite(object, QStringLiteral("mic_packet_age_ms"), sample.micAgeMs);
		object.insert(QStringLiteral("confidence"), sample.confidence);
		return object;
	}

	static void insertFinite(QJsonObject &object, const QString &key, double value)
	{
		if (std::isfinite(value))
			object.insert(key, value);
	}

	void startIncident(const QString &reason, RecoveryTarget target, int confidence)
	{
		const uint64_t monotonicNow = os_gettime_ns();
		if (!incidentReports_->isChecked() || incident_.active ||
		    (lastIncidentStartNs_ && monotonicNow - lastIncidentStartNs_ < kIncidentCooldownNs))
			return;
		lastIncidentStartNs_ = monotonicNow;
		const QDateTime nowDate = QDateTime::currentDateTime();
		const QString fileName = QStringLiteral("sync-guardian-incident-%1.json")
					 .arg(nowDate.toString(QStringLiteral("yyyyMMdd-HHmmss-zzz")));
		char *basePath = obs_module_config_path("incidents");
		if (!basePath)
			return;
		QDir directory(QString::fromUtf8(basePath));
		bfree(basePath);
		if (!directory.exists() && !directory.mkpath(QStringLiteral(".")))
			return;

		incident_.active = true;
		incident_.path = directory.filePath(fileName);
		incident_.finalizeAtNs = monotonicNow + kIncidentPostNs;
		incident_.root = QJsonObject{};
		incident_.samples = QJsonArray{};
		incident_.root.insert(QStringLiteral("plugin_version"), QStringLiteral(PLUGIN_VERSION));
		incident_.root.insert(QStringLiteral("detected_at"), nowDate.toString(Qt::ISODateWithMs));
		incident_.root.insert(QStringLiteral("reason"), reason);
		incident_.root.insert(QStringLiteral("suggested_target"), targetName(target));
		incident_.root.insert(QStringLiteral("confidence"), confidence);
		incident_.root.insert(QStringLiteral("mode"), modeName(currentMode()));
		insertFinite(incident_.root, QStringLiteral("baseline_offset_ms"), baselineOffsetMs_.load());
		for (const auto &sample : diagnosticHistory_) {
			if (monotonicNow - sample.wallNs <= kIncidentPreNs)
				incident_.samples.append(diagnosticSampleJson(sample));
		}
		appendEvent(QStringLiteral("Incident capture started: 30 seconds of post-event diagnostics will be written to %1")
				    .arg(fileName),
			    false);
	}

	void updateIncidentCapture(uint64_t now)
	{
		if (incident_.active && now >= incident_.finalizeAtNs)
			finalizeIncident(false);
	}

	void finalizeIncident(bool interrupted)
	{
		if (!incident_.active)
			return;
		incident_.root.insert(QStringLiteral("finalized_at"), QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
		incident_.root.insert(QStringLiteral("interrupted_by_plugin_unload"), interrupted);
		incident_.root.insert(QStringLiteral("samples"), incident_.samples);
		QFile file(incident_.path);
		if (file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
			file.write(QJsonDocument(incident_.root).toJson(QJsonDocument::Indented));
		incident_ = IncidentCapture{};
	}

	void logNewTimestampJumps()
	{
		for (size_t index = 0; index < states_.size(); ++index) {
			const uint64_t count = index == 0 ? states_[index].videoJumpCount.load() : states_[index].audioJumpCount.load();
			if (count <= loggedJumpCounts_[index])
				continue;
			const int64_t errorNs = index == 0 ? states_[index].lastVideoJumpErrorNs.load()
						       : states_[index].lastAudioJumpErrorNs.load();
			const uint64_t newEvents = count - loggedJumpCounts_[index];
			loggedJumpCounts_[index] = count;
			appendEvent(QStringLiteral("%1 timestamp discontinuity detected: %2 ms timeline error%3")
					    .arg(states_[index].role)
					    .arg(nsToMs(errorNs), 0, 'f', 2)
					    .arg(newEvents > 1 ? QStringLiteral(" (multiple callbacks since last poll)") : QString()),
				    false);
		}
	}

	void setCell(size_t row, int column, const QString &text)
	{
		if (QTableWidgetItem *item = statusTable_->item(static_cast<int>(row), column))
			item->setText(text);
	}

	void appendEvent(const QString &event, bool addChapter = true)
	{
		const QString timestamp = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
		const QString line = QStringLiteral("[%1] %2").arg(timestamp, event);
		if (eventLog_)
			eventLog_->append(line.toHtmlEscaped());
		blog(LOG_INFO, "[sync-guardian] %s", event.toUtf8().constData());

		if (addChapter && chapterMarkers_ && chapterMarkers_->isChecked()) {
			const QByteArray chapter = QStringLiteral("Sync Guardian - %1").arg(event).toUtf8();
			obs_frontend_recording_add_chapter(chapter.constData());
		}

		if (jsonLogging_ && jsonLogging_->isChecked()) {
			QJsonObject object;
			object.insert(QStringLiteral("time"), timestamp);
			object.insert(QStringLiteral("event"), event);
			object.insert(QStringLiteral("mode"), modeName(currentMode()));
			insertFinite(object, QStringLiteral("raw_video_minus_desktop_ms"), calculateAvOffsetMs());
			insertFinite(object, QStringLiteral("filtered_video_minus_desktop_ms"), filteredOffsetMs_.load());
			insertFinite(object, QStringLiteral("baseline_ms"), baselineOffsetMs_.load());
			insertFinite(object, QStringLiteral("drift_ms"), currentDriftMs());
			insertFinite(object, QStringLiteral("corrected_drift_ms"),
				     softSyncCorrectionActive() ? effectiveDriftMs() : currentDriftMs());
			insertFinite(object, QStringLiteral("drift_rate_ms_per_minute"), driftRateMsPerMinute_.load());
			insertFinite(object, QStringLiteral("mic_minus_desktop_ms"), calculateMicOffsetMs());
			object.insert(QStringLiteral("confidence"), currentConfidence_);
			object.insert(QStringLiteral("video_resets"), static_cast<double>(states_[0].resetCount.load()));
			object.insert(QStringLiteral("desktop_resets"), static_cast<double>(states_[1].resetCount.load()));
			object.insert(QStringLiteral("mic_resets"), static_cast<double>(states_[2].resetCount.load()));
			char *path = obs_module_config_path("sync-guardian-events.jsonl");
			if (path) {
				QFile file(QString::fromUtf8(path));
				if (file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
					file.write(QJsonDocument(object).toJson(QJsonDocument::Compact));
					file.write("\n");
				}
				bfree(path);
			}
		}
	}

	void registerHotkeys()
	{
		hotkeys_[0] = obs_hotkey_register_frontend("sync_guardian.reset_video", "Sync Guardian: Reset Video Only",
						      hotkeyCallback, this);
		hotkeys_[1] = obs_hotkey_register_frontend("sync_guardian.reset_desktop", "Sync Guardian: Reset Desktop Audio Only",
						      hotkeyCallback, this);
		hotkeys_[2] = obs_hotkey_register_frontend("sync_guardian.reset_mic", "Sync Guardian: Reset Mic Only",
						      hotkeyCallback, this);
		hotkeys_[3] = obs_hotkey_register_frontend("sync_guardian.reset_audio", "Sync Guardian: Reset Both Audio Sources",
						      hotkeyCallback, this);
		hotkeys_[4] = obs_hotkey_register_frontend("sync_guardian.rebuild_group", "Sync Guardian: Rebuild Entire Sync Group",
						      hotkeyCallback, this);
		hotkeys_[5] = obs_hotkey_register_frontend("sync_guardian.restore_snapshot", "Sync Guardian: Restore Last Known-Good State",
						      hotkeyCallback, this);
		hotkeys_[6] = obs_hotkey_register_frontend("sync_guardian.mark_event", "Sync Guardian: Mark Sync Event",
						      hotkeyCallback, this);
		hotkeys_[7] = obs_hotkey_register_frontend("sync_guardian.recalibrate", "Sync Guardian: Restart A/V Auto Calibration",
						      hotkeyCallback, this);
	}

	void unregisterHotkeys()
	{
		for (obs_hotkey_id id : hotkeys_) {
			if (id != OBS_INVALID_HOTKEY_ID)
				obs_hotkey_unregister(id);
		}
	}

	static void hotkeyCallback(void *data, obs_hotkey_id id, obs_hotkey_t *, bool pressed)
	{
		if (!pressed)
			return;
		auto *self = static_cast<SyncGuardian *>(data);
		QMetaObject::invokeMethod(self->lifetimeContext_, [self, id]() {
			if (id == self->hotkeys_[0])
				self->manualReset(RecoveryTarget::Video, QStringLiteral("Reset Video Only (hotkey)"));
			else if (id == self->hotkeys_[1])
				self->manualReset(RecoveryTarget::DesktopAudio, QStringLiteral("Reset Desktop Audio Only (hotkey)"));
			else if (id == self->hotkeys_[2])
				self->manualReset(RecoveryTarget::Mic, QStringLiteral("Reset Mic Only (hotkey)"));
			else if (id == self->hotkeys_[3])
				self->manualReset(RecoveryTarget::BothAudio, QStringLiteral("Reset Both Audio Sources (hotkey)"));
			else if (id == self->hotkeys_[4])
				self->manualReset(RecoveryTarget::EntireGroup, QStringLiteral("Rebuild Entire Sync Group (hotkey)"));
			else if (id == self->hotkeys_[5])
				self->restoreSnapshotState();
			else if (id == self->hotkeys_[6])
				self->appendEvent(QStringLiteral("Manual sync event marker (hotkey)"));
			else if (id == self->hotkeys_[7])
				self->clearCalibration(QStringLiteral("hotkey"), true);
		}, Qt::QueuedConnection);
	}

	static void frontendEvent(enum obs_frontend_event event, void *data)
	{
		auto *self = static_cast<SyncGuardian *>(data);
		if (!self || !self->panel_)
			return;
		if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED || event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
			QMetaObject::invokeMethod(self->lifetimeContext_, [self]() {
				self->refreshSourceLists();
				self->loadConfiguration();
				self->bindAllSources();
				self->updateEngineSettings();
				self->syncSoftSyncFilters();
				self->clearCalibration(QStringLiteral("OBS scene collection/load event"), false);
			}, Qt::QueuedConnection);
		}
	}
};

SyncGuardian *g_guardian = nullptr;

} // namespace

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Automatic DistroAV/NDI A/V timestamp monitoring, staged receiver recovery, and incident diagnostics for OBS Studio.";
}

bool obs_module_load(void)
{
	blog(LOG_INFO, "[sync-guardian] Loading version %s", PLUGIN_VERSION);
	registerVideoProbeFilter();
	registerSoftSyncFilter();
	return true;
}

void obs_module_post_load(void)
{
	g_guardian = new SyncGuardian();
}

void obs_module_unload(void)
{
	delete g_guardian;
	g_guardian = nullptr;
	{
		std::lock_guard<std::mutex> lock(g_videoProbeRegistryMutex);
		g_videoProbeRegistry.clear();
	}
	{
		std::lock_guard<std::mutex> lock(g_softSyncRegistryMutex);
		g_softSyncRegistry.clear();
	}
	blog(LOG_INFO, "[sync-guardian] Unloaded");
}
