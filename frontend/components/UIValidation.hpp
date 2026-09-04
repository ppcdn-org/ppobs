#pragma once

#include <obs.hpp>

#include <QObject>

enum class StreamSettingsAction {
	OpenSettings,
	Cancel,
	ContinueStream,
};

class UIValidation : public QObject {
	Q_OBJECT

public:
	/* Confirm video about to record or stream has sources.  Shows alert
	 * box notifying there are no video sources Returns true if user clicks
	 * "Yes" Returns false if user clicks "No" */
	static bool NoSourcesConfirmation(QWidget *parent);

	/* Check streaming requirements, shows warning with options to open
	 * settings, cancel stream, or attempt connection anyways.  If setup
	 * basics is missing in stream, explain missing fields and offer to
	 * open settings, cancel, or continue.  Returns Continue if all
	 * settings are valid. */
	static StreamSettingsAction StreamSettingsConfirmation(QWidget *parent, OBSService service);

	/* If PPCenter is enabled on the given (WHIP) service, checks that its
	 * URL, App ID, App Secret and Region fields are all filled in, since
	 * a stream/schedule start with any of them missing just fails later
	 * with a less actionable error from the WHIP output. Shows a warning
	 * with options to open settings or cancel. Returns Continue if
	 * PPCenter is disabled, or all of its fields are filled in. */
	static StreamSettingsAction PPCenterFieldsConfirmation(QWidget *parent, OBSService service);
};
