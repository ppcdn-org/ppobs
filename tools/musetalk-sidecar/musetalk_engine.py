"""MuseTalk v1.5 inference for the ppobs avatar worker.

Adapted from the resident engine in liveFayAgent's musetalk-bridge
(tools/musetalk-bridge/realtime_engine.py), which had already worked out the
things that are not obvious from MuseTalk's own scripts:

  * model loading needs the process cwd to be MUSETALK_DIR, because several
    MuseTalk modules hardcode relative paths (FaceParsing reaches for
    './models/face-parse-bisent/...');
  * the per-reference preprocessing - landmarks, bboxes, VAE latents, blend
    masks - has to be done once and cached, or every utterance pays ~70s;
  * a per-frame landmark bbox jitters a few pixels on real footage, and
    snapping the crop window to it makes the rendered mouth flicker, so the
    bbox is smoothed over nearby frames;
  * frames with no detected face must keep their position in the cycle rather
    than be dropped, and render as the original frame.

What is different here, and it is the central problem: that bridge serves
*clips* over HTTP - one audio file in, one video out - while this worker has a
continuous microphone to follow. MuseTalk's audio API is clip-shaped
(get_audio_feature takes a wav path, get_whisper_chunk needs the whole clip's
length), so a stream has to be cut into windows and each window run as a tiny
clip.

That sets the latency floor: a window's duration, plus the inference time for
it. At the default 0.6s window that is well over half a second, which is far
beyond the 80ms the design doc targets - so mode A cannot be lip-synced by
being fast. It has to be compensated by delaying the broadcast audio, which is
what the sync offset in stage D is for. Shrinking the window trades latency
for GPU overhead per call, not for accuracy.
"""

from __future__ import annotations

import os
import sys
import tempfile
import wave
from pathlib import Path

import numpy as np


class MuseTalkUnavailable(Exception):
    """Raised when the engine cannot be built, with a reason worth printing."""


def _write_wav(path: Path, samples: np.ndarray, sample_rate: int) -> None:
    """MuseTalk's audio processor reads a file, so the window has to become
    one. 16-bit PCM because that is what librosa/whisper expect to load."""
    clipped = np.clip(samples, -1.0, 1.0)
    pcm = (clipped * 32767.0).astype("<i2")

    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(sample_rate)
        wav.writeframes(pcm.tobytes())


class MuseTalkEngine:
    """Streaming wrapper over MuseTalk v1.5.

    name/real_model/width/height match what the worker's handshake reports, so
    the plugin can tell a real model from the stub.
    """

    name = "musetalk v1.5"
    real_model = True

    def __init__(
        self,
        musetalk_dir: str,
        asset_path: str,
        fps: int,
        sample_rate: int,
        max_edge: int = 1024,
        batch_size: int = 8,
        extra_margin: int = 10,
        parsing_mode: str = "jaw",
        window_frames: int = 15,
        half_decode: bool = True,
        gpu_id: int = 0,
    ) -> None:
        self.musetalk_dir = Path(musetalk_dir).resolve()
        if not (self.musetalk_dir / "musetalk").is_dir():
            raise MuseTalkUnavailable(f"'{self.musetalk_dir}' does not look like a MuseTalk checkout")

        asset = Path(asset_path) if asset_path else None
        if not asset or not asset.is_file():
            raise MuseTalkUnavailable("no avatar artwork was given to prepare")

        # cwd and sys.path first: the imports below reach for relative paths.
        os.chdir(self.musetalk_dir)
        if str(self.musetalk_dir) not in sys.path:
            sys.path.insert(0, str(self.musetalk_dir))

        try:
            import cv2
            import torch

            from musetalk.utils.utils import load_all_model
            from musetalk.utils.audio_processor import AudioProcessor
            from musetalk.utils.face_parsing import FaceParsing
            from transformers import WhisperModel
        except ImportError as exc:
            raise MuseTalkUnavailable(
                f"MuseTalk's dependencies are not importable ({exc}). This worker has to run in "
                f"MuseTalk's own environment - e.g. {self.musetalk_dir}\\venv\\Scripts\\python.exe"
            ) from exc

        self._cv2 = cv2
        self._torch = torch

        self.fps = max(1, fps)
        self.sample_rate = sample_rate
        self.max_edge = max_edge
        self.batch_size = batch_size
        self.extra_margin = extra_margin
        self.parsing_mode = parsing_mode
        self.window_frames = max(1, window_frames)
        self.window_samples = int(self.sample_rate * self.window_frames / self.fps)
        self.half_decode = half_decode

        self.device = torch.device(f"cuda:{gpu_id}" if torch.cuda.is_available() else "cpu")
        print(f"[musetalk] loading models on {self.device}", flush=True)

        vae, unet, pe = load_all_model(
            unet_model_path="./models/musetalkV15/unet.pth",
            vae_type="sd-vae",
            unet_config="./models/musetalkV15/musetalk.json",
            device=self.device,
        )
        self.timesteps = torch.tensor([0], device=self.device)
        self.pe = pe.half().to(self.device)
        vae.vae = vae.vae.half().to(self.device)
        unet.model = unet.model.half().to(self.device)
        self.vae = vae
        self.unet = unet
        self.weight_dtype = unet.model.dtype

        self.audio_processor = AudioProcessor(feature_extractor_path="./models/whisper")
        whisper = WhisperModel.from_pretrained("./models/whisper")
        self.whisper = whisper.to(device=self.device, dtype=self.weight_dtype).eval()
        self.whisper.requires_grad_(False)

        self.fp = FaceParsing(left_cheek_width=90, right_cheek_width=90)

        print("[musetalk] preparing the avatar", flush=True)
        self.avatar = self._build_avatar(asset)
        self.cycle_length = len(self.avatar["frame_list_cycle"])

        # What the plugin receives is the repainted *region*, not the whole
        # picture, so width/height here are the reference frame - the
        # coordinate space the region's x/y are in. The region itself is a
        # fraction of this, which is the entire point: compositing it
        # worker-side cost 72ms a frame and sent megabytes per frame, while
        # the GPU already holds the artwork.
        height, width = self.avatar["frame_list_cycle"][0].shape[:2]
        self.width = width
        self.height = height
        self.max_edge = max_edge

        self._pending = np.zeros(0, dtype=np.float32)
        self._cycle_pos = 0
        self.stage_us = {"whisper": 0, "unet": 0, "vae": 0, "blend": 0}
        self.decode_start = self.avatar["decode_start_latent"] if self.half_decode else 0
        print(
            f"[musetalk] ready: {self.width}x{self.height}, cycle {self.cycle_length} frames, "
            f"decoding latent rows {self.decode_start}..{self.LATENT_ROWS}",
            flush=True,
        )

    # -- per-reference preprocessing ---------------------------------------

    def _cap_reference_size(self, img_list):
        """Scales the extracted frames down so their long edge is at most
        max_edge. A no-op when they already fit.

        Downscaled rather than refused: a user picks a photo, and failing on it
        would mean every one of them has to pass through an image editor first.
        The cap also has to exist at all - crop_box on a phone photo
        (1944x2592 measured) exceeds what a single CROP message may carry, so
        without it the plugin rejects every region and the avatar never moves.
        Capping here rather than downscaling the region later keeps a payload's
        pixel count equal to its destination rect, which is what the protocol
        assumes.
        """
        cv2 = self._cv2

        first = cv2.imread(img_list[0])
        if first is None:
            raise MuseTalkUnavailable("could not read the extracted reference frames")

        height, width = first.shape[:2]
        long_edge = max(width, height)
        if long_edge <= self.max_edge:
            print(f"[musetalk] reference {width}x{height}", flush=True)
            return img_list

        scale = self.max_edge / float(long_edge)
        size = (int(round(width * scale)), int(round(height * scale)))
        print(f"[musetalk] reference {width}x{height} -> {size[0]}x{size[1]}", flush=True)

        for path in img_list:
            frame = cv2.imread(path)
            if frame is None:
                continue
            cv2.imwrite(path, cv2.resize(frame, size, interpolation=cv2.INTER_AREA))

        return img_list

    def _build_avatar(self, source: Path) -> dict:
        cv2 = self._cv2
        from musetalk.utils.preprocessing import get_landmark_and_bbox, coord_placeholder
        from musetalk.utils.blending import get_image_prepare_material

        with tempfile.TemporaryDirectory() as tmp:
            frame_dir = Path(tmp) / "frames"
            frame_dir.mkdir()

            if source.suffix.lower() in (".mp4", ".mov", ".mkv", ".webm", ".avi", ".flv"):
                cap = cv2.VideoCapture(str(source))
                idx = 0
                while True:
                    ok, frame = cap.read()
                    if not ok:
                        break
                    cv2.imwrite(str(frame_dir / f"{idx:08d}.png"), frame)
                    idx += 1
                cap.release()
            else:
                frame = cv2.imread(str(source))
                if frame is None:
                    raise MuseTalkUnavailable(f"could not read '{source}' as an image")
                cv2.imwrite(str(frame_dir / "00000000.png"), frame)

            img_list = sorted(str(p) for p in frame_dir.glob("*.png"))

            # Capped before anything measures the frames; see
            # _cap_reference_size for why the cap has to exist.
            img_list = self._cap_reference_size(img_list)

            coord_list, frame_list = get_landmark_and_bbox(img_list, 0)

        raw_coords, valid = [], []
        for bbox, frame in zip(coord_list, frame_list):
            if bbox == coord_placeholder:
                raw_coords.append(None)
                valid.append(False)
                continue
            x1, y1, x2, y2 = bbox
            y2 = min(y2 + self.extra_margin, frame.shape[0])
            raw_coords.append([x1, y1, x2, y2])
            valid.append(True)

        if not any(valid):
            # The message matters: this is what an animal or cartoon avatar
            # hits, and the actionable answer is the other mode, not a retry.
            raise MuseTalkUnavailable(
                "no face was detected in the avatar artwork. MuseTalk needs a human face; "
                "for an animal or cartoon avatar use the mouth-deformation mode instead"
            )

        # Smooth the window over nearby valid frames; a raw per-frame bbox
        # jitters and makes the rendered mouth flicker up and down.
        smooth_radius = 4
        valid_idx = [i for i, ok in enumerate(valid) if ok]
        coords_smoothed = list(raw_coords)
        for i in valid_idx:
            near = [raw_coords[j] for j in valid_idx if abs(j - i) <= smooth_radius]
            coords_smoothed[i] = [sum(c[k] for c in near) / len(near) for k in range(4)]

        coords, latents = [], []
        for bbox, frame in zip(coords_smoothed, frame_list):
            if bbox is None:
                coords.append(None)
                latents.append(None)
                continue
            x1, y1, x2, y2 = (int(round(v)) for v in bbox)
            crop = self._cv2.resize(frame[y1:y2, x1:x2], (256, 256), interpolation=self._cv2.INTER_LANCZOS4)
            coords.append([x1, y1, x2, y2])
            latents.append(self.vae.get_latents_for_unet(crop))

        # Every position needs some latent to keep the UNet batch aligned;
        # invalid positions render as the untouched original frame anyway.
        fill_latent = next(l for l in latents if l is not None)
        fill_coord = next(c for c in coords if c is not None)
        filled_latents, filled_coords = [], []
        for latent, coord in zip(latents, coords):
            if latent is not None:
                fill_latent, fill_coord = latent, coord
            filled_latents.append(fill_latent)
            filled_coords.append(fill_coord)

        # Ping-pong, so a short reference loops without a jump cut. A single
        # still image gives a two-frame cycle of the same picture, which is
        # the "fixed head, moving mouth" case the design doc warns is stiff.
        frame_cycle = frame_list + frame_list[::-1]
        coord_cycle = filled_coords + filled_coords[::-1]
        latent_cycle = filled_latents + filled_latents[::-1]
        valid_cycle = valid + valid[::-1]

        masks, mask_boxes = [], []
        for i, frame in enumerate(frame_cycle):
            if not valid_cycle[i]:
                masks.append(None)
                mask_boxes.append(None)
                continue
            mask, crop_box = get_image_prepare_material(
                frame, coord_cycle[i], fp=self.fp, mode=self.parsing_mode
            )
            masks.append(mask)
            mask_boxes.append(crop_box)

        # Which latent row the VAE actually has to start decoding from.
        #
        # get_image_prepare_material zeroes the mask above the crop box's
        # midline ("keep upper_boundary_ratio of talking area"), so the top of
        # every decoded 256x256 is multiplied by zero and thrown away - the VAE
        # spends half its time on pixels nobody sees. That half was 57% of the
        # per-frame budget once the paste moved to the GPU.
        #
        # The boundary is measured from the mask rather than assumed to be the
        # halfway point: it is expressed in the crop box's coordinates, not the
        # face bbox's, and the two only coincide if the expansion happens to be
        # centred. Measuring costs nothing here and cannot drift from whatever
        # MuseTalk does upstream.
        decode_start = self._decode_start_latent(masks, mask_boxes, coord_cycle, valid_cycle)

        return {
            "decode_start_latent": decode_start,
            "frame_list_cycle": frame_cycle,
            "coord_list_cycle": coord_cycle,
            "input_latent_list_cycle": latent_cycle,
            "mask_list_cycle": masks,
            "mask_coords_list_cycle": mask_boxes,
            "valid_cycle": valid_cycle,
        }

    # SD's VAE downsamples by 8, so one latent row is eight pixels of the
    # 256x256 crop.
    LATENT_SCALE = 8
    LATENT_ROWS = 256 // LATENT_SCALE

    # Latent rows decoded above the first row that matters. The decoder is
    # convolutional, so the top edge of a partial decode is wrong until its
    # receptive field is filled; these rows absorb that and are then thrown
    # away. Two rows is sixteen pixels of context.
    DECODE_OVERLAP_ROWS = 2

    def _decode_start_latent(self, masks, mask_boxes, coords, valid) -> int:
        """The lowest latent row every cycle position can start decoding at.

        One row for the whole cycle rather than one per position: the UNet
        batch mixes positions, and slicing them separately would mean a decode
        call per frame instead of per batch. Taking the minimum decodes a
        little more than some positions need, never less.
        """
        start = self.LATENT_ROWS

        for mask, crop_box, bbox, ok in zip(masks, mask_boxes, coords, valid):
            if not ok or mask is None or crop_box is None:
                continue

            alpha = np.asarray(mask)
            if alpha.ndim == 3:
                alpha = alpha[:, :, 0]

            rows = np.nonzero(alpha.any(axis=1))[0]
            if not len(rows):
                continue

            x_s, y_s, _x_e, _y_e = (int(v) for v in crop_box)
            x1, y1, x2, y2 = (int(v) for v in bbox)
            bbox_h = max(1, y2 - y1)

            # Mask row -> row of the face bbox -> row of the 256-tall crop.
            top_in_bbox = int(rows[0]) - (y1 - y_s)
            top_in_crop = top_in_bbox * 256.0 / bbox_h

            row = int(top_in_crop // self.LATENT_SCALE) - self.DECODE_OVERLAP_ROWS
            start = min(start, max(0, row))

        # A fully masked-in avatar needs the whole thing; also the floor for
        # anything that measured oddly.
        return 0 if start >= self.LATENT_ROWS else start

    # -- streaming inference -----------------------------------------------

    def feed(self, samples, pts_ns: int):
        """Takes 16 kHz mono audio and yields (bgra, pts_ns, rect) per frame.

        rect is (x, y, w, h) in the reference frame: the region is composited
        there by the plugin rather than being a picture of its own.

        Audio is buffered until a whole window is available, because
        get_whisper_chunk needs a clip rather than a sample. Nothing is
        emitted until then, which is the bulk of mode A's latency.
        """
        # The pts of the oldest buffered sample: the incoming block is stamped
        # with its own first sample, so whatever is already queued sits that
        # many samples earlier. Derived from each message rather than carried
        # forward, which keeps it from drifting.
        queued = len(self._pending)
        buffer_pts = pts_ns - int(queued * 1_000_000_000 / self.sample_rate)

        self._pending = np.concatenate([self._pending, np.asarray(samples, dtype=np.float32)])

        while len(self._pending) >= self.window_samples:
            window = self._pending[: self.window_samples]
            self._pending = self._pending[self.window_samples :]

            for (pixels, rect), index in self._run_window(window):
                frame_pts = buffer_pts + int(index * 1_000_000_000 / self.fps)
                yield pixels, frame_pts, rect

            buffer_pts += int(self.window_samples * 1_000_000_000 / self.sample_rate)

    def _run_window(self, window: np.ndarray):
        """Per-stage timings land in self.stage_us.

        Measured because "mode A is too slow" is not actionable while "mode A
        spends most of its time in <stage>" is: the whisper pass is a fixed
        cost per window and amortises if the window grows, whereas UNet time
        is per frame and does not. The protocol's STAT message carries these
        three fields for exactly this reason.
        """
        torch = self._torch
        from musetalk.utils.utils import datagen
        import time as _time

        with torch.no_grad():
            started = _time.perf_counter()
            with tempfile.TemporaryDirectory() as tmp:
                wav_path = Path(tmp) / "window.wav"
                _write_wav(wav_path, window, self.sample_rate)

                features, librosa_length = self.audio_processor.get_audio_feature(str(wav_path))
                chunks = self.audio_processor.get_whisper_chunk(
                    features, self.device, self.weight_dtype, self.whisper, librosa_length, fps=self.fps
                )
            self.stage_us["whisper"] += int((_time.perf_counter() - started) * 1e6)

            produced = 0
            for whisper_batch, latent_batch in datagen(
                chunks, self.avatar["input_latent_list_cycle"], self.batch_size, device=self.device
            ):
                started = _time.perf_counter()
                audio_features = self.pe(whisper_batch.to(self.device))
                latent_batch = latent_batch.to(device=self.device, dtype=self.unet.model.dtype)
                pred = self.unet.model(latent_batch, self.timesteps, encoder_hidden_states=audio_features).sample
                pred = pred.to(device=self.device, dtype=self.vae.vae.dtype)
                torch.cuda.synchronize() if self.device.type == "cuda" else None
                self.stage_us["unet"] += int((_time.perf_counter() - started) * 1e6)

                started = _time.perf_counter()
                if self.decode_start:
                    # Only the rows the mask keeps, plus the overlap that makes
                    # the top edge valid; the overlap is dropped afterwards.
                    sliced = pred[:, :, self.decode_start :, :]
                    decoded = self.vae.decode_latents(sliced)
                    drop = self.DECODE_OVERLAP_ROWS * self.LATENT_SCALE
                    recons = [img[drop:] for img in decoded]
                else:
                    recons = list(self.vae.decode_latents(pred))
                torch.cuda.synchronize() if self.device.type == "cuda" else None
                self.stage_us["vae"] += int((_time.perf_counter() - started) * 1e6)

                for recon in recons:
                    idx = self._cycle_pos % self.cycle_length
                    self._cycle_pos += 1

                    started = _time.perf_counter()
                    region = self._make_region(recon, idx)
                    self.stage_us["blend"] += int((_time.perf_counter() - started) * 1e6)

                    yield region, produced
                    produced += 1

    def _make_region(self, recon, idx: int):
        """Builds the repainted region as BGRA with the blend mask as alpha.

        This replaces MuseTalk's get_image_blending, which is faithful but
        works at full frame size: it converts the whole picture to PIL and back
        twice, flips its channels twice and pastes over it, all to change a
        region a fraction of its size. Measured at 72ms a frame on a
        768x1024 avatar - over half of mode A's budget, none of it on the GPU.

        The compositing itself only ever happened inside crop_box, and PIL's
        paste(im, box, mask) is alpha compositing with the mask as alpha. So
        doing the paste within crop_box and handing the mask over as an alpha
        channel is the same operation, with the final blend done by the GPU
        that already holds the artwork.

        Returns (bgra_bytes, (x, y, w, h)).
        """
        cv2 = self._cv2
        frame = self.avatar["frame_list_cycle"][idx]
        crop_box = self.avatar["mask_coords_list_cycle"][idx]

        if not self.avatar["valid_cycle"][idx] or crop_box is None:
            # No face at this cycle position. A fully transparent region shows
            # the artwork through untouched, which is what MuseTalk's own
            # renderer does by copying the original frame - and unlike sending
            # nothing, it keeps frames flowing so the staleness fallback does
            # not trip.
            x_s, y_s, x_e, y_e = 0, 0, min(16, frame.shape[1]), min(16, frame.shape[0])
            blank = np.zeros((y_e - y_s, x_e - x_s, 4), dtype=np.uint8)
            return blank.tobytes(), (x_s, y_s, x_e - x_s, y_e - y_s)

        x_s, y_s, x_e, y_e = (int(v) for v in crop_box)
        x1, y1, x2, y2 = (int(v) for v in self.avatar["coord_list_cycle"][idx])

        # get_crop_box expands around the face centre by 1.5x and does not
        # clamp, so on a small or tightly framed avatar the box runs off the
        # edge. MuseTalk gets away with that because PIL's crop pads
        # out-of-bounds; a numpy slice does not - a negative start counts from
        # the far end and yields an empty region, which then fails the resize.
        # Clamping also keeps the rect inside the reference frame, which is
        # what the protocol requires of a CROP.
        frame_h, frame_w = frame.shape[:2]
        cx_s, cy_s = max(0, x_s), max(0, y_s)
        cx_e, cy_e = min(frame_w, x_e), min(frame_h, y_e)

        if cx_e <= cx_s or cy_e <= cy_s:
            blank = np.zeros((1, 1, 4), dtype=np.uint8)
            return blank.tobytes(), (0, 0, 1, 1)

        region = frame[cy_s:cy_e, cx_s:cx_e].copy()

        # With a partial decode the recon is only the bottom of the 256-tall
        # crop, so it lands lower down the bbox by however much was skipped.
        bbox_h = max(1, y2 - y1)
        if self.decode_start:
            skipped_256 = (self.decode_start + self.DECODE_OVERLAP_ROWS) * self.LATENT_SCALE
            top_offset = int(round(skipped_256 * bbox_h / 256.0))
        else:
            top_offset = 0

        try:
            resized = cv2.resize(recon.astype(np.uint8), (x2 - x1, max(1, bbox_h - top_offset)))
        except Exception:
            # A degenerate bbox costs this frame's mouth, not the stream.
            resized = None

        if resized is not None:
            # Paste within the region, clipped to it: the face bbox sits inside
            # crop_box by construction, but a rounded coordinate must not be
            # allowed to write past the edge.
            px, py = x1 - cx_s, y1 - cy_s + top_offset
            h, w = resized.shape[:2]
            ph = min(h, region.shape[0] - py)
            pw = min(w, region.shape[1] - px)
            if px >= 0 and py >= 0 and ph > 0 and pw > 0:
                region[py : py + ph, px : px + pw] = resized[:ph, :pw]

        mask = self.avatar["mask_list_cycle"][idx]
        alpha = np.asarray(mask, dtype=np.uint8)
        if alpha.ndim == 3:
            alpha = alpha[:, :, 0]
        # The mask was built for the unclamped box, so take the window that
        # survived clamping rather than squashing the whole mask into it - a
        # resize here would slide the blend off the mouth.
        mask_y0, mask_x0 = cy_s - y_s, cx_s - x_s
        alpha = alpha[mask_y0 : mask_y0 + region.shape[0], mask_x0 : mask_x0 + region.shape[1]]

        if alpha.shape[:2] != region.shape[:2]:
            alpha = cv2.resize(alpha, (region.shape[1], region.shape[0]), interpolation=cv2.INTER_LINEAR)

        bgra = np.dstack([region, alpha])
        return bgra.tobytes(), (cx_s, cy_s, region.shape[1], region.shape[0])
