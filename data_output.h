#ifndef DATA_OUTPUT_H
#define DATA_OUTPUT_H

#include "jem_loadprofile.h"
#include <stddef.h>

/*
 * Write the decoded load profile to a CSV file, following the layout used by
 * the prometer100_dlms load-survey report:
 *
 *   Meter Serial No. : <serial>
 *   Company Number :
 *   Modem Time : <Raspberry Pi local clock at write time>
 *   Meter Time : <meter_time>
 *   <blank line>
 *   Date,Ch1 (...),Ch2 (...),...
 *   YYYY-MM-DD HH:MM:SS,v1,v2,...
 *
 * The file is created as <output_dir>/<serial>_LP_<startdate>.csv
 * (output_dir may be NULL for the current directory).
 *
 * Interval timestamps are formatted in the meter's own wall-clock (the stored
 * Unix value is treated as UTC so the printed time matches the meter).
 *
 * Returns 0 on success, -1 on error.
 */
int jem_write_loadprofile_csv(const char *output_dir,
                              int profile_num,
                              const jem_lp_header_t *hdr,
                              const jem_lp_row_t *rows, int nrows,
                              const char *meter_serial,
                              const char *meter_time);

#endif /* DATA_OUTPUT_H */
