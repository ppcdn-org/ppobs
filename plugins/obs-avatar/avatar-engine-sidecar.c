/* Mode A: MuseTalk running in a worker process, reached over a named pipe.
 *
 * The plugin is the pipe *server* and the worker connects to it. That is the
 * whole reason the Python side needs no pywin32: connecting to a named pipe
 * from Python is open(r"\\.\pipe\name", "r+b"), while creating one is not.
 *
 * All pipe I/O is overlapped, not because the throughput needs it - a 256x256
 * BGRA stream at 25 fps is 6.4 MB/s - but because shutdown has to be
 * deterministic. With blocking I/O, a reader parked in ConnectNamedPipe or
 * ReadFile can only be freed by cancelling another thread's synchronous I/O,
 * and getting that subtly wrong means OBS hangs on exit. Here every wait is a
 * WaitForMultipleObjects over {io event, stop event}, so stopping is just
 * SetEvent.
 *
 * It connects, negotiates, streams audio out and frames in. When the config
 * names a worker script it also owns the worker's life: it starts the
 * interpreter, keeps it inside a job object so a crashed OBS cannot orphan a
 * process holding gigabytes of VRAM, and relaunches it with a growing delay
 * when it dies. Without one it only waits, which is how a developer runs the
 * worker in a terminal to watch its output.
 */

#include "avatar-engine-sidecar.h"
#include "avatar-sidecar-protocol.h"

#include <util/platform.h>
#include <util/threading.h>
#include <util/dstr.h>

#include <windows.h>

#define do_log(level, format, ...) \
	blog(level, "[obs-avatar sidecar: '%s'] " format, sidecar_owner_name(sc), ##__VA_ARGS__)

/* One instance is enough: a pipe serves exactly one avatar source, and a
 * second source gets its own pipe with its own name. */
#define SIDECAR_PIPE_INSTANCES 1

/* Generous enough that the kernel buffers a whole frame, so a worker's write
 * does not block waiting for our reader to drain. */
#define SIDECAR_PIPE_IN_BUFFER (2u * 1024u * 1024u)

/* The other direction is kept small on purpose: it is the audio on its way to
 * the worker, and whatever sits in the kernel's buffer is audio the worker has
 * not seen yet. With the 2 MiB the frames need, a worker that cannot keep up
 * is fed half a minute of backlog, the mouth moves to speech from half a
 * minute ago, and once the buffer fills every write blocks. About a second
 * is as much as is worth having in flight. */
#define SIDECAR_PIPE_OUT_BUFFER (64u * 1024u)

/* Audio not yet handed to the pipe, in seconds. The writer thread drains it,
 * so a worker that has stopped reading costs this much memory and no more:
 * when it is full the oldest audio is dropped, because a mouth that is a
 * second late is wrong, and one that is ten seconds late is just broken. */
#define SIDECAR_TX_RING_SECONDS 1

/* Reported at most this often, since a worker that is too slow is too slow on
 * every tick and one line per tick would bury everything else in the log. */
#define SIDECAR_DROP_LOG_NS 5000000000ULL

/* How long to wait before accepting another connection after a worker drops.
 * A crashed worker that is being restarted should not spin this thread. */
#define SIDECAR_RECONNECT_MS 1000

/* How old the newest frame may be before the source stops showing it. A
 * frozen mouth left on stream is worse than a still avatar, because it
 * still looks like it is talking. */
#define SIDECAR_FRAME_STALE_NS 500000000ULL

/* Relaunch delay for a worker the plugin started: doubling from the minimum
 * to the maximum, so a worker that dies on import (a wrong interpreter, a
 * missing package) is retried a few times a minute instead of spinning, while
 * one that crashed once comes back almost at once. A session that lasted
 * SIDECAR_STABLE_NS counts as healthy and resets the delay. */
#define SIDECAR_BACKOFF_MIN_MS 1000
#define SIDECAR_BACKOFF_MAX_MS 30000
#define SIDECAR_STABLE_NS 10000000000ULL

/* A launched worker that has neither connected nor exited by now is hung.
 * The worker connects before it loads any model, so this is not a model
 * loading budget - only how long an interpreter may take to start. */
#define SIDECAR_CONNECT_TIMEOUT_MS 60000

/* How long a worker gets to leave on its own once its session is over (it
 * sees the pipe close), and after a BYE at shutdown. Past this it is killed. */
#define SIDECAR_EXIT_GRACE_MS 1000
#define SIDECAR_BYE_GRACE_MS 2000

struct sidecar {
	obs_source_t *owner;

	char *asset_path;
	uint32_t fps;
	uint32_t sample_rate;

	char *pipe_name;
	HANDLE pipe;

	/* The worker process, when the plugin starts it. Owned by the pipe
	 * thread; destroy only touches these after joining it. */
	bool managed;
	char *worker_python;
	char *worker_script;
	char *worker_engine;
	char *musetalk_dir;
	char *worker_log;
	bool worker_log_started;
	HANDLE worker;
	HANDLE job;
	volatile bool bye_sent;

	HANDLE stop_event;
	HANDLE read_event;
	HANDLE write_event;
	pthread_t thread;
	bool thread_valid;

	/* Writes come from a thread of their own, reads from the reader's, so the
	 * two directions get separate OVERLAPPEDs and the write side is
	 * serialised. The source's tick thread never writes to the pipe: it only
	 * puts audio in the ring below, because a pipe write can wait as long as
	 * the worker takes to read, and the tick runs under the engine lock that
	 * the renderer and the properties dialog also want. */
	pthread_mutex_t write_mutex;
	uint8_t *tx;
	size_t tx_cap;

	pthread_mutex_t tx_mutex;
	float *ring;
	size_t ring_cap;
	size_t ring_len;
	int64_t ring_pts;
	uint64_t ring_dropped;
	uint64_t drop_logged;
	uint64_t drop_reported;
	float mouth_gain;
	bool gain_dirty;
	HANDLE tx_event;
	pthread_t writer;
	bool writer_valid;
	bool bye_pending;

	/* The latest frame, handed to the renderer under this lock. */
	pthread_mutex_t frame_mutex;
	uint8_t *frame;
	size_t frame_cap;
	uint32_t frame_width;
	uint32_t frame_height;
	int64_t frame_pts;
	bool frame_valid;
	uint64_t frame_arrived;

	/* Set when the frame is only the repainted region: where it goes, and
	 * the reference frame it is positioned in. */
	bool frame_is_region;
	uint32_t region_x;
	uint32_t region_y;
	uint32_t ref_width;
	uint32_t ref_height;

	/* Owned by the graphics thread, inside render(). */
	gs_texture_t *texture;
	uint32_t tex_width;
	uint32_t tex_height;
	int64_t uploaded_pts;
	bool uploaded;
	bool stale_logged;

	pthread_mutex_t state_mutex;
	struct avatar_engine_stats stats;

	/* Payload scratch, owned by the reader thread alone. */
	uint8_t *rx;
	size_t rx_cap;
};

/* A source's name is not always available: it is NULL while the source is
 * being torn down, and there is no source at all when the engine is driven
 * directly, as the loopback test drives it. Logging must survive both rather
 * than hand blog() a null pointer. */
static inline const char *sidecar_owner_name(const struct sidecar *sc)
{
	const char *name = sc->owner ? obs_source_get_name(sc->owner) : NULL;
	return name ? name : "(detached)";
}

const char *avatar_engine_sidecar_pipe_name(void *data)
{
	const struct sidecar *sc = data;
	return sc ? sc->pipe_name : NULL;
}

/* ---------------------------------------------------------------------------
 * Overlapped I/O helpers
 * ------------------------------------------------------------------------- */

/* Waits for one overlapped operation, or for the stop request. Returns true
 * when the I/O completed, false when stopping or on error - in which case the
 * pending operation has been cancelled. */
static bool sidecar_wait_io(struct sidecar *sc, OVERLAPPED *ov, HANDLE event, DWORD *transferred)
{
	HANDLE waits[2] = {event, sc->stop_event};
	const DWORD result = WaitForMultipleObjects(2, waits, FALSE, INFINITE);

	if (result != WAIT_OBJECT_0) {
		/* Stopping, or the wait itself failed. Either way the operation
		 * must not be left pending against a buffer we are about to
		 * stop using. */
		CancelIoEx(sc->pipe, ov);
		/* Collect the cancellation so the OVERLAPPED is free to reuse. */
		GetOverlappedResult(sc->pipe, ov, transferred, TRUE);
		return false;
	}

	return GetOverlappedResult(sc->pipe, ov, transferred, FALSE) != 0;
}

/* Reads exactly `len` bytes. A byte-mode pipe may hand back any prefix of what
 * was asked for, which is precisely the partial-message case the framing exists
 * to handle, so this loops rather than trusting one read. */
static bool sidecar_read_exact(struct sidecar *sc, uint8_t *buf, size_t len)
{
	size_t done = 0;

	while (done < len) {
		OVERLAPPED ov = {0};
		ov.hEvent = sc->read_event;
		ResetEvent(sc->read_event);

		const DWORD want = (DWORD)((len - done > MAXDWORD) ? MAXDWORD : (len - done));
		DWORD got = 0;

		if (ReadFile(sc->pipe, buf + done, want, &got, &ov)) {
			done += got;
			continue;
		}

		if (GetLastError() != ERROR_IO_PENDING)
			return false;

		if (!sidecar_wait_io(sc, &ov, sc->read_event, &got))
			return false;

		/* A zero-length completion means the far end closed. */
		if (!got)
			return false;

		done += got;
	}

	return true;
}

/* Caller holds write_mutex. */
static bool sidecar_write_all(struct sidecar *sc, const uint8_t *buf, size_t len)
{
	size_t done = 0;

	while (done < len) {
		OVERLAPPED ov = {0};
		ov.hEvent = sc->write_event;
		ResetEvent(sc->write_event);

		const DWORD want = (DWORD)((len - done > MAXDWORD) ? MAXDWORD : (len - done));
		DWORD put = 0;

		if (WriteFile(sc->pipe, buf + done, want, &put, &ov)) {
			done += put;
			continue;
		}

		if (GetLastError() != ERROR_IO_PENDING)
			return false;

		if (!sidecar_wait_io(sc, &ov, sc->write_event, &put))
			return false;

		if (!put)
			return false;

		done += put;
	}

	return true;
}

static bool sidecar_send(struct sidecar *sc, uint16_t type, const uint8_t *payload, uint32_t payload_len)
{
	uint8_t header[AVATAR_MSG_HEADER_SIZE];
	avatar_msg_header_write(header, type, payload_len);

	pthread_mutex_lock(&sc->write_mutex);
	bool ok = sidecar_write_all(sc, header, sizeof(header));
	if (ok && payload_len)
		ok = sidecar_write_all(sc, payload, payload_len);
	pthread_mutex_unlock(&sc->write_mutex);

	return ok;
}

/* ---------------------------------------------------------------------------
 * Message handling
 * ------------------------------------------------------------------------- */

static bool sidecar_send_hello(struct sidecar *sc)
{
	const char *asset = sc->asset_path ? sc->asset_path : "";
	const uint32_t asset_len = (uint32_t)strlen(asset);
	const uint32_t payload_len = 16 + asset_len;

	uint8_t *payload = bmalloc(payload_len);
	avatar_put_u32(payload, AVATAR_PROTOCOL_VERSION);
	avatar_put_u32(payload + 4, sc->sample_rate);
	avatar_put_u32(payload + 8, sc->fps);
	avatar_put_u32(payload + 12, asset_len);
	if (asset_len)
		memcpy(payload + 16, asset, asset_len);

	const bool ok = sidecar_send(sc, AVATAR_MSG_HELLO, payload, payload_len);
	bfree(payload);
	return ok;
}

static bool sidecar_handle_ready(struct sidecar *sc, const uint8_t *payload, uint32_t len)
{
	if (len < 20) {
		do_log(LOG_WARNING, "READY payload truncated (%u bytes)", len);
		return false;
	}

	const uint32_t version = avatar_get_u32(payload);
	const uint32_t width = avatar_get_u32(payload + 4);
	const uint32_t height = avatar_get_u32(payload + 8);
	const uint32_t flags = avatar_get_u32(payload + 12);
	const uint32_t name_len = avatar_get_u32(payload + 16);

	if (version != AVATAR_PROTOCOL_VERSION) {
		/* Refused rather than tolerated: a half-understood frame stream
		 * would put garbage on the stream, which is worse than no
		 * avatar at all. */
		do_log(LOG_ERROR, "worker speaks protocol %u, this build speaks %d - refusing the connection", version,
		       AVATAR_PROTOCOL_VERSION);
		return false;
	}

	/* This is the reference frame - the coordinate space a CROP's x/y are in
	 * - not a payload, so it is bounded far more loosely than a frame's
	 * edge: a 1080p avatar is a perfectly reasonable reference even though
	 * no single message may carry one. */
	if (!width || !height || width > 8192 || height > 8192) {
		do_log(LOG_ERROR, "worker announced an unusable reference size %ux%u", width, height);
		return false;
	}

	pthread_mutex_lock(&sc->frame_mutex);
	/* The reference frame a CROP's coordinates are relative to. */
	sc->ref_width = width;
	sc->ref_height = height;
	pthread_mutex_unlock(&sc->frame_mutex);

	pthread_mutex_lock(&sc->state_mutex);
	sc->stats.connected = true;
	sc->stats.real_model = (flags & AVATAR_READY_REAL_MODEL) != 0;
	sc->stats.engine[0] = 0;
	if (name_len && 20 + (size_t)name_len <= len) {
		const size_t copy = (name_len < sizeof(sc->stats.engine) - 1) ? name_len
									      : sizeof(sc->stats.engine) - 1;
		memcpy(sc->stats.engine, payload + 20, copy);
		sc->stats.engine[copy] = 0;
	}
	const bool real = sc->stats.real_model;
	pthread_mutex_unlock(&sc->state_mutex);

	do_log(LOG_INFO, "worker ready: %s, %ux%u, %s", sc->stats.engine[0] ? sc->stats.engine : "(unnamed)", width,
	       height, real ? "real model" : "STUB - no model loaded, frames are a placeholder");

	/* A worker that has just (re)started knows nothing of the setting. */
	pthread_mutex_lock(&sc->tx_mutex);
	sc->gain_dirty = true;
	pthread_mutex_unlock(&sc->tx_mutex);
	SetEvent(sc->tx_event);

	return true;
}

static bool sidecar_handle_frame(struct sidecar *sc, const uint8_t *payload, uint32_t len)
{
	if (len < AVATAR_FRAME_HEADER_SIZE)
		return false;

	const int64_t pts = avatar_get_i64(payload);
	const uint32_t width = avatar_get_u32(payload + 8);
	const uint32_t height = avatar_get_u32(payload + 12);

	if (!avatar_frame_payload_valid(len, width, height)) {
		do_log(LOG_WARNING, "discarding a FRAME whose %ux%u does not match its %u byte payload", width, height,
		       len);
		/* The framing is still intact - only this message was wrong - so
		 * the connection survives. */
		return true;
	}

	const size_t pixels = (size_t)width * (size_t)height * 4u;

	pthread_mutex_lock(&sc->frame_mutex);
	if (sc->frame_cap < pixels) {
		sc->frame = brealloc(sc->frame, pixels);
		sc->frame_cap = pixels;
	}
	memcpy(sc->frame, payload + AVATAR_FRAME_HEADER_SIZE, pixels);
	sc->frame_width = width;
	sc->frame_height = height;
	sc->frame_pts = pts;
	sc->frame_valid = true;
	sc->frame_is_region = false;
	sc->frame_arrived = os_gettime_ns();
	pthread_mutex_unlock(&sc->frame_mutex);

	pthread_mutex_lock(&sc->state_mutex);
	sc->stats.frames++;
	pthread_mutex_unlock(&sc->state_mutex);
	return true;
}

/* The repainted region only. Its alpha is the blend mask, so compositing it
 * over the artwork is a plain alpha blend - which is why this can be left to
 * the GPU instead of costing 72ms a frame on the worker's CPU. */
static bool sidecar_handle_crop(struct sidecar *sc, const uint8_t *payload, uint32_t len)
{
	if (len < AVATAR_CROP_HEADER_SIZE)
		return false;

	const int64_t pts = avatar_get_i64(payload);
	const uint32_t x = avatar_get_u32(payload + 8);
	const uint32_t y = avatar_get_u32(payload + 12);
	const uint32_t width = avatar_get_u32(payload + 16);
	const uint32_t height = avatar_get_u32(payload + 20);

	pthread_mutex_lock(&sc->frame_mutex);
	const uint32_t ref_width = sc->ref_width;
	const uint32_t ref_height = sc->ref_height;
	pthread_mutex_unlock(&sc->frame_mutex);

	if (!avatar_crop_payload_valid(len, x, y, width, height, ref_width, ref_height)) {
		do_log(LOG_WARNING, "discarding a CROP of %ux%u at %u,%u that does not fit a %ux%u frame (%u bytes)",
		       width, height, x, y, ref_width, ref_height, len);
		/* The framing is intact, so only this message is lost. */
		return true;
	}

	const size_t pixels = (size_t)width * (size_t)height * 4u;

	pthread_mutex_lock(&sc->frame_mutex);
	if (sc->frame_cap < pixels) {
		sc->frame = brealloc(sc->frame, pixels);
		sc->frame_cap = pixels;
	}
	memcpy(sc->frame, payload + AVATAR_CROP_HEADER_SIZE, pixels);
	sc->frame_width = width;
	sc->frame_height = height;
	sc->frame_pts = pts;
	sc->frame_valid = true;
	sc->frame_is_region = true;
	sc->region_x = x;
	sc->region_y = y;
	sc->frame_arrived = os_gettime_ns();
	pthread_mutex_unlock(&sc->frame_mutex);

	pthread_mutex_lock(&sc->state_mutex);
	sc->stats.frames++;
	pthread_mutex_unlock(&sc->state_mutex);
	return true;
}

static void sidecar_handle_stat(struct sidecar *sc, const uint8_t *payload, uint32_t len)
{
	if (len < AVATAR_STAT_PAYLOAD_SIZE)
		return;

	pthread_mutex_lock(&sc->state_mutex);
	sc->stats.whisper_us = avatar_get_u32(payload);
	sc->stats.unet_us = avatar_get_u32(payload + 4);
	sc->stats.vae_us = avatar_get_u32(payload + 8);
	sc->stats.queue_depth = avatar_get_u32(payload + 12);
	sc->stats.dropped = avatar_get_u32(payload + 16);
	pthread_mutex_unlock(&sc->state_mutex);
}

/* The worker's own log lines, forwarded into obs.log. Without this a separate
 * process fails silently as far as anyone reading the OBS log is concerned. */
static void sidecar_handle_log(struct sidecar *sc, const uint8_t *payload, uint32_t len)
{
	if (len < 4)
		return;

	const uint32_t level = avatar_get_u32(payload);
	int obs_level;

	switch (level) {
	case AVATAR_LOG_ERROR:
		obs_level = LOG_ERROR;
		break;
	case AVATAR_LOG_WARNING:
		obs_level = LOG_WARNING;
		break;
	case AVATAR_LOG_DEBUG:
		obs_level = LOG_DEBUG;
		break;
	default:
		obs_level = LOG_INFO;
		break;
	}

	struct dstr text = {0};
	dstr_ncopy(&text, (const char *)payload + 4, len - 4);
	do_log(obs_level, "worker: %s", text.array ? text.array : "");
	dstr_free(&text);
}

static void sidecar_handle_error(struct sidecar *sc, const uint8_t *payload, uint32_t len)
{
	const uint32_t code = (len >= 4) ? avatar_get_u32(payload) : 0;

	struct dstr text = {0};
	if (len > 4)
		dstr_ncopy(&text, (const char *)payload + 4, len - 4);

	do_log(LOG_ERROR, "worker reported error %u: %s", code, text.array ? text.array : "(no detail)");
	dstr_free(&text);
}

/* ---------------------------------------------------------------------------
 * Session
 * ------------------------------------------------------------------------- */

static bool sidecar_wait_stop(struct sidecar *sc, DWORD ms)
{
	return WaitForSingleObject(sc->stop_event, ms) == WAIT_OBJECT_0;
}

/* ---------------------------------------------------------------------------
 * Worker process
 * ------------------------------------------------------------------------- */

static unsigned sidecar_next_backoff(unsigned ms)
{
	return (ms >= SIDECAR_BACKOFF_MAX_MS / 2) ? SIDECAR_BACKOFF_MAX_MS : ms * 2;
}

/* Everything the worker writes before the handshake - an interpreter that is
 * not there, an import that fails, a traceback - goes to its stdout/stderr, and
 * nobody is attached to those in a GUI process. A file is the one place that
 * output survives to be read. */
static HANDLE sidecar_open_worker_log(struct sidecar *sc, SECURITY_ATTRIBUTES *sa)
{
	if (!sc->worker_log[0])
		return INVALID_HANDLE_VALUE;

	wchar_t *wide = NULL;
	if (os_utf8_to_wcs_ptr(sc->worker_log, 0, &wide) == 0)
		return INVALID_HANDLE_VALUE;

	/* The first launch of this engine starts a fresh file; relaunches append,
	 * because the crash that caused the relaunch is what the file is for. */
	if (!sc->worker_log_started) {
		HANDLE truncate = CreateFileW(wide, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
					      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if (truncate != INVALID_HANDLE_VALUE)
			CloseHandle(truncate);
		sc->worker_log_started = true;
	}

	HANDLE file = CreateFileW(wide, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, sa,
				  OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	bfree(wide);
	return file;
}

static bool sidecar_spawn_worker(struct sidecar *sc)
{
	const char *python = sc->worker_python[0] ? sc->worker_python : "python";

	/* -X utf8 so the log is UTF-8 whatever the console code page is, -u so a
	 * crash does not take its last lines down with it in a buffer. */
	struct dstr cmd = {0};
	dstr_printf(&cmd, "\"%s\" -X utf8 -u \"%s\" --pipe \"%s\" --engine %s", python, sc->worker_script,
		    sc->pipe_name, sc->worker_engine);
	if (sc->musetalk_dir[0])
		dstr_catf(&cmd, " --musetalk-dir \"%s\"", sc->musetalk_dir);

	SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};

	HANDLE nul = CreateFileA("NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
				 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	HANDLE log = sidecar_open_worker_log(sc, &sa);
	HANDLE out = (log != INVALID_HANDLE_VALUE) ? log : nul;

	/* A job that kills its members when the last handle closes. OBS dying
	 * without running its shutdown path would otherwise leave the worker
	 * running with the model resident in VRAM. */
	HANDLE job = CreateJobObjectW(NULL, NULL);
	if (job) {
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
		limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
		SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
	}

	if (log != INVALID_HANDLE_VALUE) {
		struct dstr header = {0};
		dstr_printf(&header, "---- launching worker ----\r\n%s\r\n", cmd.array);
		DWORD written = 0;
		WriteFile(log, header.array, (DWORD)header.len, &written, NULL);
		dstr_free(&header);
	}

	/* Only the handles the child needs are inherited. Without the list,
	 * CreateProcess with inheritance on hands over every inheritable handle
	 * OBS happens to hold. */
	HANDLE inherit[2] = {nul, out};
	const DWORD inherit_count = (nul == out) ? 1 : 2;

	STARTUPINFOEXW si = {0};
	si.StartupInfo.cb = sizeof(si);
	si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
	si.StartupInfo.hStdInput = nul;
	si.StartupInfo.hStdOutput = out;
	si.StartupInfo.hStdError = out;

	SIZE_T attr_size = 0;
	InitializeProcThreadAttributeList(NULL, 1, 0, &attr_size);
	LPPROC_THREAD_ATTRIBUTE_LIST attrs = bmalloc(attr_size);
	bool attrs_ready = InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size) != 0;
	if (attrs_ready &&
	    !UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit,
				       inherit_count * sizeof(HANDLE), NULL, NULL)) {
		DeleteProcThreadAttributeList(attrs);
		attrs_ready = false;
	}
	si.lpAttributeList = attrs;

	wchar_t *wide_cmd = NULL;
	os_utf8_to_wcs_ptr(cmd.array, 0, &wide_cmd);

	PROCESS_INFORMATION pi = {0};
	bool ok = attrs_ready && wide_cmd &&
		  CreateProcessW(NULL, wide_cmd, NULL, NULL, TRUE,
				 CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, NULL, NULL,
				 &si.StartupInfo, &pi);
	const DWORD err = ok ? 0 : GetLastError();

	if (attrs_ready)
		DeleteProcThreadAttributeList(attrs);
	bfree(attrs);
	bfree(wide_cmd);

	if (log != INVALID_HANDLE_VALUE)
		CloseHandle(log);
	if (nul != INVALID_HANDLE_VALUE)
		CloseHandle(nul);

	if (!ok) {
		do_log(LOG_ERROR, "could not start the worker (error %lu): %s", err, cmd.array);
		if (job)
			CloseHandle(job);
		dstr_free(&cmd);
		return false;
	}

	/* Suspended until it is in the job, so it cannot start a child of its own
	 * outside it. */
	if (job && !AssignProcessToJobObject(job, pi.hProcess))
		do_log(LOG_WARNING, "could not put the worker in a job object (%lu); it will outlive OBS if OBS crashes",
		       GetLastError());

	ResumeThread(pi.hThread);
	CloseHandle(pi.hThread);

	sc->worker = pi.hProcess;
	sc->job = job;

	do_log(LOG_INFO, "started the worker (pid %lu): %s", pi.dwProcessId, cmd.array);
	if (sc->worker_log[0])
		do_log(LOG_INFO, "worker output goes to %s", sc->worker_log);

	dstr_free(&cmd);
	return true;
}

/* Waits `grace_ms` for the worker to leave by itself, then removes whatever is
 * left. Closing the job is what makes the removal complete: a virtualenv's
 * python.exe is a launcher that runs the real interpreter as a child, and
 * terminating the launcher alone would leave that child holding the GPU. */
static void sidecar_reap_worker(struct sidecar *sc, DWORD grace_ms)
{
	if (!sc->worker)
		return;

	if (WaitForSingleObject(sc->worker, grace_ms) == WAIT_OBJECT_0) {
		DWORD code = 0;
		GetExitCodeProcess(sc->worker, &code);
		do_log(code == 0 ? LOG_INFO : LOG_WARNING, "worker exited with code %lu (0x%lX)", code, code);
	} else {
		do_log(LOG_INFO, "stopping the worker");
	}

	CloseHandle(sc->worker);
	sc->worker = NULL;

	if (sc->job) {
		CloseHandle(sc->job);
		sc->job = NULL;
	}
}

enum sidecar_connect_result {
	SIDECAR_CONNECT_OK,
	SIDECAR_CONNECT_STOP,
	SIDECAR_CONNECT_FAILED,
	SIDECAR_CONNECT_WORKER_EXITED,
	SIDECAR_CONNECT_TIMEOUT,
};

static enum sidecar_connect_result sidecar_connect(struct sidecar *sc)
{
	OVERLAPPED ov = {0};
	ov.hEvent = sc->read_event;
	ResetEvent(sc->read_event);

	if (ConnectNamedPipe(sc->pipe, &ov))
		return SIDECAR_CONNECT_OK;

	const DWORD err = GetLastError();

	/* A worker that connected between CreateNamedPipe and here is already
	 * attached, which is success rather than an error to recover from. */
	if (err == ERROR_PIPE_CONNECTED)
		return SIDECAR_CONNECT_OK;

	if (err != ERROR_IO_PENDING) {
		do_log(LOG_ERROR, "ConnectNamedPipe failed (%lu)", err);
		return SIDECAR_CONNECT_FAILED;
	}

	/* A worker the plugin started is watched while waiting: if it dies on
	 * import, nothing will ever connect, and an unwatched wait would sit
	 * here forever with the avatar silently not moving. */
	HANDLE waits[3] = {sc->read_event, sc->stop_event, sc->worker};
	const DWORD count = sc->worker ? 3 : 2;
	const DWORD timeout = sc->worker ? SIDECAR_CONNECT_TIMEOUT_MS : INFINITE;

	const DWORD result = WaitForMultipleObjects(count, waits, FALSE, timeout);

	DWORD transferred = 0;
	if (result == WAIT_OBJECT_0)
		return GetOverlappedResult(sc->pipe, &ov, &transferred, FALSE) ? SIDECAR_CONNECT_OK
									       : SIDECAR_CONNECT_FAILED;

	CancelIoEx(sc->pipe, &ov);
	/* A connection can complete in the instant before the cancel lands, in
	 * which case it is a connection and not a cancellation. */
	const bool connected = GetOverlappedResult(sc->pipe, &ov, &transferred, TRUE) != 0;

	if (result == WAIT_OBJECT_0 + 1)
		return SIDECAR_CONNECT_STOP;
	if (connected)
		return SIDECAR_CONNECT_OK;
	if (result == WAIT_OBJECT_0 + 2)
		return SIDECAR_CONNECT_WORKER_EXITED;
	if (result == WAIT_TIMEOUT)
		return SIDECAR_CONNECT_TIMEOUT;
	return SIDECAR_CONNECT_FAILED;
}

static void sidecar_session(struct sidecar *sc)
{
	if (!sidecar_send_hello(sc)) {
		do_log(LOG_WARNING, "worker connected but the handshake could not be sent");
		return;
	}

	bool ready = false;

	for (;;) {
		uint8_t header[AVATAR_MSG_HEADER_SIZE];
		if (!sidecar_read_exact(sc, header, sizeof(header)))
			return;

		struct avatar_msg_header msg;
		if (!avatar_msg_header_read(header, &msg)) {
			/* A byte stream offers nothing to resynchronise against,
			 * so a bad header means the rest is untrustworthy. */
			do_log(LOG_ERROR, "malformed message header; dropping the connection");
			return;
		}

		if (msg.payload_len) {
			if (sc->rx_cap < msg.payload_len) {
				sc->rx = brealloc(sc->rx, msg.payload_len);
				sc->rx_cap = msg.payload_len;
			}
			if (!sidecar_read_exact(sc, sc->rx, msg.payload_len))
				return;
		}

		switch (msg.type) {
		case AVATAR_MSG_READY:
			if (!sidecar_handle_ready(sc, sc->rx, msg.payload_len))
				return;
			ready = true;
			break;

		case AVATAR_MSG_FRAME:
			if (!ready) {
				do_log(LOG_WARNING, "worker sent a frame before READY; dropping the connection");
				return;
			}
			if (!sidecar_handle_frame(sc, sc->rx, msg.payload_len))
				return;
			break;

		case AVATAR_MSG_CROP:
			if (!ready) {
				do_log(LOG_WARNING, "worker sent a crop before READY; dropping the connection");
				return;
			}
			if (!sidecar_handle_crop(sc, sc->rx, msg.payload_len))
				return;
			break;

		case AVATAR_MSG_STAT:
			sidecar_handle_stat(sc, sc->rx, msg.payload_len);
			break;

		case AVATAR_MSG_LOG:
			sidecar_handle_log(sc, sc->rx, msg.payload_len);
			break;

		case AVATAR_MSG_ERROR:
			sidecar_handle_error(sc, sc->rx, msg.payload_len);
			return;

		default:
			/* Forward compatibility: a newer worker's message type is
			 * read and discarded, not treated as a fault. The payload
			 * has already been consumed above, so the stream stays in
			 * sync. */
			do_log(LOG_DEBUG, "ignoring unknown message type %u (%u bytes)", msg.type, msg.payload_len);
			break;
		}
	}
}

static void *sidecar_thread(void *param)
{
	struct sidecar *sc = param;

	os_set_thread_name("avatar-sidecar");

	do_log(LOG_INFO, "waiting for a worker on %s", sc->pipe_name);

	unsigned backoff_ms = SIDECAR_BACKOFF_MIN_MS;

	while (!sidecar_wait_stop(sc, 0)) {
		if (sc->managed && !sc->worker && !sidecar_spawn_worker(sc)) {
			do_log(LOG_WARNING, "trying again in %u ms", backoff_ms);
			if (sidecar_wait_stop(sc, backoff_ms))
				break;
			backoff_ms = sidecar_next_backoff(backoff_ms);
			continue;
		}

		const enum sidecar_connect_result connect = sidecar_connect(sc);
		if (connect == SIDECAR_CONNECT_STOP || connect == SIDECAR_CONNECT_FAILED)
			break;

		bool stable = false;

		if (connect == SIDECAR_CONNECT_OK) {
			const uint64_t started = os_gettime_ns();
			sidecar_session(sc);
			stable = os_gettime_ns() - started >= SIDECAR_STABLE_NS;
		} else if (connect == SIDECAR_CONNECT_WORKER_EXITED) {
			do_log(LOG_ERROR, "the worker exited before connecting; see %s",
			       sc->worker_log[0] ? sc->worker_log : "its console output");
		} else {
			do_log(LOG_ERROR, "the worker did not connect within %d seconds", SIDECAR_CONNECT_TIMEOUT_MS / 1000);
		}

		pthread_mutex_lock(&sc->state_mutex);
		const bool was_connected = sc->stats.connected;
		sc->stats.connected = false;
		pthread_mutex_unlock(&sc->state_mutex);

		/* Audio queued for a worker that is gone must not be delivered to its
		 * replacement as if it were current. */
		pthread_mutex_lock(&sc->tx_mutex);
		sc->ring_len = 0;
		pthread_mutex_unlock(&sc->tx_mutex);

		/* The last frame is kept on purpose rather than cleared: the
		 * source should hold the final expression while the worker is
		 * restarting, not blink to nothing. Staleness handling (hold,
		 * then fall back to mode B) belongs to the source. */

		DisconnectNamedPipe(sc->pipe);

		/* Only an unplanned disconnect is worth a warning. Saying "waiting
		 * for it to come back" while shutting down is a false alarm, and
		 * a false alarm in a log costs someone real time later. */
		if (sidecar_wait_stop(sc, 0))
			break;

		if (!sc->managed) {
			if (was_connected)
				do_log(LOG_WARNING, "worker disconnected; waiting for it to come back");
			if (sidecar_wait_stop(sc, SIDECAR_RECONNECT_MS))
				break;
			continue;
		}

		if (was_connected)
			do_log(LOG_WARNING, "worker disconnected");

		/* Never leave the old process running beside its replacement: two
		 * workers would each load the model, and the second to connect
		 * would find the pipe taken. */
		sidecar_reap_worker(sc, SIDECAR_EXIT_GRACE_MS);

		if (stable)
			backoff_ms = SIDECAR_BACKOFF_MIN_MS;

		do_log(LOG_INFO, "relaunching the worker in %u ms", backoff_ms);
		if (sidecar_wait_stop(sc, backoff_ms))
			break;
		backoff_ms = sidecar_next_backoff(backoff_ms);
	}

	/* After a BYE the worker is leaving on its own and only needs a moment.
	 * Without one it may be in the middle of loading a model and will not
	 * look at the pipe for a long while, which is not worth stalling a
	 * property edit over. */
	sidecar_reap_worker(sc, sc->bye_sent ? SIDECAR_BYE_GRACE_MS : 0);

	return NULL;
}

/* ---------------------------------------------------------------------------
 * Engine ops
 * ------------------------------------------------------------------------- */

static void sidecar_destroy(void *data);
static bool sidecar_ready(void *data);

/* Everything bound for the worker leaves from here. The thread sleeps until
 * there is something to send and then writes it, however long the pipe takes
 * to accept it: it is the only thread that can be stuck behind a slow worker,
 * and nothing else waits on it. */
static void *sidecar_writer_thread(void *param)
{
	struct sidecar *sc = param;

	os_set_thread_name("avatar-sidecar-tx");

	HANDLE waits[2] = {sc->tx_event, sc->stop_event};

	while (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0) {
		for (;;) {
			uint8_t gain_payload[AVATAR_MOUTH_GAIN_PAYLOAD_SIZE];
			bool send_gain = false;
			bool send_bye = false;
			uint32_t audio_len = 0;

			pthread_mutex_lock(&sc->tx_mutex);

			if (sc->gain_dirty) {
				avatar_put_f32(gain_payload, sc->mouth_gain);
				sc->gain_dirty = false;
				send_gain = true;
			}

			size_t count = sc->ring_len;
			if (count > avatar_audio_max_samples())
				count = avatar_audio_max_samples();

			if (count) {
				audio_len = avatar_audio_payload_size((uint32_t)count);
				if (sc->tx_cap < audio_len) {
					sc->tx = brealloc(sc->tx, audio_len);
					sc->tx_cap = audio_len;
				}

				avatar_put_i64(sc->tx, sc->ring_pts);
				avatar_put_u32(sc->tx + 8, (uint32_t)count);
				for (size_t i = 0; i < count; i++)
					avatar_put_f32(sc->tx + AVATAR_AUDIO_HEADER_SIZE + i * 4, sc->ring[i]);

				memmove(sc->ring, sc->ring + count, (sc->ring_len - count) * sizeof(float));
				sc->ring_len -= count;
				sc->ring_pts += (int64_t)count * 1000000000LL / (sc->sample_rate ? sc->sample_rate : 16000);
			}

			/* Last, so everything queued before it still reaches the worker. */
			if (sc->bye_pending && !count) {
				send_bye = true;
				sc->bye_pending = false;
			}

			pthread_mutex_unlock(&sc->tx_mutex);

			if (!send_gain && !audio_len && !send_bye)
				break;

			/* Nothing is sent to a worker that is not there; whatever was
			 * queued for it is simply dropped. */
			if (!sidecar_ready(sc))
				continue;

			bool ok = true;
			if (send_gain)
				ok = sidecar_send(sc, AVATAR_MSG_MOUTH_GAIN, gain_payload, sizeof(gain_payload));
			if (ok && audio_len)
				ok = sidecar_send(sc, AVATAR_MSG_AUDIO, sc->tx, audio_len);
			if (ok && send_bye)
				sc->bye_sent = sidecar_send(sc, AVATAR_MSG_BYE, NULL, 0);

			if (!ok) {
				/* The reader thread notices the same breakage and handles
				 * the reconnect; nothing to do here but stop writing. */
				pthread_mutex_lock(&sc->state_mutex);
				sc->stats.connected = false;
				pthread_mutex_unlock(&sc->state_mutex);
			}

			if (sidecar_wait_stop(sc, 0))
				return NULL;
		}
	}

	return NULL;
}

static void *sidecar_create(const struct avatar_engine_config *cfg, obs_source_t *owner)
{
	struct sidecar *sc = bzalloc(sizeof(*sc));
	sc->owner = owner;
	sc->asset_path = bstrdup(cfg->asset_path ? cfg->asset_path : "");
	sc->fps = cfg->fps;
	sc->sample_rate = cfg->sample_rate;
	sc->pipe = INVALID_HANDLE_VALUE;

	/* The mutexes come first and unwind by hand, because from here on every
	 * failure path goes through sidecar_destroy, which destroys all three -
	 * and destroying one that was never initialised is as undefined as
	 * leaking one. */
	if (pthread_mutex_init(&sc->write_mutex, NULL) != 0) {
		bfree(sc->asset_path);
		bfree(sc);
		return NULL;
	}

	if (pthread_mutex_init(&sc->frame_mutex, NULL) != 0) {
		pthread_mutex_destroy(&sc->write_mutex);
		bfree(sc->asset_path);
		bfree(sc);
		return NULL;
	}

	if (pthread_mutex_init(&sc->state_mutex, NULL) != 0) {
		pthread_mutex_destroy(&sc->frame_mutex);
		pthread_mutex_destroy(&sc->write_mutex);
		bfree(sc->asset_path);
		bfree(sc);
		return NULL;
	}

	if (pthread_mutex_init(&sc->tx_mutex, NULL) != 0) {
		pthread_mutex_destroy(&sc->state_mutex);
		pthread_mutex_destroy(&sc->frame_mutex);
		pthread_mutex_destroy(&sc->write_mutex);
		bfree(sc->asset_path);
		bfree(sc);
		return NULL;
	}

	sc->ring_cap = (size_t)(sc->sample_rate ? sc->sample_rate : 16000) * SIDECAR_TX_RING_SECONDS;
	sc->ring = bmalloc(sc->ring_cap * sizeof(float));
	sc->mouth_gain = cfg->mouth_gain > 0.0f ? cfg->mouth_gain : 1.0f;

	/* The pipe name has to be unique per source: two avatar sources in one
	 * scene would otherwise fight over one pipe, and the second
	 * CreateNamedPipe would hand its worker the first source's audio. */
	struct dstr name = {0};
	dstr_printf(&name, "\\\\.\\pipe\\ppobs-avatar-%lu-%p", GetCurrentProcessId(), (void *)sc);
	sc->pipe_name = name.array;

	sc->stop_event = CreateEvent(NULL, TRUE, FALSE, NULL);
	sc->read_event = CreateEvent(NULL, TRUE, FALSE, NULL);
	sc->write_event = CreateEvent(NULL, TRUE, FALSE, NULL);
	sc->tx_event = CreateEvent(NULL, FALSE, FALSE, NULL);

	if (!sc->stop_event || !sc->read_event || !sc->write_event || !sc->tx_event) {
		blog(LOG_ERROR, "[obs-avatar sidecar] could not create the I/O events");
		sidecar_destroy(sc);
		return NULL;
	}

	sc->pipe = CreateNamedPipeA(sc->pipe_name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
				    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, SIDECAR_PIPE_INSTANCES,
				    SIDECAR_PIPE_OUT_BUFFER, SIDECAR_PIPE_IN_BUFFER, 0, NULL);

	if (sc->pipe == INVALID_HANDLE_VALUE) {
		blog(LOG_ERROR, "[obs-avatar sidecar] CreateNamedPipe('%s') failed (%lu)", sc->pipe_name,
		     GetLastError());
		sidecar_destroy(sc);
		return NULL;
	}

	sc->worker_python = bstrdup(cfg->worker_python ? cfg->worker_python : "");
	sc->worker_script = bstrdup(cfg->worker_script ? cfg->worker_script : "");
	sc->worker_engine = bstrdup(cfg->worker_engine && *cfg->worker_engine ? cfg->worker_engine : "musetalk");
	sc->worker_log = bstrdup(cfg->worker_log ? cfg->worker_log : "");

	/* A trailing backslash would turn the closing quote of the command line
	 * into an escaped one and swallow the arguments after it. */
	sc->musetalk_dir = bstrdup(cfg->musetalk_dir ? cfg->musetalk_dir : "");
	for (size_t len = strlen(sc->musetalk_dir);
	     len && (sc->musetalk_dir[len - 1] == '\\' || sc->musetalk_dir[len - 1] == '/'); len--)
		sc->musetalk_dir[len - 1] = 0;

	if (sc->worker_script[0]) {
		/* Checked once, here, rather than on every relaunch: a path that
		 * is wrong now is wrong until the settings change, and editing the
		 * settings recreates the engine. Falling back to waiting for a
		 * hand-started worker keeps the source usable meanwhile. */
		const bool python_is_path = strpbrk(sc->worker_python, "\\/:") != NULL;

		if (!os_file_exists(sc->worker_script)) {
			blog(LOG_ERROR,
			     "[obs-avatar sidecar] worker script '%s' does not exist; not starting a worker. "
			     "Start one by hand against %s",
			     sc->worker_script, sc->pipe_name);
		} else if (python_is_path && !os_file_exists(sc->worker_python)) {
			blog(LOG_ERROR,
			     "[obs-avatar sidecar] Python interpreter '%s' does not exist; not starting a worker. "
			     "Start one by hand against %s",
			     sc->worker_python, sc->pipe_name);
		} else {
			sc->managed = true;
		}
	}

	if (pthread_create(&sc->thread, NULL, sidecar_thread, sc) != 0) {
		blog(LOG_ERROR, "[obs-avatar sidecar] could not start the pipe thread");
		sidecar_destroy(sc);
		return NULL;
	}
	sc->thread_valid = true;

	if (pthread_create(&sc->writer, NULL, sidecar_writer_thread, sc) != 0) {
		blog(LOG_ERROR, "[obs-avatar sidecar] could not start the pipe writer thread");
		sidecar_destroy(sc);
		return NULL;
	}
	sc->writer_valid = true;

	return sc;
}

static void sidecar_destroy(void *data)
{
	struct sidecar *sc = data;
	if (!sc)
		return;

	/* Say goodbye before pulling the pipe out from under the worker, so it
	 * exits on a BYE it understands instead of on a read error. Best effort
	 * and before the stop event, because every write waits on that event and
	 * would be cancelled the moment it is set. */
	if (sidecar_ready(sc)) {
		/* Through the writer like everything else, and waited for only
		 * briefly: a writer stuck behind a worker that has stopped reading
		 * would hold this up for as long as the stall lasts, and a worker
		 * that is not reading would not have heard the BYE anyway. */
		pthread_mutex_lock(&sc->tx_mutex);
		sc->bye_pending = true;
		pthread_mutex_unlock(&sc->tx_mutex);
		SetEvent(sc->tx_event);

		for (int waited = 0; waited < 30 && !sc->bye_sent; waited++)
			Sleep(10);
	}

	if (sc->stop_event)
		SetEvent(sc->stop_event);

	if (sc->thread_valid)
		pthread_join(sc->thread, NULL);
	if (sc->writer_valid)
		pthread_join(sc->writer, NULL);

	sidecar_reap_worker(sc, 0);

	if (sc->texture) {
		obs_enter_graphics();
		gs_texture_destroy(sc->texture);
		obs_leave_graphics();
	}

	if (sc->pipe != INVALID_HANDLE_VALUE) {
		CancelIoEx(sc->pipe, NULL);
		DisconnectNamedPipe(sc->pipe);
		CloseHandle(sc->pipe);
	}

	if (sc->stop_event)
		CloseHandle(sc->stop_event);
	if (sc->read_event)
		CloseHandle(sc->read_event);
	if (sc->write_event)
		CloseHandle(sc->write_event);
	if (sc->tx_event)
		CloseHandle(sc->tx_event);

	pthread_mutex_destroy(&sc->write_mutex);
	pthread_mutex_destroy(&sc->frame_mutex);
	pthread_mutex_destroy(&sc->state_mutex);
	pthread_mutex_destroy(&sc->tx_mutex);

	bfree(sc->ring);
	bfree(sc->frame);
	bfree(sc->rx);
	bfree(sc->tx);
	bfree(sc->pipe_name);
	bfree(sc->asset_path);
	bfree(sc->worker_python);
	bfree(sc->worker_script);
	bfree(sc->worker_engine);
	bfree(sc->musetalk_dir);
	bfree(sc->worker_log);
	bfree(sc);
}

static bool sidecar_ready(void *data)
{
	struct sidecar *sc = data;

	pthread_mutex_lock(&sc->state_mutex);
	const bool connected = sc->stats.connected;
	pthread_mutex_unlock(&sc->state_mutex);
	return connected;
}

static void sidecar_push_audio(void *data, const float *samples, size_t count, int64_t pts_ns)
{
	struct sidecar *sc = data;

	if (!count || !sidecar_ready(sc))
		return;

	const uint32_t rate = sc->sample_rate ? sc->sample_rate : 16000;
	uint64_t dropped_now = 0;

	pthread_mutex_lock(&sc->tx_mutex);

	/* A block larger than the whole ring keeps its tail, which is the newest. */
	if (count > sc->ring_cap) {
		const size_t skip = count - sc->ring_cap;
		samples += skip;
		pts_ns += (int64_t)skip * 1000000000LL / rate;
		count = sc->ring_cap;
		sc->ring_dropped += skip;
		dropped_now += skip;
	}

	if (sc->ring_len + count > sc->ring_cap) {
		const size_t over = sc->ring_len + count - sc->ring_cap;
		memmove(sc->ring, sc->ring + over, (sc->ring_len - over) * sizeof(float));
		sc->ring_len -= over;
		sc->ring_pts += (int64_t)over * 1000000000LL / rate;
		sc->ring_dropped += over;
		dropped_now += over;
	}

	if (sc->ring_len == 0)
		sc->ring_pts = pts_ns;

	memcpy(sc->ring + sc->ring_len, samples, count * sizeof(float));
	sc->ring_len += count;

	const uint64_t total_dropped = sc->ring_dropped;
	pthread_mutex_unlock(&sc->tx_mutex);

	SetEvent(sc->tx_event);

	if (!dropped_now)
		return;

	/* The one place this is knowable: audio is being produced faster than the
	 * worker takes it. Said once in a while, with the remedy, because the
	 * symptom on stream is a mouth that lags and skips and nothing says why. */
	const uint64_t now = os_gettime_ns();
	if (!sc->drop_logged || now - sc->drop_logged >= SIDECAR_DROP_LOG_NS) {
		const uint64_t since = total_dropped - sc->drop_reported;
		do_log(LOG_WARNING,
		       "the worker is not keeping up with real time: dropped %.1f s of audio since the last report. "
		       "Lower the generated frame rate",
		       (double)since / rate);
		sc->drop_logged = now;
		sc->drop_reported = total_dropped;
	}
}

void avatar_engine_sidecar_set_mouth_gain(void *data, float gain)
{
	struct sidecar *sc = data;
	if (!sc)
		return;

	pthread_mutex_lock(&sc->tx_mutex);
	const bool changed = sc->mouth_gain != gain;
	sc->mouth_gain = gain;
	if (changed)
		sc->gain_dirty = true;
	pthread_mutex_unlock(&sc->tx_mutex);

	if (changed)
		SetEvent(sc->tx_event);
}

static bool sidecar_lock_frame(void *data, struct avatar_engine_frame *out)
{
	struct sidecar *sc = data;

	pthread_mutex_lock(&sc->frame_mutex);

	if (!sc->frame_valid) {
		pthread_mutex_unlock(&sc->frame_mutex);
		return false;
	}

	out->bgra = sc->frame;
	out->width = sc->frame_width;
	out->height = sc->frame_height;
	out->pts_ns = sc->frame_pts;
	out->is_region = sc->frame_is_region;
	out->x = sc->region_x;
	out->y = sc->region_y;
	out->ref_width = sc->ref_width;
	out->ref_height = sc->ref_height;
	return true;
}

static void sidecar_unlock_frame(void *data)
{
	struct sidecar *sc = data;
	pthread_mutex_unlock(&sc->frame_mutex);
}

/* Draws whatever the worker last sent.
 *
 * A CROP is composited over the artwork here rather than worker-side: the
 * paste was over half of mode A's per-frame cost and none of it on the GPU,
 * while the GPU already holds the artwork. A FRAME (the stub's whole picture)
 * simply replaces it.
 *
 * Returns false when there is nothing recent to draw, which makes the source
 * fall back to the still artwork. Staleness lives here rather than in the
 * source because only the engine knows when its last frame arrived. */
static bool sidecar_render(void *data, gs_texture_t *base, uint32_t cx, uint32_t cy)
{
	struct sidecar *sc = data;

	pthread_mutex_lock(&sc->frame_mutex);

	const bool have = sc->frame_valid;
	const bool fresh = have && (os_gettime_ns() - sc->frame_arrived) < SIDECAR_FRAME_STALE_NS;
	const bool is_region = sc->frame_is_region;
	const uint32_t width = sc->frame_width;
	const uint32_t height = sc->frame_height;
	const uint32_t x = sc->region_x;
	const uint32_t y = sc->region_y;
	const uint32_t ref_width = sc->ref_width ? sc->ref_width : width;
	const uint32_t ref_height = sc->ref_height ? sc->ref_height : height;

	if (fresh && sc->frame_pts != sc->uploaded_pts) {
		if (!sc->texture || sc->tex_width != width || sc->tex_height != height) {
			gs_texture_destroy(sc->texture);
			sc->texture = gs_texture_create(width, height, GS_BGRA, 1, NULL, GS_DYNAMIC);
			sc->tex_width = width;
			sc->tex_height = height;
		}
		if (sc->texture) {
			gs_texture_set_image(sc->texture, sc->frame, width * 4, false);
			sc->uploaded_pts = sc->frame_pts;
			sc->uploaded = true;
		}
	}

	pthread_mutex_unlock(&sc->frame_mutex);

	if (!fresh || !sc->uploaded || !sc->texture) {
		if (have && !fresh && !sc->stale_logged) {
			sc->stale_logged = true;
			do_log(LOG_WARNING, "no frame for %llu ms; falling back to the still avatar",
			       (unsigned long long)(SIDECAR_FRAME_STALE_NS / 1000000ULL));
		}
		return false;
	}

	if (sc->stale_logged) {
		sc->stale_logged = false;
		do_log(LOG_INFO, "frames resumed");
	}

	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);

	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);

	if (!is_region) {
		/* A whole picture of its own: it replaces the artwork. */
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);
		gs_effect_set_texture_srgb(gs_effect_get_param_by_name(effect, "image"), sc->texture);
		while (gs_effect_loop(effect, "Draw"))
			gs_draw_sprite(sc->texture, 0, cx, cy);
		gs_blend_state_pop();
		gs_enable_framebuffer_srgb(previous);
		return true;
	}

	if (!base) {
		gs_enable_framebuffer_srgb(previous);
		return false;
	}

	/* Artwork first, premultiplied as it was loaded. */
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	gs_effect_set_texture_srgb(gs_effect_get_param_by_name(effect, "image"), base);
	while (gs_effect_loop(effect, "Draw"))
		gs_draw_sprite(base, 0, cx, cy);
	gs_blend_state_pop();

	/* Then the region, mapped from the reference frame's pixels onto
	 * whatever size the source is drawing at. Straight alpha, because the
	 * mask arrives as an alpha channel rather than premultiplied in. */
	const float sx = (float)cx / (float)ref_width;
	const float sy = (float)cy / (float)ref_height;

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);
	gs_matrix_push();
	gs_matrix_translate3f((float)x * sx, (float)y * sy, 0.0f);

	gs_effect_set_texture_srgb(gs_effect_get_param_by_name(effect, "image"), sc->texture);
	while (gs_effect_loop(effect, "Draw"))
		gs_draw_sprite(sc->texture, 0, (uint32_t)((float)width * sx), (uint32_t)((float)height * sy));

	gs_matrix_pop();
	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(previous);
	return true;
}

static void sidecar_get_stats(void *data, struct avatar_engine_stats *out)
{
	struct sidecar *sc = data;

	pthread_mutex_lock(&sc->state_mutex);
	*out = sc->stats;
	pthread_mutex_unlock(&sc->state_mutex);
}

const struct avatar_engine_ops avatar_engine_sidecar_ops = {
	.id = "sidecar",
	.create = sidecar_create,
	.destroy = sidecar_destroy,
	.ready = sidecar_ready,
	.push_audio = sidecar_push_audio,
	.lock_frame = sidecar_lock_frame,
	.unlock_frame = sidecar_unlock_frame,
	/* Preferred by the source: a region has to be composited over the
	 * artwork, which needs the base texture. lock_frame stays for callers
	 * that want the pixels themselves, such as the loopback test. */
	.render = sidecar_render,
	.get_stats = sidecar_get_stats,
};
