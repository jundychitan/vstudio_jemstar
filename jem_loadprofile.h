#ifndef JEM_LOADPROFILE_H
#define JEM_LOADPROFILE_H

#include <stdint.h>
#include <stddef.h>
#include <time.h>

/*
 * Parser for the reassembled Jemstar load-profile image (the concatenation of
 * all 44-01 block data fields). Calibrated against the JemRead export
 * jemstarProf20480701.csv (all decoded values matched exactly).
 *
 * Image layout:
 *   Offset 0  : base_time    (uint32 LE, Unix seconds, meter local wall-clock)
 *   Offset 4  : channel_cnt  (uint16 LE)   -> values per interval record
 *   Offset 6  : interval_min (uint16 LE)   -> interval length in minutes (=5)
 *   Offset 8  : channel table, channel_cnt entries x 8 bytes
 *                 { uint32 config_ts (LE), uint8 desc[4] }
 *   then       : interval data
 *
 * Interval data:
 *   - Fixed records of channel_cnt uint16 (LE). Each value carries two flag
 *     bits: bit15 (0x8000) marks an EVENT/boundary record (set on all channels
 *     of that record); bit14 (0x4000) marks a PARTIAL interval (JemRead prints
 *     a trailing '*'). The numeric value is (raw & 0x3FFF).
 *   - A Data record is a plain values record; time advances by interval.
 *   - An EVENT record (bit15 set) is immediately followed by a 10-byte marker
 *     [uint16 tag][uint32 ts1][uint32 ts2]; ts1 is the event's wall-clock time.
 *     tag 0x8000 = Midnight (day boundary); other tags are shown as "Event".
 *     The interval grid re-anchors to a marker time when it differs from the
 *     running grid by >= one interval (e.g. midnight, or a clock reset after a
 *     power failure).
 */

#define JEM_LP_MAX_CHANNELS   64
#define JEM_LP_MAX_ROWS       60000
#define JEM_LP_EVENT_LEN      24

typedef struct {
    uint32_t config_ts;
    uint8_t  desc[4];
} jem_lp_channel_t;

typedef struct {
    time_t           base_time;      /* header base time (most recent) */
    int              channel_count;  /* values per record              */
    int              interval_min;   /* header word @6 (minutes)       */
    int              interval_sec;   /* interval used for row timestamps */
    jem_lp_channel_t channels[JEM_LP_MAX_CHANNELS];
    int              table_count;
} jem_lp_header_t;

typedef struct {
    time_t  time;                          /* interval / event timestamp */
    int     have_time;
    char    event[JEM_LP_EVENT_LEN];       /* "Data", "Midnight", "Event" */
    double  values[JEM_LP_MAX_CHANNELS];   /* numeric value (raw & 0x3FFF) */
    uint8_t partial[JEM_LP_MAX_CHANNELS];  /* 1 if bit14 set (JemRead '*') */
    int     nvalues;
} jem_lp_row_t;

/* Date-time range filter (meter wall-clock seconds). */
typedef struct {
    time_t start;
    time_t end;
} jem_lp_range_t;

/* Parse "YYYY-MM-DD_HH:MM,YYYY-MM-DD_HH:MM" into a range. Returns 0 / -1. */
int jem_lp_parse_range(const char *str, jem_lp_range_t *range);

/* Keep only rows with start <= time <= end (in place). Returns retained count.
 * A zero range (start==end==0) keeps all rows. */
int jem_lp_filter_range(jem_lp_row_t *rows, int nrows, const jem_lp_range_t *range);

/*
 * Parse the payload. interval_override_sec > 0 forces the interval period;
 * otherwise it is derived from the header (interval_min * 60).
 * Fills *hdr and up to max_rows rows.
 * Returns the number of rows parsed, or -1 on error.
 */
int jem_lp_parse(const uint8_t *payload, size_t len,
                 int interval_override_sec,
                 jem_lp_header_t *hdr,
                 jem_lp_row_t *rows, int max_rows);

/* Build a human-readable name for channel index i, e.g. "Ch1 (01 00 01 01)". */
void jem_lp_channel_name(const jem_lp_header_t *hdr, int i,
                         char *out, size_t out_max);

#endif /* JEM_LOADPROFILE_H */
