#include "UIValidation.hpp"

#include <OBSApp.hpp>
#include <widgets/OBSBasic.hpp>

#include <QMessageBox>
#include <QPushButton>

#include "moc_UIValidation.cpp"

static bool HevcMultitrackEncoderAvailable();
static bool WHIPResolutionFpsWithinLimits(QString &outOfRangeText);

static int CountVideoSources()
{
	int count = 0;
	auto countSources = [](void *param, obs_source_t *source) {
		if (!source)
			return true;

		uint32_t flags = obs_source_get_output_flags(source);
		if ((flags & OBS_SOURCE_VIDEO) != 0)
			(*static_cast<int *>(param))++;

		return true;
	};

	obs_enum_sources(countSources, &count);
	return count;
}

bool UIValidation::NoSourcesConfirmation(QWidget *parent)
{
	// There are sources, don't need confirmation
	if (CountVideoSources() != 0)
		return true;

	// Ignore no video if no parent is visible to alert on
	if (!parent->isVisible())
		return true;

	QString msg = QTStr("NoSources.Text");
	msg += "\n\n";
	msg += QTStr("NoSources.Text.AddSource");

	QMessageBox messageBox(parent);
	messageBox.setWindowTitle(QTStr("NoSources.Title"));
	messageBox.setText(msg);

	QAbstractButton *yesButton = messageBox.addButton(QTStr("Yes"), QMessageBox::YesRole);
	messageBox.addButton(QTStr("No"), QMessageBox::NoRole);
	messageBox.setIcon(QMessageBox::Question);
	messageBox.exec();

	if (messageBox.clickedButton() != yesButton)
		return false;
	else
		return true;
}

StreamSettingsAction UIValidation::StreamSettingsConfirmation(QWidget *parent, OBSService service)
{
	if (obs_service_can_try_to_connect(service))
		return StreamSettingsAction::ContinueStream;

	char const *serviceType = obs_service_get_type(service);
	bool isCustomService = (strcmp(serviceType, "rtmp_custom") == 0);

	char const *streamUrl = obs_service_get_connect_info(service, OBS_SERVICE_CONNECT_INFO_SERVER_URL);
	char const *streamKey = obs_service_get_connect_info(service, OBS_SERVICE_CONNECT_INFO_STREAM_KEY);

	bool streamUrlMissing = !(streamUrl != NULL && streamUrl[0] != '\0');
	bool streamKeyMissing = !(streamKey != NULL && streamKey[0] != '\0');

	QString msg;
	if (!isCustomService && streamUrlMissing && streamKeyMissing) {
		msg = QTStr("Basic.Settings.Stream.MissingUrlAndApiKey");
	} else if (!isCustomService && streamKeyMissing) {
		msg = QTStr("Basic.Settings.Stream.MissingStreamKey");
	} else {
		msg = QTStr("Basic.Settings.Stream.MissingUrl");
	}

	QMessageBox messageBox(parent);
	messageBox.setWindowTitle(QTStr("Basic.Settings.Stream.MissingSettingAlert"));
	messageBox.setText(msg);

	QPushButton *cancel;
	QPushButton *settings;

#ifdef __APPLE__
#define ACCEPT_BUTTON QMessageBox::AcceptRole
#define REJECT_BUTTON QMessageBox::ResetRole
#else
#define ACCEPT_BUTTON QMessageBox::NoRole
#define REJECT_BUTTON QMessageBox::NoRole
#endif
	settings = messageBox.addButton(QTStr("Basic.Settings.Stream.StreamSettingsWarning"), ACCEPT_BUTTON);
	cancel = messageBox.addButton(QTStr("Cancel"), REJECT_BUTTON);

	messageBox.setDefaultButton(settings);
	messageBox.setEscapeButton(cancel);

	messageBox.setIcon(QMessageBox::Warning);
	messageBox.exec();

	if (messageBox.clickedButton() == settings)
		return StreamSettingsAction::OpenSettings;
	if (messageBox.clickedButton() == cancel)
		return StreamSettingsAction::Cancel;

	return StreamSettingsAction::ContinueStream;
}

StreamSettingsAction UIValidation::PPCenterFieldsConfirmation(QWidget *parent, OBSService service)
{
	OBSDataAutoRelease settings = obs_service_get_settings(service);

	auto isEmpty = [&](const char *key) {
		const char *value = obs_data_get_string(settings, key);
		return !value || !*value;
	};

	bool missingField = isEmpty("ppcenter_url") || isEmpty("ppcenter_appid") || isEmpty("ppcenter_secret") ||
			    isEmpty("ppcenter_region");

	QString outOfRangeText;

	QString messageText;
	if (missingField) {
		messageText = QTStr("Basic.Settings.Stream.PPCenter.MissingFields");
	} else if (!HevcMultitrackEncoderAvailable()) {
		// See docs/design/whip-hevc-h264-multitrack-simulcast-design.zh-CN.md
		// §4.1 "对无法创建 HEVC encoder 的情况，启动失败并明确提示" -
		// caught here, before streaming starts, rather than letting
		// WHIPOutput fail after the H264 session is already live (which
		// would then have to be aborted per the strong-consistency
		// start requirement).
		messageText = QTStr("Basic.Settings.Stream.WHIPHevcH264Multitrack.NoEncoder");
	} else if (!WHIPResolutionFpsWithinLimits(outOfRangeText)) {
		// ppobs推流限制: resolution/FPS ceiling (see the matching hard
		// enforcement in WHIPOutput::Setup(), whip-output.cpp - this is
		// only the early, user-facing warning; the bitrate leg of the
		// same limit isn't duplicated here since it depends on which of
		// Simple/Advanced output mode is active and is already caught
		// with a clear error at actual stream start).
		messageText = outOfRangeText;
	} else {
		return StreamSettingsAction::ContinueStream;
	}

	QMessageBox messageBox(parent);
	messageBox.setWindowTitle(QTStr("Basic.Settings.Stream.MissingSettingAlert"));
	messageBox.setText(messageText);

	QPushButton *cancel;
	QPushButton *settings_button;

	settings_button = messageBox.addButton(QTStr("Basic.Settings.Stream.StreamSettingsWarning"), ACCEPT_BUTTON);
	cancel = messageBox.addButton(QTStr("Cancel"), REJECT_BUTTON);

	messageBox.setDefaultButton(settings_button);
	messageBox.setEscapeButton(cancel);

	messageBox.setIcon(QMessageBox::Warning);
	messageBox.exec();

	if (messageBox.clickedButton() == settings_button)
		return StreamSettingsAction::OpenSettings;

	return StreamSettingsAction::Cancel;
}

// Checks only whether *some* HEVC video encoder is registered at all - not
// whether it can pair with the user's specific H264 pick, which
// ResolveWHIPHevcEncoderId() (utility/WHIPHevcEncoders.hpp, used at actual
// stream start) resolves more precisely. Good enough for an early sanity
// check: if there is no HEVC encoder whatsoever, streaming will fail no
// matter which H264 encoder is selected. No-op (returns true) when
// multitrack isn't even enabled, so this never blocks a plain WHIP stream.
static bool HevcMultitrackEncoderAvailable()
{
	if (!config_get_bool(OBSBasic::Get()->Config(), "Stream1", "WHIPHevcH264Multitrack"))
		return true;

	size_t i = 0;
	const char *id;
	while (obs_enum_encoder_types(i++, &id)) {
		const char *codec = obs_get_encoder_codec(id);
		if (codec && strcmp(codec, "hevc") == 0 && obs_get_encoder_type(id) == OBS_ENCODER_VIDEO)
			return true;
	}
	return false;
}

// ppobs推流限制 §1/§2 (resolution/FPS) early warning - reads the same
// applied "Video" config values WHIPOutput::Setup() will see once the
// encoders are actually created (config_get_uint("OutputCX"/"OutputCY")
// mirrors obs_video_info after main->ResetVideo() has run, and FPS from
// the same config keys OBSBasicSettings uses). This is advisory only:
// the authoritative check that can't be bypassed lives in
// WHIPOutput::Setup() (see checkStreamLimits() there) and covers bitrate
// too, which this early check does not duplicate.
static bool WHIPResolutionFpsWithinLimits(QString &outOfRangeText)
{
	config_t *config = OBSBasic::Get()->Config();

	const uint32_t cx = config_get_uint(config, "Video", "OutputCX");
	const uint32_t cy = config_get_uint(config, "Video", "OutputCY");
	const bool landscape = cx >= cy;
	const uint32_t maxCx = landscape ? 3840 : 2160;
	const uint32_t maxCy = landscape ? 2160 : 3840;

	double fps = 30.0;
	const uint32_t fpsType = config_get_uint(config, "Video", "FPSType");
	if (fpsType == 1) {
		fps = (double)config_get_uint(config, "Video", "FPSInt");
	} else if (fpsType == 2) {
		const double num = (double)config_get_uint(config, "Video", "FPSNum");
		const double den = (double)config_get_uint(config, "Video", "FPSDen");
		if (den > 0)
			fps = num / den;
	} else {
		const char *common = config_get_string(config, "Video", "FPSCommon");
		if (common)
			sscanf(common, "%lf", &fps);
	}

	if (cx > maxCx || cy > maxCy) {
		QString resStr = QString("%1x%2").arg(QString::number(cx), QString::number(cy));
		QString limitStr = QString("%1x%2").arg(QString::number(maxCx), QString::number(maxCy));
		outOfRangeText = QTStr("Basic.Settings.Stream.WHIPResolutionTooHigh").arg(resStr, limitStr);
		return false;
	}

	if (fps > 60.01) {
		outOfRangeText = QTStr("Basic.Settings.Stream.WHIPFpsTooHigh").arg(QString::number(fps, 'g', 4));
		return false;
	}

	return true;
}
