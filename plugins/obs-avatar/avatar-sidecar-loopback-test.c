/* Drives the real sidecar client against the real Python worker.
 *
 * avatar-protocol-test.c pins the bytes and the worker's --selftest pins the
 * same bytes from Python, but neither makes the two ends talk. This does: it
 * creates the engine exactly as the source will, starts the worker as a child
 * process, and then checks the things only a live session can show - the
 * handshake completes, audio reaches the worker, frames come back, and
 * shutdown is clean.
 *
 * The assertion that matters most is causality: a frame generated from a loud
 * window and a frame generated from silence must differ, and differ in the
 * mouth specifically. Frames merely arriving would prove the pipe works while
 * the audio was being ignored.
 *
 * Needs Python, which not every machine building OBS has, so a missing or
 * unstartable interpreter SKIPs loudly rather than failing. Set
 * AVATAR_TEST_PYTHON to choose the interpreter.
 *
 * By default it drives the stub engine, which runs anywhere. Pointed at the
 * real one it verifies the half of the protocol the stub never exercises -
 * CROP, where the worker sends only the repainted region and the plugin
 * composites it:
 *
 *   set AVATAR_TEST_ENGINE=musetalk
 *   set AVATAR_TEST_ASSET=D:\media\images\greg02.jpg
 *   set AVATAR_TEST_PYTHON=D:\github\MuseTalk\venv\Scripts\python.exe
 *
 * That path loads 3.4GB of weights and prepares the avatar before the first
 * frame, so it is given a much longer handshake timeout and is not something
 * to run on every build.
 */

#include "avatar-engine-sidecar.h"
#include "avatar-sidecar-protocol.h"
#include "avatar-test-check.h"

#include <util/dstr.h>
#include <util/platform.h>

#include <windows.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef AVATAR_WORKER_SCRIPT
#define AVATAR_WORKER_SCRIPT ""
#endif

#define TEST_FPS 25
#define TEST_SAMPLE_RATE 16000
#define TEST_CHUNK 640 /* one frame's worth of audio at 25 fps */

#define READY_TIMEOUT_MS 30000
/* The real engine loads 3.4GB and prepares the avatar before it answers;
 * 54.8s measured, so this leaves room for a cold page cache. */
#define READY_TIMEOUT_REAL_MS 180000
#define FRAME_TIMEOUT_MS 15000
#define EXIT_TIMEOUT_MS 10000
#define POLL_MS 25

/* The stub's mouth colour, BGRA. Coupling the test to it is deliberate:
 * counting mouth pixels is what turns "a frame arrived" into "this frame was
 * generated from that audio". Both sides of the pair live in this repo, so if
 * the stub's palette changes this test is the right thing to fail. */
#define MOUTH_B 20
#define MOUTH_G 20
#define MOUTH_R 90

struct captured {
	uint8_t *bgra;
	size_t size;
	uint32_t width;
	uint32_t height;
	int64_t pts_ns;
};

static void skip(const char *why)
{
	printf("SKIP: %s\n", why);
	fflush(stdout);
	exit(0);
}

static uint64_t elapsed_ms(uint64_t since_ns)
{
	return (os_gettime_ns() - since_ns) / 1000000ULL;
}

static bool spawn_worker(const char *python, const char *engine, const char *pipe_name, PROCESS_INFORMATION *pi)
{
	struct dstr cmd = {0};
	dstr_printf(&cmd, "\"%s\" \"%s\" --pipe \"%s\" --engine %s", python, AVATAR_WORKER_SCRIPT, pipe_name,
		    engine);

	printf("starting worker: %s\n", cmd.array);
	fflush(stdout);

	STARTUPINFOA si = {0};
	si.cb = sizeof(si);

	const BOOL ok = CreateProcessA(NULL, cmd.array, NULL, NULL, FALSE, 0, NULL, NULL, &si, pi);
	const DWORD err = ok ? 0 : GetLastError();
	dstr_free(&cmd);

	if (!ok)
		printf("CreateProcess failed (%lu)\n", err);

	return ok != 0;
}

/* Copies the engine's current frame out from under its lock. The lock must be
 * held for as little as possible - the reader thread blocks on it. */
static bool capture(void *engine, struct captured *out)
{
	struct avatar_engine_frame frame;
	if (!avatar_engine_sidecar_ops.lock_frame(engine, &frame))
		return false;

	const size_t size = (size_t)frame.width * frame.height * 4u;
	if (out->size < size) {
		out->bgra = brealloc(out->bgra, size);
		out->size = size;
	}
	memcpy(out->bgra, frame.bgra, size);
	out->width = frame.width;
	out->height = frame.height;
	out->pts_ns = frame.pts_ns;

	avatar_engine_sidecar_ops.unlock_frame(engine);
	return true;
}

static size_t count_mouth_pixels(const struct captured *cap)
{
	size_t count = 0;
	const size_t pixels = (size_t)cap->width * cap->height;

	for (size_t i = 0; i < pixels; i++) {
		const uint8_t *px = cap->bgra + i * 4;
		if (px[0] == MOUTH_B && px[1] == MOUTH_G && px[2] == MOUTH_R)
			count++;
	}

	return count;
}

/* Pushes `chunks` chunks of audio, advancing the timestamp the way the source
 * will: a running audio timeline, not wall-clock. `amplitude` of zero is
 * digital silence. */
static int64_t push_audio(void *engine, int64_t pts_ns, int chunks, float amplitude)
{
	float samples[TEST_CHUNK];

	for (int c = 0; c < chunks; c++) {
		for (int i = 0; i < TEST_CHUNK; i++) {
			const double t = (double)(c * TEST_CHUNK + i) / (double)TEST_SAMPLE_RATE;
			samples[i] = (float)(amplitude * sin(2.0 * 3.14159265358979 * 300.0 * t));
		}

		avatar_engine_sidecar_ops.push_audio(engine, samples, TEST_CHUNK, pts_ns);
		pts_ns += (int64_t)TEST_CHUNK * 1000000000LL / TEST_SAMPLE_RATE;
	}

	return pts_ns;
}

/* Waits until the current frame either has a mouth drawn or has none. */
static bool wait_for_mouth(void *engine, bool want_mouth, struct captured *cap)
{
	const uint64_t started = os_gettime_ns();

	while (elapsed_ms(started) < FRAME_TIMEOUT_MS) {
		if (capture(engine, cap)) {
			const size_t mouth = count_mouth_pixels(cap);
			if ((mouth > 0) == want_mouth)
				return true;
		}
		os_sleep_ms(POLL_MS);
	}

	return false;
}

int main(void)
{
	if (!AVATAR_WORKER_SCRIPT[0] || !os_file_exists(AVATAR_WORKER_SCRIPT))
		skip("the worker script was not found (AVATAR_WORKER_SCRIPT)");

	const char *python = getenv("AVATAR_TEST_PYTHON");
	if (!python || !*python)
		python = "python";

	/* The stub needs no artwork and runs anywhere; the real engine needs
	 * both an asset to prepare and MuseTalk's own interpreter. */
	const char *engine_name = getenv("AVATAR_TEST_ENGINE");
	if (!engine_name || !*engine_name)
		engine_name = "stub";

	const char *asset = getenv("AVATAR_TEST_ASSET");
	if (!asset)
		asset = "";

	const bool real = strcmp(engine_name, "stub") != 0;
	if (real && !*asset)
		skip("AVATAR_TEST_ENGINE is set but AVATAR_TEST_ASSET is not");

	printf("engine: %s%s%s\n", engine_name, *asset ? ", asset: " : "", asset);

	const struct avatar_engine_config cfg = {
		.asset_path = asset,
		/* No worker_script: the test starts the worker itself, which is
		 * also how a developer drives the skeleton by hand. */
		.fps = TEST_FPS,
		.sample_rate = TEST_SAMPLE_RATE,
	};

	void *engine = avatar_engine_sidecar_ops.create(&cfg, NULL);
	CHECK(engine != NULL);

	const char *pipe_name = avatar_engine_sidecar_pipe_name(engine);
	CHECK(pipe_name != NULL);
	CHECK(pipe_name[0] != 0);
	printf("pipe: %s\n", pipe_name);

	/* Nothing has connected yet, so the engine must say so rather than
	 * pretend to be usable. */
	CHECK(!avatar_engine_sidecar_ops.ready(engine));

	PROCESS_INFORMATION pi = {0};
	if (!spawn_worker(python, engine_name, pipe_name, &pi)) {
		avatar_engine_sidecar_ops.destroy(engine);
		skip("could not start the Python interpreter (set AVATAR_TEST_PYTHON)");
	}

	struct captured open_frame = {0};
	struct captured shut_frame = {0};
	int status = 1;

	/* Declared up here rather than where it is first used: the error paths
	 * below jump to `done`, and a jump over an initialised declaration is
	 * the kind of thing a compiler is entitled to refuse. */
	size_t open_mouth = 0;

	/* --- handshake --- */
	const uint64_t ready_timeout = real ? READY_TIMEOUT_REAL_MS : READY_TIMEOUT_MS;
	const uint64_t connect_started = os_gettime_ns();
	while (!avatar_engine_sidecar_ops.ready(engine) && elapsed_ms(connect_started) < ready_timeout)
		os_sleep_ms(POLL_MS);

	CHECK(avatar_engine_sidecar_ops.ready(engine));

	struct avatar_engine_stats stats;
	avatar_engine_sidecar_ops.get_stats(engine, &stats);
	CHECK(stats.connected);
	/* Whether the worker is the model or the stub has to be reported
	 * honestly, because the source uses this flag to decide whether the
	 * avatar can be trusted on stream. */
	CHECK(stats.real_model == real);
	if (!real)
		CHECK(strstr(stats.engine, "stub") != NULL);
	printf("handshake ok: engine='%s', real_model=%d\n", stats.engine, (int)stats.real_model);

	/* The real engine finds a face in the artwork and says where the eyes are, so
	 * the plugin can blink them; the stub has no face to find. */
	if (real) {
		struct avatar_eyes eyes = {0};
		const uint64_t eyes_started = os_gettime_ns();
		while (!avatar_engine_sidecar_get_eyes(engine, &eyes) && elapsed_ms(eyes_started) < 5000)
			os_sleep_ms(POLL_MS);

		CHECK(eyes.valid);
		CHECK(eyes.count == 2);
		for (uint32_t i = 0; i < eyes.count; i++) {
			const struct avatar_eye *eye = &eyes.eye[i];
			CHECK(eye->left < eye->right);
			CHECK(eye->right <= (float)eyes.ref_width);
			CHECK(eye->eh >= 1.0f);
			/* The lids' curves have to put the upper lid above the lower at the eye's middle. */
			const float x = 0.0f;
			const float up = eye->up[0] * x * x + eye->up[1] * x + eye->up[2];
			const float lo = eye->lo[0] * x * x + eye->lo[1] * x + eye->lo[2];
			CHECK(up < lo);
		}
		printf("eyes: %u, first %.0f-%.0f px wide at x %.0f, opens %.1f px\n", eyes.count, eyes.eye[0].left,
		       eyes.eye[0].right, eyes.eye[0].cx, eyes.eye[0].eh);
	} else {
		struct avatar_eyes eyes = {0};
		CHECK(!avatar_engine_sidecar_get_eyes(engine, &eyes));
	}

	/* --- audio out, frames back --- */
	int64_t pts = 0;
	/* One second, loud - or two for the real engine, which holds back a window
	 * and a stretch of look-ahead (together over a second) before its first frame. */
	pts = push_audio(engine, pts, real ? TEST_FPS * 2 : TEST_FPS, 0.5f);

	if (real) {
		/* The real engine repaints a region of the artwork rather than
		 * producing a picture, so what is checked here is the part the
		 * stub can never exercise: that CROP arrives, that it is marked
		 * as a region, and that it lands inside the reference frame -
		 * the plugin has to place it there, so a rect that did not fit
		 * would be a texture drawn off the artwork. */
		const uint64_t waited = os_gettime_ns();
		bool got = false;
		while (!got && elapsed_ms(waited) < FRAME_TIMEOUT_MS) {
			got = capture(engine, &open_frame);
			if (!got)
				os_sleep_ms(POLL_MS);
		}

		if (!got) {
			printf("FAIL: no region arrived from the real engine\n");
			goto done;
		}

		struct avatar_engine_frame frame;
		CHECK(avatar_engine_sidecar_ops.lock_frame(engine, &frame));
		const bool is_region = frame.is_region;
		const uint32_t rx = frame.x, ry = frame.y;
		const uint32_t rw = frame.width, rh = frame.height;
		const uint32_t fw = frame.ref_width, fh = frame.ref_height;
		avatar_engine_sidecar_ops.unlock_frame(engine);

		printf("region: %ux%u at %u,%u in %ux%u, pts=%lld ns\n", rw, rh, rx, ry, fw, fh,
		       (long long)open_frame.pts_ns);

		CHECK(is_region);
		CHECK(rw > 0 && rh > 0);
		CHECK(fw > 0 && fh > 0);
		CHECK(rx + rw <= fw);
		CHECK(ry + rh <= fh);
		CHECK(open_frame.pts_ns >= 0);

		avatar_engine_sidecar_ops.get_stats(engine, &stats);
		printf("stats: frames=%llu\n", (unsigned long long)stats.frames);
		CHECK(stats.frames > 0);

		status = 0;
		goto done;
	}

	if (!wait_for_mouth(engine, true, &open_frame)) {
		printf("FAIL: no frame with an open mouth arrived\n");
		goto done;
	}

	CHECK(open_frame.width == 256);
	CHECK(open_frame.height == 256);
	/* The frame has to be stamped with the audio window it came from, not
	 * the time it arrived - that timestamp is what stage D's sync offset
	 * gets measured against. */
	CHECK(open_frame.pts_ns >= 0);
	CHECK(open_frame.pts_ns < pts);

	open_mouth = count_mouth_pixels(&open_frame);
	printf("open frame: %ux%u, pts=%lld ns, mouth=%zu px\n", open_frame.width, open_frame.height,
	       (long long)open_frame.pts_ns, open_mouth);
	CHECK(open_mouth > 0);

	/* --- silence must close the mouth --- */
	pts = push_audio(engine, pts, TEST_FPS, 0.0f);

	if (!wait_for_mouth(engine, false, &shut_frame)) {
		printf("FAIL: the mouth never closed on silence\n");
		goto done;
	}

	printf("shut frame: pts=%lld ns, mouth=%zu px\n", (long long)shut_frame.pts_ns,
	       count_mouth_pixels(&shut_frame));

	/* This pair is the point of the whole test: the pixels changed because
	 * the audio changed. Frames arriving on their own would prove the pipe
	 * works while the audio was being ignored. */
	CHECK(count_mouth_pixels(&shut_frame) == 0);
	CHECK(open_frame.size == shut_frame.size);
	CHECK(memcmp(open_frame.bgra, shut_frame.bgra, open_frame.size) != 0);
	CHECK(shut_frame.pts_ns > open_frame.pts_ns);

	avatar_engine_sidecar_ops.get_stats(engine, &stats);
	printf("stats: frames=%llu dropped=%llu queue=%u\n", (unsigned long long)stats.frames,
	       (unsigned long long)stats.dropped, stats.queue_depth);
	/* Two seconds of audio at 25 fps is fifty windows; allow for the last
	 * few still being in flight. */
	CHECK(stats.frames >= TEST_FPS);

	status = 0;

done:
	/* --- clean shutdown --- */
	avatar_engine_sidecar_ops.destroy(engine);

	/* destroy sends BYE, so the worker should exit by agreement rather than
	 * by tripping over a closed pipe. */
	const DWORD wait = WaitForSingleObject(pi.hProcess, EXIT_TIMEOUT_MS);
	if (wait == WAIT_OBJECT_0) {
		DWORD code = 1;
		GetExitCodeProcess(pi.hProcess, &code);
		printf("worker exited with code %lu\n", code);
		if (status == 0)
			CHECK(code == 0);
	} else {
		printf("FAIL: the worker did not exit after BYE\n");
		TerminateProcess(pi.hProcess, 1);
		status = 1;
	}

	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);

	bfree(open_frame.bgra);
	bfree(shut_frame.bgra);

	printf(status == 0 ? "loopback: ok\n" : "loopback: FAILED\n");
	return status;
}
