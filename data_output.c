#include "data_output.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* Format a meter timestamp (stored Unix value treated as UTC wall-clock). */
static void fmt_meter_time(time_t t, char *out, size_t out_max)
{
    if (out_max == 0) return;
    struct tm *tmv = gmtime(&t);
    if (!tmv || strftime(out, out_max, "%Y-%m-%d %H:%M:%S", tmv) == 0)
        snprintf(out, out_max, " ");
}

int jem_write_loadprofile_csv(const char *output_dir,
                              int profile_num,
                              const jem_lp_header_t *hdr,
                              const jem_lp_row_t *rows, int nrows,
                              const char *meter_serial,
                              const char *meter_time)
{
    const char *serial = (meter_serial && meter_serial[0]) ? meter_serial : "unknown";

    /* Derive a start date for the filename from the first timestamped row. */
    char date_str[16] = "unknown";
    time_t date_src = hdr->base_time;
    for (int i = 0; i < nrows; i++) {
        if (rows[i].have_time) { date_src = rows[i].time; break; }
    }
    {
        struct tm *tmv = gmtime(&date_src);
        if (tmv && strftime(date_str, sizeof(date_str), "%Y-%m-%d", tmv) == 0)
            snprintf(date_str, sizeof(date_str), "unknown");
    }

    char filename[512];
    if (output_dir && output_dir[0])
        snprintf(filename, sizeof(filename), "%s/%s_LP%d_%s.csv", output_dir, serial, profile_num, date_str);
    else
        snprintf(filename, sizeof(filename), "%s_LP%d_%s.csv", serial, profile_num, date_str);

    FILE *f = fopen(filename, "w");
    if (!f) { perror(filename); return -1; }

    /* Metadata header. */
    fprintf(f, "Meter Serial No. : %s\n", serial);
    fprintf(f, "Company Number :\n");
    {
        time_t now = time(NULL);
        struct tm *tmv = localtime(&now);
        if (tmv)
            fprintf(f, "Modem Time : %04d-%02d-%02d %02d:%02d:%02d\n",
                    tmv->tm_year + 1900, tmv->tm_mon + 1, tmv->tm_mday,
                    tmv->tm_hour, tmv->tm_min, tmv->tm_sec);
        else
            fprintf(f, "Modem Time :\n");
    }
    if (meter_time && meter_time[0])
        fprintf(f, "Meter Time : %s\n", meter_time);
    else {
        char mt[32];
        fmt_meter_time(hdr->base_time, mt, sizeof(mt));
        fprintf(f, "Meter Time : %s\n", mt);
    }
    fprintf(f, "\n");

    /* Column header. */
    fprintf(f, "Date,Event");
    for (int c = 0; c < hdr->channel_count; c++) {
        char name[64];
        jem_lp_channel_name(hdr, c, name, sizeof(name));
        fprintf(f, ",%s", name);
    }
    fprintf(f, "\n");

    /* Data rows (oldest to newest, as parsed). */
    for (int r = 0; r < nrows; r++) {
        const jem_lp_row_t *row = &rows[r];
        char ts[32];
        fmt_meter_time(row->time, ts, sizeof(ts));
        fprintf(f, "%s,%s", ts, row->event[0] ? row->event : "Data");
        for (int c = 0; c < hdr->channel_count; c++) {
            if (c < row->nvalues)
                fprintf(f, ",%.10g%s", row->values[c], row->partial[c] ? "*" : "");
            else
                fprintf(f, ",");
        }
        fprintf(f, "\n");
    }

    fclose(f);
    printf("Load profile written to %s (%d rows, %d channels)\n",
           filename, nrows, hdr->channel_count);
    return 0;
}
