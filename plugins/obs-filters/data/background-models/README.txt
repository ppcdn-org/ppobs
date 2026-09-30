AI Background Removal - portrait matting models
===============================================

Models are loaded from the OBS configuration directory, not from this folder:

  portable   <OBS folder>\config\background-models\
  installed  %APPDATA%\background-models\

Open the filter's properties and look at "Model folder:" for the exact path,
which is created for you the first time the dialog is opened. Put the .onnx
files there, then reopen the dialog to refresh the list.

This folder is only a fallback: models placed here still load, but adding to a
per-machine installation needs administrator rights, so the configuration
directory is the recommended location.

Supported families
------------------

  MODNet                 e.g. modnet.onnx
  RobustVideoMatting     e.g. rvm_mobilenetv3_fp32.onnx

The family is inferred from the file name. If a model has been renamed, or was
exported yourself, set "Model Type" explicitly in the filter properties instead
of leaving it on automatic.

Licences
--------

No model weights ship with this package. Each model carries its own licence,
which is not necessarily the same as the licence of the code that produced it,
and some are not redistributable:

  MODNet               Apache-2.0 (weights: check the source you obtained them
                       from; the reference release restricts commercial use)
  RobustVideoMatting   GPL-3.0

Verify the licence of any weights you deploy, and keep a record of where they
came from. Do not add model files to this repository: committing them would
itself be redistribution.
