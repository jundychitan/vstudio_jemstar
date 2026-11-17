#include "jem_loadprofile.h"

#include <stdio.h>
#include <string.h>

/* Plausible Unix-timestamp window for embedded markers (~2025-10 .. ~2026-09).
 * Widen if the meter clock is far outside this range. */
#define JEM_TS_MIN     0x69000000u
#define JEM_TS_MAX     0x6B000000u

#define JEM_FLAG_EVENT   0x8000u   /* bit15: event/boundary record            */
#define JEM_FLAG_PARTIAL 0x4000u   /* bit14: partial interval (JemRead '*')   */
#define JEM_VALUE_MASK   0x3FFFu
#define JEM_MARKER_LEN   10        /* [tag u16][ts1 u32][ts2 u32] */

/* Event marker tags (identify the event type of the record they follow). */
#define JEM_TAG_MIDNIGHT  0x8000u  /* day boundary                            */
#define JEM_TAG_FREEZE    0x4000u  /* register freeze                         */
#define JEM_TAG_BILLRESET 0x2000u  /* billing reset                           */
#define JEM_TAG_POWERFAIL 0x0800u  /* power failure (ts1=failure, ts2=restore) */

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int plausible_ts(uint32_t v) { return v >= JEM_TS_MIN && v <= JEM_TS_MAX; }

/* A 10-byte marker at 'off': tag then two plausible timestamps 4 bytes apart. */
static int is_marker(const uint8_t *d, size_t off, size_t len)
{
    if (off + JEM_MARKER_LEN > len) return 0;
    return plausible_ts(rd32(d + off + 2)) && plausible_ts(rd32(d + off + 6));
}

static const char *event_name(uint16_t tag)
{
    switch (tag) {
        case JEM_TAG_MIDNIGHT:  return "Midnight";
        case JEM_TAG_FREEZE:    return "Register Freeze";
        case JEM_TAG_BILLRESET: return "Billing Reset";
        case JEM_TAG_POWERFAIL: return "Power Failure";
        default:                return "Event";
    }
}

int jem_lp_parse(const uint8_t *payload, size_t len,
                 int interval_override_sec,
                 jem_lp_header_t *hdr,
                 jem_lp_row_t *rows, int max_rows)
{
    if (!payload || len < 8 || !hdr || !rows || max_rows <= 0) return -1;
    memset(hdr, 0, sizeof(*hdr));

    hdr->base_time     = (time_t)rd32(payload + 0);
    hdr->channel_count = rd16(payload + 4);
    hdr->interval_min  = rd16(payload + 6);

    if (hdr->channel_count < 1 || hdr->channel_count > JEM_LP_MAX_CHANNELS) {
        fprintf(stderr, "jem_lp: implausible channel count %d\n", hdr->channel_count);
        return -1;
    }

    /* Channel configuration table. */
    size_t off = 8;
    for (int i = 0; i < hdr->channel_count; i++) {
        if (off + 8 > len) break;
        hdr->channels[i].config_ts = rd32(payload + off);
        memcpy(hdr->channels[i].desc, payload + off + 4, 4);
        off += 8;
        hdr->table_count++;
    }

    if (interval_override_sec > 0)
        hdr->interval_sec = interval_override_sec;
    else if (hdr->interval_min >= 1 && hdr->interval_min <= 1440)
        hdr->interval_sec = hdr->interval_min * 60;
    else
        hdr->interval_sec = 900;

    const size_t rec        = (size_t)hdr->channel_count * 2;
    const size_t data_start = off;
    const time_t interval   = hdr->interval_sec;

    /* Anchor: locate the first event marker; the event record precedes it by
     * one record. This skips any fixed preamble after the channel table. */
    long ms = -1;
    for (size_t o = data_start; o + JEM_MARKER_LEN <= len; o++) {
        if (is_marker(payload, o, len)) { ms = (long)o; break; }
    }

    size_t p = data_start;
    if (ms >= 0 && (size_t)ms >= data_start + rec)
        p = (size_t)ms - rec;

    time_t grid     = 0;
    int    have_grid = 0;
    int    nrows    = 0;

    while (p + rec <= len && nrows < max_rows) {
        /* Determine if this is an event record (any channel has bit15 set). */
        int flagged = 0;
        for (int k = 0; k < hdr->channel_count; k++)
            if (rd16(payload + p + (size_t)k * 2) & JEM_FLAG_EVENT) { flagged = 1; break; }

        jem_lp_row_t *row = &rows[nrows];
        memset(row, 0, sizeof(*row));
        row->nvalues = hdr->channel_count;
        for (int k = 0; k < hdr->channel_count; k++) {
            uint16_t raw = rd16(payload + p + (size_t)k * 2);
            row->values[k]  = (double)(raw & JEM_VALUE_MASK);
            row->partial[k] = (raw & JEM_FLAG_PARTIAL) ? 1 : 0;
        }

        if (flagged && is_marker(payload, p + rec, len)) {
            uint16_t    tag  = rd16(payload + p + rec);
            time_t      ts1  = (time_t)rd32(payload + p + rec + 2);
            time_t      ts2  = (time_t)rd32(payload + p + rec + 6);
            const char *name = event_name(tag);

            row->time      = ts1;
            row->have_time = 1;
            snprintf(row->event, sizeof(row->event), "%s", name);
            nrows++;

            /* A power failure carries two timestamps: ts1 = failure time,
             * ts2 = restore time. When they differ, emit a separate
             * "<event> End" row (no values) at ts2, matching JemRead. */
            if (ts2 != ts1 && nrows < max_rows) {
                jem_lp_row_t *er = &rows[nrows];
                memset(er, 0, sizeof(*er));
                er->time      = ts2;
                er->have_time = 1;
                er->nvalues   = 0;               /* blank value cells */
                snprintf(er->event, sizeof(er->event), "%s End", name);
                nrows++;
            }

            /* Resume the interval grid from the later timestamp (ts2), so data
             * after a power failure restarts at the correct interval. */
            time_t diff = ts2 - grid;
            if (diff < 0) diff = -diff;
            if (!have_grid || diff >= interval) {
                grid = (time_t)((ts2 / interval) * interval);
                have_grid = 1;
            }
            p += rec + JEM_MARKER_LEN;
        } else {
            if (!have_grid) {
                grid = (time_t)((hdr->base_time / interval) * interval);
                have_grid = 1;
            } else {
                grid += interval;
            }
            row->time      = grid;
            row->have_time = 1;
            snprintf(row->event, sizeof(row->event), "Data");
            nrows++;
            p += rec;
        }
    }

    return nrows;
}

void jem_lp_channel_name(const jem_lp_header_t *hdr, int i, char *out, size_t out_max)
{
    if (i >= 0 && i < hdr->table_count) {
        const uint8_t *d = hdr->channels[i].desc;
        snprintf(out, out_max, "Ch%d (%02x %02x %02x %02x)",
                 i + 1, d[0], d[1], d[2], d[3]);
    } else {
        snprintf(out, out_max, "Ch%d", i + 1);
    }
}

/* Convert a UTC calendar date/time to a Unix epoch (matches the meter
 * wall-clock basis used for row timestamps). Avoids non-portable timegm. */
static time_t utc_epoch(int year, int mon, int mday, int hour, int min, int sec)
{
    static const int cum[12] = {0,31,59,90,120,151,181,212,243,273,304,334};
    long days = (long)(year - 1970) * 365
              + (year - 1969) / 4 - (year - 1901) / 100 + (year - 1601) / 400;
    days += cum[(mon - 1) % 12];
    if (mon > 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) days++;
    days += mday - 1;
    return (time_t)days * 86400 + (time_t)hour * 3600 + min * 60 + sec;
}

int jem_lp_parse_range(const char *str, jem_lp_range_t *range)
{
    int y1, mo1, d1, h1, mi1, y2, mo2, d2, h2, mi2;
    if (!str || !range) return -1;
    if (sscanf(str, "%d-%d-%d_%d:%d,%d-%d-%d_%d:%d",
               &y1, &mo1, &d1, &h1, &mi1, &y2, &mo2, &d2, &h2, &mi2) != 10) {
        fprintf(stderr, "Invalid range. Use YYYY-MM-DD_HH:MM,YYYY-MM-DD_HH:MM\n");
        return -1;
    }
    range->start = utc_epoch(y1, mo1, d1, h1, mi1, 0);
    range->end   = utc_epoch(y2, mo2, d2, h2, mi2, 59);  /* include the end minute */
    return 0;
}

int jem_lp_filter_range(jem_lp_row_t *rows, int nrows, const jem_lp_range_t *range)
{
    if (!range || (range->start == 0 && range->end == 0)) return nrows;
    int k = 0;
    for (int i = 0; i < nrows; i++) {
        if (rows[i].time >= range->start && rows[i].time <= range->end) {
            if (k != i) rows[k] = rows[i];
            k++;
        }
    }
    return k;
}
