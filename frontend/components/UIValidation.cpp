#include "UIValidation.hpp"

#include <OBSApp.hpp>
#include <widgets/OBSBasic.hpp>

#include <QMessageBox>
#include <QPushButton>

#include "moc_UIValidation.cpp"

static bool HevcMultitrackEncoderAvailable();

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
