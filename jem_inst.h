#ifndef JEM_INST_H
#define JEM_INST_H

#include <stdint.h>
#include <stddef.h>

/*
 * Instantaneous reading (command 52 01 -> 15 x int32 LE).
 *
 * The 15 raw slots are converted to engineering values through a field-map
 * table in jem_inst.c: value = raw[slot] * scale. Three fields are CONFIRMED
 * (verified against the JemWare export inst.xlsx across 15 captures); the rest
 * are PROVISIONAL and reproduce a single capture only - their slot/scale need
 * calibration with a capture that varies each phase independently.
 */

#define JEM_INST_NSLOTS  15

/* An extra register value (command 52 12) to append to the output file. */
typedef struct {
    const char *name;    /* output label, e.g. "kWh_Del" */
    double      value;
    int         present; /* 1 if read succeeded */
} jem_reg_out_t;

/*
 * Write the instantaneous reading to
 *   <output_dir>/<meter_id>_inst_<YYYYMMDD_HHMMSS>.txt
 * (output_dir may be NULL for the current directory).
 * Each present entry in regs[] is appended as "<name>=<value>".
 * Returns 0 on success, -1 on error.
 */
int jem_inst_write(const char *output_dir, const char *meter_id,
                   const char *meter_time,
                   const int32_t *raw, int nslots,
                   const jem_reg_out_t *regs, int nregs,
                   int zeroed);   /* if non-zero, all instantaneous fields = 0 */

/* Print the raw 15 slots to stdout (useful for scale calibration). */
void jem_inst_dump_raw(const int32_t *raw, int nslots);

#endif /* JEM_INST_H */
