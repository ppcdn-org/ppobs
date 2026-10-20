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
 * This is the stage C skeleton: it connects, negotiates, streams audio out and
 * frames in. Starting the worker process is deliberately not done yet - see
 * the note on worker_command below.
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
#define SIDECAR_PIPE_BUFFER (2u * 1024u * 1024u)

/* How long to wait before accepting another connection after a worker drops.
 * A crashed worker that is being restarted should not spin this thread. */
#define SIDECAR_RECONNECT_MS 1000

struct sidecar {
	obs_source_t *owner;

	char *asset_path;
	uint32_t fps;
	uint32_t sample_rate;

	char *pipe_name;
	HANDLE pipe;

	HANDLE stop_event;
	HANDLE read_event;
	HANDLE write_event;
	pthread_t thread;
	bool thread_valid;

	/* Writes come from the source's tick thread, reads from our own, so the
	 * two directions get separate OVERLAPPEDs and the write side is
	 * serialised. */
	pthread_mutex_t write_mutex;
	uint8_t *tx;
	size_t tx_cap;

	/* The latest frame, handed to the renderer under this lock. */
	pthread_mutex_t frame_mutex;
	uint8_t *frame;
	size_t frame_cap;
	uint32_t frame_width;
	uint32_t frame_height;
	int64_t frame_pts;
	bool frame_valid;

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

	if (!width || !height || width > AVATAR_FRAME_MAX_EDGE || height > AVATAR_FRAME_MAX_EDGE) {
		do_log(LOG_ERROR, "worker announced an unusable frame size %ux%u", width, height);
		return false;
	}

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

static bool sidecar_connect(struct sidecar *sc)
{
	OVERLAPPED ov = {0};
	ov.hEvent = sc->read_event;
	ResetEvent(sc->read_event);

	if (ConnectNamedPipe(sc->pipe, &ov))
		return true;

	const DWORD err = GetLastError();

	/* A worker that connected between CreateNamedPipe and here is already
	 * attached, which is success rather than an error to recover from. */
	if (err == ERROR_PIPE_CONNECTED)
		return true;

	if (err != ERROR_IO_PENDING) {
		do_log(LOG_ERROR, "ConnectNamedPipe failed (%lu)", err);
		return false;
	}

	DWORD transferred = 0;
	return sidecar_wait_io(sc, &ov, sc->read_event, &transferred);
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

	while (WaitForSingleObject(sc->stop_event, 0) != WAIT_OBJECT_0) {
		if (!sidecar_connect(sc))
			break;

		sidecar_session(sc);

		pthread_mutex_lock(&sc->state_mutex);
		const bool was_connected = sc->stats.connected;
		sc->stats.connected = false;
		pthread_mutex_unlock(&sc->state_mutex);

		/* The last frame is kept on purpose rather than cleared: the
		 * source should hold the final expression while the worker is
		 * restarting, not blink to nothing. Staleness handling (hold,
		 * then fall back to mode B) belongs to the source. */

		DisconnectNamedPipe(sc->pipe);

		/* Only an unplanned disconnect is worth a warning. Saying "waiting
		 * for it to come back" while shutting down is a false alarm, and
		 * a false alarm in a log costs someone real time later. */
		const bool stopping = WaitForSingleObject(sc->stop_event, 0) == WAIT_OBJECT_0;
		if (was_connected && !stopping)
			do_log(LOG_WARNING, "worker disconnected; waiting for it to come back");

		if (stopping || WaitForSingleObject(sc->stop_event, SIDECAR_RECONNECT_MS) == WAIT_OBJECT_0)
			break;
	}

	return NULL;
}

/* ---------------------------------------------------------------------------
 * Engine ops
 * ------------------------------------------------------------------------- */

static void sidecar_destroy(void *data);
static bool sidecar_ready(void *data);

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

	/* The pipe name has to be unique per source: two avatar sources in one
	 * scene would otherwise fight over one pipe, and the second
	 * CreateNamedPipe would hand its worker the first source's audio. */
	struct dstr name = {0};
	dstr_printf(&name, "\\\\.\\pipe\\ppobs-avatar-%lu-%p", GetCurrentProcessId(), (void *)sc);
	sc->pipe_name = name.array;

	sc->stop_event = CreateEvent(NULL, TRUE, FALSE, NULL);
	sc->read_event = CreateEvent(NULL, TRUE, FALSE, NULL);
	sc->write_event = CreateEvent(NULL, TRUE, FALSE, NULL);

	if (!sc->stop_event || !sc->read_event || !sc->write_event) {
		blog(LOG_ERROR, "[obs-avatar sidecar] could not create the I/O events");
		sidecar_destroy(sc);
		return NULL;
	}

	sc->pipe = CreateNamedPipeA(sc->pipe_name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
				    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, SIDECAR_PIPE_INSTANCES,
				    SIDECAR_PIPE_BUFFER, SIDECAR_PIPE_BUFFER, 0, NULL);

	if (sc->pipe == INVALID_HANDLE_VALUE) {
		blog(LOG_ERROR, "[obs-avatar sidecar] CreateNamedPipe('%s') failed (%lu)", sc->pipe_name,
		     GetLastError());
		sidecar_destroy(sc);
		return NULL;
	}

	if (cfg->worker_command && *cfg->worker_command) {
		/* Spawning, with the backoff relaunch the design calls for, is
		 * the next piece of work. Until then this is reported rather
		 * than silently ignored, so a configured command that does
		 * nothing is not mistaken for a broken worker. */
		blog(LOG_WARNING, "[obs-avatar sidecar] worker_command is set but launching is not implemented yet; "
				  "start the worker by hand against the pipe named in the log");
	}

	if (pthread_create(&sc->thread, NULL, sidecar_thread, sc) != 0) {
		blog(LOG_ERROR, "[obs-avatar sidecar] could not start the pipe thread");
		sidecar_destroy(sc);
		return NULL;
	}
	sc->thread_valid = true;

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
	if (sidecar_ready(sc))
		sidecar_send(sc, AVATAR_MSG_BYE, NULL, 0);

	if (sc->stop_event)
		SetEvent(sc->stop_event);

	if (sc->thread_valid)
		pthread_join(sc->thread, NULL);

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

	pthread_mutex_destroy(&sc->write_mutex);
	pthread_mutex_destroy(&sc->frame_mutex);
	pthread_mutex_destroy(&sc->state_mutex);

	bfree(sc->frame);
	bfree(sc->rx);
	bfree(sc->tx);
	bfree(sc->pipe_name);
	bfree(sc->asset_path);
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

	if (count > avatar_audio_max_samples())
		count = avatar_audio_max_samples();

	const uint32_t payload_len = avatar_audio_payload_size((uint32_t)count);

	if (sc->tx_cap < payload_len) {
		sc->tx = brealloc(sc->tx, payload_len);
		sc->tx_cap = payload_len;
	}

	avatar_put_i64(sc->tx, pts_ns);
	avatar_put_u32(sc->tx + 8, (uint32_t)count);
	for (size_t i = 0; i < count; i++)
		avatar_put_f32(sc->tx + AVATAR_AUDIO_HEADER_SIZE + i * 4, samples[i]);

	if (!sidecar_send(sc, AVATAR_MSG_AUDIO, sc->tx, payload_len)) {
		/* The reader thread notices the same breakage and handles the
		 * reconnect; nothing to do here but stop writing. */
		pthread_mutex_lock(&sc->state_mutex);
		sc->stats.connected = false;
		pthread_mutex_unlock(&sc->state_mutex);
	}
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
	return true;
}

static void sidecar_unlock_frame(void *data)
{
	struct sidecar *sc = data;
	pthread_mutex_unlock(&sc->frame_mutex);
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
	.get_stats = sidecar_get_stats,
};
