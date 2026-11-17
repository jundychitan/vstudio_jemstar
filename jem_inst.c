#include "jem_inst.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/*
 * Field map: engineering value = raw[slot] * scale.
 *
 *   slot >= 0 : use that raw int32 slot
 *   slot <  0 : field not yet resolved -> written blank (pending calibration)
 *   confirmed : 1 = verified against inst.xlsx; 0 = provisional best-guess
 *
 * Calibration is a one-line edit per field here. Provisional entries were
 * fitted to a single capture and are NOT expected to hold when the phases
 * are driven independently.
 */
typedef struct {
    const char *name;
    int         slot;
    double      scale;
    int         confirmed;
} inst_field_t;

static const inst_field_t FIELDS[] = {
    /* name              slot   scale          confirmed */
    { "Va",                 1,  0.00362319, 1 },  /* provisional */
    { "Phase_angle_Va",    -1,  0.0       , 0 },  /* provisional */
    { "Ia",                 3,  0.00209930, 1 },  /* CONFIRMED   */
    { "Phase_angle_Ia",    -1, -0.0       , 0 },  /* provisional */
    { "Vb",                 5,  0.00362319, 1 },  /* provisional */
    { "Phase_angle_Vb",    -1,  0.0       , 0 },  /* provisional */
    { "Ib",                 7,  0.00209930, 1 },  /* pending     */
    { "Phase_angle_Ib",    -1,  0.0,        0 },  /* pending     */
    { "Vc",                 9,  0.00362319, 1 },  /* provisional */
    { "Phase_angle_Vc",    -1, -0.0,        0 },  /* provisional */
    { "Ic",                11,  0.00209930, 1 },  /* pending     */
    { "Phase_angle_Ic",    -1,  0.0,        0 },  /* pending     */
    { "Total_Watts",       12,  0.00705490, 1 },  /* CONFIRMED   */
    { "Total_VA",          13,  0.00705490, 1 },  /* CONFIRMED   */
    { "I_neutral",         -1,  0.0       , 0 },  /* provisional */
};
#define NFIELDS ((int)(sizeof(FIELDS) / sizeof(FIELDS[0])))

void jem_inst_dump_raw(const int32_t *raw, int nslots)
{
    printf("Instantaneous raw slots (52 01):");
    for (int i = 0; i < nslots; i++) {
        if (i % 5 == 0) printf("\n  ");
        printf("[%2d]=%-8d ", i, raw[i]);
    }
    printf("\n");
}

int jem_inst_write(const char *output_dir, const char *meter_id,
                   const char *meter_time,
                   const int32_t *raw, int nslots,
                   const jem_reg_out_t *regs, int nregs,
                   int zeroed)
{
    const char *id = (meter_id && meter_id[0]) ? meter_id : "unknown";

    char ts[32] = "time";
    {
        time_t now = time(NULL);
        struct tm *t = localtime(&now);
        if (t)
            snprintf(ts, sizeof(ts), "%04d%02d%02d_%02d%02d%02d",
                     t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
                     t->tm_hour, t->tm_min, t->tm_sec);
    }

    char filename[512];
    if (output_dir && output_dir[0])
        snprintf(filename, sizeof(filename), "%s/%s_inst_%s.txt", output_dir, id, ts);
    else
        snprintf(filename, sizeof(filename), "%s_inst_%s.txt", id, ts);

    FILE *f = fopen(filename, "w");
    if (!f) { perror(filename); return -1; }

    fprintf(f, "Meter_ID=%s\n", id);
    if (meter_time && meter_time[0])
        fprintf(f, "Meter_Time=%s\n", meter_time);
    for (int i = 0; i < NFIELDS; i++) {
        const inst_field_t *fl = &FIELDS[i];
        const char *fmt = strstr(fl->name, "angle") ? "%s=%.2f\n" : "%s=%.3f\n";
        if (zeroed) {
            fprintf(f, fmt, fl->name, 0.0);              /* disabled: all zero */
        } else if (fl->slot >= 0 && fl->slot < nslots) {
            double v = (double)raw[fl->slot] * fl->scale;
            fprintf(f, fmt, fl->name, v);
        } else {
            fprintf(f, "%s=\n", fl->name);               /* pending calibration */
        }
    }

    /* Energy / demand registers (command 52 12) - confirmed. */
    for (int i = 0; i < nregs; i++)
        if (regs[i].present)
            fprintf(f, "%s=%.3f\n", regs[i].name, regs[i].value);

    fclose(f);
    printf("Instantaneous reading written to %s\n", filename);
    if (!zeroed)
        printf("Note: only Ia, Total_Watts and Total_VA are calibrated; other "
               "fields are provisional/pending (edit the field map in jem_inst.c).\n");
    return 0;
}
