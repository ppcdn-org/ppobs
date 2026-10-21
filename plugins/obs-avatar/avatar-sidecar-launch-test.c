/* Drives the sidecar's own management of the worker process: starting it,
 * noticing it die, relaunching it with a delay that grows, and not leaving it
 * behind.
 *
 * The loopback test starts the worker itself, so it proves the conversation but
 * not any of this. This one hands the engine a script and an interpreter and
 * lets it do the starting. The stub engine stands in for MuseTalk, because what
 * is under test is the process handling, not the model.
 *
 * Needs Python, like the loopback test, and SKIPs loudly without it. Set
 * AVATAR_TEST_PYTHON to choose the interpreter.
 */

#include "avatar-engine-sidecar.h"
#include "avatar-test-check.h"

#include <util/dstr.h>
#include <util/platform.h>

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef AVATAR_WORKER_SCRIPT
#define AVATAR_WORKER_SCRIPT ""
#endif

#define TEST_FPS 25
#define TEST_SAMPLE_RATE 16000

#define READY_TIMEOUT_MS 30000
#define POLL_MS 25

/* The first relaunch waits SIDECAR_BACKOFF_MIN_MS, the next twice that. */
#define CRASH_LOOP_WATCH_MS 5000

#define LAUNCH_MARKER "---- launching worker ----"

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

static bool wait_ready(void *engine, bool want, uint64_t timeout_ms)
{
	const uint64_t started = os_gettime_ns();
	while (avatar_engine_sidecar_ops.ready(engine) != want && elapsed_ms(started) < timeout_ms)
		os_sleep_ms(POLL_MS);
	return avatar_engine_sidecar_ops.ready(engine) == want;
}

static int count_in_file(const char *path, const char *needle)
{
	char *text = os_quick_read_utf8_file(path);
	if (!text)
		return 0;

	int count = 0;
	for (const char *at = strstr(text, needle); at; at = strstr(at + strlen(needle), needle))
		count++;

	bfree(text);
	return count;
}

/* How many python processes are running with this pipe on their command line.
 * Asked of PowerShell because that is the only place the command line of
 * another process is readable without a debug privilege. The query's own
 * command line contains the pattern too, hence the name filter. */
static int count_workers(const char *pipe_name, bool kill)
{
	const char *leaf = strrchr(pipe_name, '\\');
	leaf = leaf ? leaf + 1 : pipe_name;

	struct dstr cmd = {0};
	dstr_printf(&cmd,
		    "powershell -NoProfile -Command \"$p = @(Get-CimInstance Win32_Process | "
		    "Where-Object { $_.Name -like 'python*' -and $_.CommandLine -like '*%s*' }); "
		    "if (%s) { $p | ForEach-Object { Stop-Process -Id $_.ProcessId -Force } }; exit $p.Count\"",
		    leaf, kill ? "$true" : "$false");

	const int count = system(cmd.array);
	dstr_free(&cmd);
	return count;
}

static struct avatar_engine_config make_config(const char *script, const char *python, const char *log)
{
	struct avatar_engine_config cfg = {
		.worker_script = script,
		.worker_python = python,
		.worker_engine = "stub",
		.worker_log = log,
		.fps = TEST_FPS,
		.sample_rate = TEST_SAMPLE_RATE,
	};
	return cfg;
}

int main(void)
{
	if (!AVATAR_WORKER_SCRIPT[0] || !os_file_exists(AVATAR_WORKER_SCRIPT))
		skip("the worker script was not found (AVATAR_WORKER_SCRIPT)");

	const char *python = getenv("AVATAR_TEST_PYTHON");
	if (!python)
		python = "";

	struct dstr probe = {0};
	/* The outer quotes are for cmd.exe, which strips one pair from a command
	 * line that starts with a quote and would otherwise mangle the rest. */
	dstr_printf(&probe, "\"\"%s\" -c \"pass\" >nul 2>&1\"", *python ? python : "python");
	const int probe_rc = system(probe.array);
	dstr_free(&probe);
	if (probe_rc != 0)
		skip("could not run the Python interpreter (set AVATAR_TEST_PYTHON)");

	char temp[MAX_PATH];
	GetTempPathA(sizeof(temp), temp);

	struct dstr log_path = {0};
	dstr_printf(&log_path, "%savatar-launch-test.log", temp);
	struct dstr crash_script = {0};
	dstr_printf(&crash_script, "%savatar-launch-test-crash.py", temp);
	struct dstr crash_log = {0};
	dstr_printf(&crash_log, "%savatar-launch-test-crash.log", temp);

	/* --- the engine starts a worker, and brings it back when it dies --- */
	{
		struct avatar_engine_config cfg = make_config(AVATAR_WORKER_SCRIPT, python, log_path.array);
		void *engine = avatar_engine_sidecar_ops.create(&cfg, NULL);
		CHECK(engine != NULL);

		const char *pipe_name = avatar_engine_sidecar_pipe_name(engine);
		CHECK(pipe_name != NULL);

		CHECK(wait_ready(engine, true, READY_TIMEOUT_MS));
		printf("worker started by the engine and connected\n");
		CHECK(count_in_file(log_path.array, LAUNCH_MARKER) == 1);
		CHECK(count_workers(pipe_name, false) >= 1);

		/* Kill it from outside, the way a crash or the task manager would. */
		CHECK(count_workers(pipe_name, true) >= 1);

		CHECK(wait_ready(engine, false, 10000));
		printf("engine noticed the worker was gone\n");

		CHECK(wait_ready(engine, true, READY_TIMEOUT_MS));
		printf("engine relaunched the worker and it connected again\n");
		CHECK(count_in_file(log_path.array, LAUNCH_MARKER) == 2);
		CHECK(count_workers(pipe_name, false) >= 1);

		/* Shutting down must take the worker with it, by BYE and not by
		 * waiting for something to time out. */
		const uint64_t destroy_started = os_gettime_ns();
		avatar_engine_sidecar_ops.destroy(engine);
		const uint64_t destroy_ms = elapsed_ms(destroy_started);
		printf("destroy took %llu ms\n", (unsigned long long)destroy_ms);
		CHECK(destroy_ms < 3000);

		/* Process teardown is asynchronous; give the OS a moment. */
		int left = 1;
		for (int i = 0; i < 40 && left; i++) {
			left = count_workers(pipe_name, false);
			if (left)
				os_sleep_ms(100);
		}
		CHECK(left == 0);
	}

	/* --- frames that arrive together are shown one at a time ---
	 *
	 * The worker produces frames in bursts, and the picture used to show the
	 * newest of each: a mouth that moved a few times a second. A second of audio
	 * pushed at once makes the stub emit about 25 frames together; played out
	 * at the frame rate they take about a second to show, so the displayed
	 * timestamp must walk through them rather than jump to the end. */
	{
		struct avatar_engine_config cfg = make_config(AVATAR_WORKER_SCRIPT, python, log_path.array);
		void *engine = avatar_engine_sidecar_ops.create(&cfg, NULL);
		CHECK(engine != NULL);
		CHECK(wait_ready(engine, true, READY_TIMEOUT_MS));

		float block[4096];
		for (int i = 0; i < 4096; i++)
			block[i] = 0.4f;

		int64_t pts = 0;
		for (int i = 0; i < 4; i++) {
			avatar_engine_sidecar_ops.push_audio(engine, block, 4096, pts);
			pts += 4096LL * 1000000000LL / TEST_SAMPLE_RATE;
		}

		int64_t seen[256];
		int seen_count = 0;
		int64_t last = -1;
		bool monotonic = true;

		const uint64_t watch_started = os_gettime_ns();
		while (elapsed_ms(watch_started) < 2500) {
			struct avatar_engine_frame frame;
			if (avatar_engine_sidecar_ops.lock_frame(engine, &frame)) {
				const int64_t now_pts = frame.pts_ns;
				avatar_engine_sidecar_ops.unlock_frame(engine);

				if (now_pts != last) {
					if (now_pts < last)
						monotonic = false;
					if (seen_count < 256)
						seen[seen_count++] = now_pts;
					last = now_pts;
				}
			}
			os_sleep_ms(5);
		}

		printf("a burst of frames was shown as %d distinct pictures\n", seen_count);
		CHECK(monotonic);
		CHECK(seen_count >= 15);

		avatar_engine_sidecar_ops.destroy(engine);
	}

	/* --- a worker that dies at once is retried with a growing delay --- */
	{
		FILE *script = fopen(crash_script.array, "wb");
		CHECK(script != NULL);
		fputs("import sys\nprint('crashing on purpose', flush=True)\nsys.exit(3)\n", script);
		fclose(script);

		struct avatar_engine_config cfg = make_config(crash_script.array, python, crash_log.array);
		void *engine = avatar_engine_sidecar_ops.create(&cfg, NULL);
		CHECK(engine != NULL);

		os_sleep_ms(CRASH_LOOP_WATCH_MS);

		const int launches = count_in_file(crash_log.array, LAUNCH_MARKER);
		printf("a worker that exits at once was launched %d times in %d ms\n", launches, CRASH_LOOP_WATCH_MS);

		/* Launches at ~0, ~1s and ~3s. Fewer than two means it gave up;
		 * more than four means the delay is not growing. */
		CHECK(launches >= 2);
		CHECK(launches <= 4);
		CHECK(count_in_file(crash_log.array, "crashing on purpose") == launches ||
		      count_in_file(crash_log.array, "crashing on purpose") == launches - 1);
		CHECK(!avatar_engine_sidecar_ops.ready(engine));

		const uint64_t destroy_started = os_gettime_ns();
		avatar_engine_sidecar_ops.destroy(engine);
		CHECK(elapsed_ms(destroy_started) < 3000);
	}

	/* --- a worker that stops reading must not stall the caller ---
	 *
	 * The case that hung OBS: a worker slower than real time stops draining its
	 * pipe, the pipe fills, and a write on the source's tick thread waited for
	 * it - under the lock the renderer and the properties dialog also take. The
	 * stalled worker here answers the handshake and then never reads again. */
	{
		struct dstr stall_script = {0};
		dstr_printf(&stall_script, "%savatar-launch-test-stall.py", temp);
		struct dstr stall_log = {0};
		dstr_printf(&stall_log, "%savatar-launch-test-stall.log", temp);

		FILE *script = fopen(stall_script.array, "wb");
		CHECK(script != NULL);
		fputs("import struct, sys, time\n"
		      "pipe = sys.argv[sys.argv.index('--pipe') + 1]\n"
		      "f = open(pipe, 'r+b', buffering=0)\n"
		      "length, kind, _ = struct.unpack('<IHH', f.read(8))\n"
		      "f.read(length)\n"
		      "name = b'stalled'\n"
		      "body = struct.pack('<IIIII', 2, 64, 64, 0, len(name)) + name\n"
		      "f.write(struct.pack('<IHH', len(body), 2, 0) + body)\n"
		      "time.sleep(600)\n",
		      script);
		fclose(script);

		struct avatar_engine_config cfg = make_config(stall_script.array, python, stall_log.array);
		void *engine = avatar_engine_sidecar_ops.create(&cfg, NULL);
		CHECK(engine != NULL);
		CHECK(wait_ready(engine, true, READY_TIMEOUT_MS));

		/* A minute of audio, in the 4096-sample blocks the source uses: far
		 * more than the pipe and the ring can hold between them. */
		float block[4096] = {0};
		for (int i = 0; i < 4096; i++)
			block[i] = 0.1f;

		uint64_t slowest_ms = 0;
		const uint64_t push_started = os_gettime_ns();
		int64_t pts = 0;
		for (int i = 0; i < 235; i++) {
			const uint64_t one = os_gettime_ns();
			avatar_engine_sidecar_ops.push_audio(engine, block, 4096, pts);
			pts += 4096LL * 1000000000LL / TEST_SAMPLE_RATE;
			const uint64_t took = elapsed_ms(one);
			if (took > slowest_ms)
				slowest_ms = took;
			/* Paced like the tick would be, so the writer gets to run. */
			os_sleep_ms(2);
		}
		printf("pushed a minute of audio at a worker that never reads: slowest push %llu ms, total %llu ms\n",
		       (unsigned long long)slowest_ms, (unsigned long long)elapsed_ms(push_started));
		CHECK(slowest_ms < 100);

		/* A worker that takes none of it is as far behind as a worker can be, so
		 * the engine must ask for fewer frames a second rather than keep throwing
		 * the audio away. It judges over ten seconds of wall time. */
		struct avatar_engine_stats stats;
		avatar_engine_sidecar_ops.get_stats(engine, &stats);
		CHECK(stats.fps == TEST_FPS);

		const uint64_t watch_started = os_gettime_ns();
		while (elapsed_ms(watch_started) < 11500) {
			avatar_engine_sidecar_ops.push_audio(engine, block, 4096, pts);
			pts += 4096LL * 1000000000LL / TEST_SAMPLE_RATE;
			os_sleep_ms(30);
		}

		avatar_engine_sidecar_ops.get_stats(engine, &stats);
		printf("frame rate after ten seconds of a worker that never reads: %u (asked for %d)\n", stats.fps,
		       TEST_FPS);
		CHECK(stats.fps < TEST_FPS);
		CHECK(stats.fps >= 15);

		const uint64_t destroy_started = os_gettime_ns();
		avatar_engine_sidecar_ops.destroy(engine);
		const uint64_t destroy_ms = elapsed_ms(destroy_started);
		printf("destroy with the worker stalled took %llu ms\n", (unsigned long long)destroy_ms);
		CHECK(destroy_ms < 3000);

		DeleteFileA(stall_script.array);
		DeleteFileA(stall_log.array);
		dstr_free(&stall_script);
		dstr_free(&stall_log);
	}

	/* --- a script that does not exist is reported, not launched --- */
	{
		struct dstr missing_log = {0};
		dstr_printf(&missing_log, "%savatar-launch-test-missing.log", temp);
		DeleteFileA(missing_log.array);

		struct avatar_engine_config cfg = make_config("Z:\\no\\such\\worker.py", python, missing_log.array);
		void *engine = avatar_engine_sidecar_ops.create(&cfg, NULL);
		CHECK(engine != NULL);

		os_sleep_ms(500);
		CHECK(!avatar_engine_sidecar_ops.ready(engine));
		CHECK(!os_file_exists(missing_log.array));

		avatar_engine_sidecar_ops.destroy(engine);
		dstr_free(&missing_log);
	}

	DeleteFileA(log_path.array);
	DeleteFileA(crash_log.array);
	DeleteFileA(crash_script.array);
	dstr_free(&log_path);
	dstr_free(&crash_log);
	dstr_free(&crash_script);

	printf("PASS\n");
	return 0;
}
