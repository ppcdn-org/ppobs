# Face models

These models are bundled so the big-head filter's reference-face tracking works
without a separate download.

| File | Source | License |
| --- | --- | --- |
| `face_detection_yunet_2026may.onnx` | [OpenCV Zoo / face_detection_yunet](https://github.com/opencv/opencv_zoo/tree/main/models/face_detection_yunet) | MIT (`LICENSE-yunet.txt`) |
| `face_recognition_sface_2021dec.onnx` | [OpenCV Zoo / face_recognition_sface](https://github.com/opencv/opencv_zoo/tree/main/models/face_recognition_sface) | Apache-2.0 (`LICENSE-sface.txt`) |

`face_detection_yunet_2026may.onnx` accepts dynamic input sizes, which lets the
filter run detection on a small letterboxed canvas instead of the fixed
640x640 input used by the older export. `face_recognition_sface_2021dec.onnx`
takes a 112x112 aligned RGB crop and emits a 128-dimensional embedding.

Both licenses permit commercial use. Keep the license files next to the models
when redistributing.
