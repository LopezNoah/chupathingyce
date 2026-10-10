#ifndef HALO_FRAME_STATS_H
#define HALO_FRAME_STATS_H

/* Per-frame wall time and CPU time (port/linux/src/posix_frame_stats.c).
   Off unless HALO_FRAME_STATS names a CSV file, written at exit. */
void halo_frame_stats_present(void);

#endif
