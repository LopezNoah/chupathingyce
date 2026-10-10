/* Per-frame measurements for benchmarks: off unless HALO_FRAME_STATS names a
   CSV path. At every D3DDevice_Present it records the wall time since the
   previous frame (the frame rate), the main thread's CPU time over that
   interval (CLOCK_THREAD_CPUTIME_ID: the game's and the OpenGL driver's work
   on the render thread, not the frame limiter's sleep), and the whole
   process's CPU time (job workers, driver and audio threads too). The CSV
   is written at exit; rows hold a timestamp so a window can be chosen. */

#include "halo_frame_stats.h"

void platform_log(const char *format, ...) __attribute__((format(printf, 1, 2)));

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <time.h>

/* 10 minutes at 1000 frames a second; 16 bytes a frame, touched only as used */
#define FRAME_STATS_MAX 600000u

struct frame_stat
{
	uint32_t time_ms;       /* since the first frame */
	uint32_t wall_us;
	uint32_t thread_cpu_us;
	uint32_t process_cpu_us;
};

static struct frame_stat *frame_stats;
static uint32_t frame_stat_count;
static char const *frame_stats_path;
static int frame_stats_state; /* 0 unknown, 1 off, 2 on */
static uint64_t frame_first_ns, frame_last_ns, frame_last_thread_ns, frame_last_process_us;

static uint64_t frame_clock_ns(clockid_t clock_id)
{
	struct timespec now;
	clock_gettime(clock_id, &now);
	return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

static uint64_t frame_process_cpu_us(void)
{
	struct rusage usage;
	getrusage(RUSAGE_SELF, &usage);
	return (uint64_t)(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1000000ull +
		(uint64_t)(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec);
}

static void frame_stats_write(void)
{
	FILE *file;
	uint32_t index;

	if (frame_stats_state != 2 || !(file = fopen(frame_stats_path, "w")))
		return;
	fprintf(file, "time_ms,wall_us,thread_cpu_us,process_cpu_us\n");
	for (index = 0; index < frame_stat_count; index++)
		fprintf(file, "%u,%u,%u,%u\n", frame_stats[index].time_ms, frame_stats[index].wall_us,
			frame_stats[index].thread_cpu_us, frame_stats[index].process_cpu_us);
	fclose(file);
	platform_log("frame stats: %u frames written to %s", frame_stat_count, frame_stats_path);
}

void halo_frame_stats_present(void)
{
	uint64_t now, thread, process;

	if (frame_stats_state == 0)
	{
		frame_stats_path = getenv("HALO_FRAME_STATS");
		frame_stats_state = 1;
		if (frame_stats_path && frame_stats_path[0] &&
			(frame_stats = calloc(FRAME_STATS_MAX, sizeof(*frame_stats))) != NULL)
		{
			frame_stats_state = 2;
			atexit(frame_stats_write);
		}
		if (frame_stats_state != 2)
			return;
		frame_first_ns = frame_last_ns = frame_clock_ns(CLOCK_MONOTONIC);
		frame_last_thread_ns = frame_clock_ns(CLOCK_THREAD_CPUTIME_ID);
		frame_last_process_us = frame_process_cpu_us();
		return;
	}
	if (frame_stats_state != 2 || frame_stat_count >= FRAME_STATS_MAX)
		return;
	now = frame_clock_ns(CLOCK_MONOTONIC);
	thread = frame_clock_ns(CLOCK_THREAD_CPUTIME_ID);
	process = frame_process_cpu_us();
	frame_stats[frame_stat_count].time_ms = (uint32_t)((now - frame_first_ns) / 1000000u);
	frame_stats[frame_stat_count].wall_us = (uint32_t)((now - frame_last_ns) / 1000u);
	frame_stats[frame_stat_count].thread_cpu_us = (uint32_t)((thread - frame_last_thread_ns) / 1000u);
	frame_stats[frame_stat_count].process_cpu_us = (uint32_t)(process - frame_last_process_us);
	frame_stat_count++;
	frame_last_ns = now;
	frame_last_thread_ns = thread;
	frame_last_process_us = process;
}
