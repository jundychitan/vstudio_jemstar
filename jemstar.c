/*
 * jemstar - Ametek JemStar load-profile reader for Raspberry Pi.
 *
 * Connects to a JemStar meter over a serial port, logs on, reads a load
 * profile using the DLE-framed binary protocol (see jem_link.h), and writes
 * the result to a CSV file in the prometer100_dlms load-survey layout.
 *
 * Load-profile selection follows the prometer100_dlms convention:
 *   -1 <start,end>  read Load Profile 1 within a date/time range
 *   -2 <start,end>  read Load Profile 2 within a date/time range
 * with the range formatted as YYYY-MM-DD_HH:MM,YYYY-MM-DD_HH:MM. Only rows
 * inside the range are written.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <getopt.h>

#include "transport.h"
#include "jem_link.h"
#include "jem_client.h"
#include "jem_loadprofile.h"
#include "jem_inst.h"
#include "data_output.h"

#define DEFAULT_BAUD       9600
#define DEFAULT_PASSWORD   "000000"
#define LP_BUF_SIZE        (1024 * 1024)   /* 1 MB reassembly buffer */

static void usage(const char *prog)
{
    printf("Usage: %s -s <serial_port> [options]\n"
           "  -s <serial_port>   Serial device (e.g. /dev/ttyUSB0, /dev/ttyS0)\n"
           "  -b <baud>          Baud rate (default: %d)\n"
           "  -d <7|8>           Data bits (default: 8)\n"
           "  -y <N|E|O>         Parity (default: N)\n"
           "  -u <username>      Logon username (default: none)\n"
           "  -w <password>      Logon password (default: %s)\n"
           "  -o <output_dir>    Output directory (default: current dir)\n"
           "  -a <addr>          Master address byte (default: 0xFE)\n"
           "  -1 <start,end>     Also read Load Profile 1 within a date/time range\n"
           "  -2 <start,end>     Also read Load Profile 2 within a date/time range\n"
           "                     Range: YYYY-MM-DD_HH:MM,YYYY-MM-DD_HH:MM\n"
           "                     (instantaneous values are always read first)\n"
           "  -P <profile>       Profile selector byte if -1/-2 not used (default: 1)\n"
           "  -i <seconds>       Override interval period (default: from meter header)\n"
           "  -D <file>          Also dump the raw reassembled payload to <file>\n"
           "  -f <file>          Offline: parse a raw payload file instead of a meter\n"
           "  -n                 Disable instantaneous read (52 01); fields output as 0\n"
           "  -v                 Verbose (hex-dump frames)\n"
           "  -T                 Run built-in self-tests and exit\n"
           "  -h                 Show this help\n",
           prog, DEFAULT_BAUD, DEFAULT_PASSWORD);
}

static parity_t parse_parity(const char *s)
{
    if (!s || !s[0]) return PARITY_NONE;
    if (s[0] == 'E' || s[0] == 'e') return PARITY_EVEN;
    if (s[0] == 'O' || s[0] == 'o') return PARITY_ODD;
    return PARITY_NONE;
}

static char parity_char(parity_t p)
{
    return p == PARITY_EVEN ? 'E' : (p == PARITY_ODD ? 'O' : 'N');
}

static void dump_raw(const char *file, const uint8_t *lp, int len)
{
    if (!file || len <= 0) return;
    FILE *fp = fopen(file, "wb");
    if (fp) {
        fwrite(lp, 1, (size_t)len, fp);
        fclose(fp);
        printf("Raw payload written to %s\n", file);
    } else {
        perror(file);
    }
}

/* Parse a payload, optionally filter to a date range, and write the CSV. */
static int emit_profile(const uint8_t *lp, int lp_len, int profile_num,
                        int interval_override, const jem_lp_range_t *range,
                        const char *output_dir, const char *meter_serial)
{
    if (lp_len <= 0) { fprintf(stderr, "No load-profile data to parse\n"); return -1; }

    jem_lp_header_t hdr;
    jem_lp_row_t *rows = (jem_lp_row_t *)malloc(sizeof(jem_lp_row_t) * JEM_LP_MAX_ROWS);
    if (!rows) { fprintf(stderr, "Out of memory for rows\n"); return -1; }

    int nrows = jem_lp_parse(lp, (size_t)lp_len, interval_override, &hdr, rows, JEM_LP_MAX_ROWS);
    if (nrows < 0) {
        fprintf(stderr, "Failed to parse load profile\n");
        free(rows);
        return -1;
    }

    if (range && (range->start || range->end)) {
        int before = nrows;
        nrows = jem_lp_filter_range(rows, nrows, range);
        printf("Filtered to %d of %d rows within the requested range\n", nrows, before);
    }

    char meter_time[32] = "";
    {
        time_t bt = hdr.base_time;
        struct tm *t = gmtime(&bt);
        if (t && strftime(meter_time, sizeof(meter_time), "%Y-%m-%d %H:%M:%S", t) == 0)
            meter_time[0] = '\0';
    }
    printf("LP%d: base %s | channels %d | interval %d s | rows %d\n",
           profile_num, meter_time, hdr.channel_count, hdr.interval_sec, nrows);

    jem_write_loadprofile_csv(output_dir, profile_num, &hdr, rows, nrows,
                              (meter_serial && meter_serial[0]) ? meter_serial : NULL,
                              meter_time);
    free(rows);
    return 0;
}

int main(int argc, char *argv[])
{
    const char *serial_port = NULL;
    const char *output_dir  = NULL;
    const char *password    = DEFAULT_PASSWORD;
    const char *username    = NULL;
    const char *infile      = NULL;
    const char *dumpfile    = NULL;
    const char *ls1_str     = NULL;
    const char *ls2_str     = NULL;
    int      baud     = DEFAULT_BAUD;
    int      databits = 8;
    parity_t parity   = PARITY_NONE;
    int      master_addr = JEM_ADDR_MASTER;
    int      profile     = 1;
    int      interval_override = 0;
    int      verbose  = 0;
    int      no_inst  = 0;

    int opt;
    while ((opt = getopt(argc, argv, "s:b:d:y:u:w:o:a:1:2:P:i:D:f:nvTh")) != -1) {
        switch (opt) {
            case 's': serial_port = optarg; break;
            case 'b': baud = atoi(optarg); break;
            case 'd': databits = atoi(optarg); break;
            case 'y': parity = parse_parity(optarg); break;
            case 'u': username = optarg; break;
            case 'w': password = optarg; break;
            case 'o': output_dir = optarg; break;
            case 'a': master_addr = (int)strtol(optarg, NULL, 0); break;
            case '1': ls1_str = optarg; break;
            case '2': ls2_str = optarg; break;
            case 'P': profile = (int)strtol(optarg, NULL, 0); break;
            case 'i': interval_override = atoi(optarg); break;
            case 'D': dumpfile = optarg; break;
            case 'f': infile = optarg; break;
            case 'n': no_inst = 1; break;
            case 'v': verbose = 1; break;
            case 'T': return jem_link_selftest();
            case 'h': usage(argv[0]); return 0;
            default:  usage(argv[0]); return 1;
        }
    }

    /* Parse the requested load-profile date ranges. */
    jem_lp_range_t r1 = {0, 0}, r2 = {0, 0};
    int do1 = 0, do2 = 0;
    if (ls1_str) { if (jem_lp_parse_range(ls1_str, &r1) < 0) return 1; do1 = 1; }
    if (ls2_str) { if (jem_lp_parse_range(ls2_str, &r2) < 0) return 1; do2 = 1; }

    uint8_t *lp = (uint8_t *)malloc(LP_BUF_SIZE);
    if (!lp) { fprintf(stderr, "Out of memory\n"); return 1; }
    char meter_serial[32] = "";

    /* -------- Offline mode: parse a saved raw payload -------- */
    if (infile) {
        FILE *fp = fopen(infile, "rb");
        if (!fp) { perror(infile); free(lp); return 1; }
        int lp_len = (int)fread(lp, 1, LP_BUF_SIZE, fp);
        fclose(fp);
        printf("Loaded %d bytes from %s\n", lp_len, infile);
        if (dumpfile) dump_raw(dumpfile, lp, lp_len);

        const jem_lp_range_t *rng = do1 ? &r1 : (do2 ? &r2 : NULL);
        int pnum = do1 ? 1 : (do2 ? 2 : profile);
        int rc = emit_profile(lp, lp_len, pnum, interval_override, rng, output_dir, meter_serial);
        free(lp);
        return rc < 0 ? 1 : 0;
    }

    /* -------- Online mode: talk to the meter -------- */
    if (!serial_port) {
        fprintf(stderr, "Error: specify -s <serial_port> (or -f <file>, or -T)\n");
        usage(argv[0]);
        free(lp);
        return 1;
    }

    transport_t tp;
    memset(&tp, 0, sizeof(tp));
    tp.verbose = verbose;

    serial_params_t sp = { baud, databits, parity, 1 };
    if (transport_open_serial(&tp, serial_port, &sp) < 0) {
        fprintf(stderr, "Failed to open serial port %s\n", serial_port);
        free(lp);
        return 1;
    }
    printf("Opened %s at %d baud %d%c1\n",
           serial_port, baud, databits, parity_char(parity));

    jem_client_t cli;
    jem_client_init(&cli, &tp, (uint8_t)master_addr, JEM_ADDR_METER);

    if (jem_identify(&cli) == 0)
        printf("Meter identity: firmware='%s' model='%s'\n", cli.firmware, cli.model);
    else
        printf("Warning: identity (06 01) read failed\n");

    jem_read_meter_config(&cli);   /* best-effort; not required */

    if (jem_logon(&cli, username, password) < 0) {
        fprintf(stderr, "Logon failed\n");
        transport_close(&tp);
        free(lp);
        return 1;
    }
    printf("Logon OK\n");

    /* Meter ID (56 1b) - used as the serial in all outputs. */
    char meter_id[32] = "";
    if (jem_read_meter_id(&cli, meter_id, sizeof(meter_id)) == 0)
        printf("Meter ID: %s\n", meter_id);
    else
        printf("Warning: meter ID (56 1b) read failed\n");

    int rc = 0;

    /* Instantaneous reading (unless disabled via -n), plus energy/demand registers. */
    {
        int32_t inst[JEM_INST_NSLOTS];
        int ni;
        if (no_inst) {
            memset(inst, 0, sizeof(inst));   /* -n: report all instantaneous data as 0 */
            ni = JEM_INST_NSLOTS;
            printf("Instantaneous read disabled (-n); instantaneous fields set to 0\n");
        } else {
            ni = jem_read_instantaneous(&cli, inst, JEM_INST_NSLOTS);
        }

        /* Read the Normal Registers block once and extract the ones we want. */
        static const struct { const char *label; const char *out; } REGMAP[] = {
            { "Working kWh Del",    "kWh_Del"    },
            { "Working kVARh Del",  "kVARh_Del"  },
            { "Working Max kW Del", "Max_kW_Del" },
            { "Working kWh Rec",    "kWh_Rec"    },
            { "Working kVARh Rec",  "kVARh_Rec"  },
        };
        const int NREG = (int)(sizeof(REGMAP) / sizeof(REGMAP[0]));
        jem_reg_out_t regs[5];
        uint8_t regbuf[4096];
        int rn = jem_read_registers(&cli, regbuf, sizeof(regbuf));
        if (rn <= 0) printf("Warning: register block (52 12) read failed\n");
        for (int i = 0; i < NREG; i++) {
            double v = 0.0;
            int ok = (rn > 0 && jem_register_extract(regbuf, rn, REGMAP[i].label, &v) == 0);
            regs[i].name    = REGMAP[i].out;
            regs[i].value   = v;
            regs[i].present = ok;
            if (ok) printf("%s = %.3f\n", REGMAP[i].out, v);
        }

        /* Meter date/time from the Present Date register. */
        char meter_time[32] = "";
        {
            uint32_t mt = 0;
            if (rn > 0 && jem_register_extract_time(regbuf, rn, "Present Date", &mt) == 0) {
                time_t tt = (time_t)mt;
                struct tm *t = gmtime(&tt);
                if (t && strftime(meter_time, sizeof(meter_time), "%Y-%m-%d %H:%M:%S", t) == 0)
                    meter_time[0] = '\0';
                printf("Meter time: %s\n", meter_time);
            }
        }

        if (ni > 0) {
            if (verbose && !no_inst) jem_inst_dump_raw(inst, ni);
            jem_inst_write(output_dir, meter_id, meter_time, inst, ni, regs, NREG, no_inst);
        } else {
            fprintf(stderr, "Instantaneous (52 01) read failed\n");
            rc = 1;
        }
    }

    /* Load profile only when explicitly requested via -1/-2. */
    if (do1) {
        int lp_len = jem_read_loadprofile(&cli, 1, lp, LP_BUF_SIZE);
        if (lp_len > 0) {
            printf("LP1: read %d bytes\n", lp_len);
            if (dumpfile) dump_raw(dumpfile, lp, lp_len);
            emit_profile(lp, lp_len, 1, interval_override, &r1, output_dir, meter_id);
        } else { fprintf(stderr, "LP1 read failed\n"); rc = 1; }
    }
    if (do2) {
        int lp_len = jem_read_loadprofile(&cli, 2, lp, LP_BUF_SIZE);
        if (lp_len > 0) {
            printf("LP2: read %d bytes\n", lp_len);
            emit_profile(lp, lp_len, 2, interval_override, &r2, output_dir, meter_id);
        } else { fprintf(stderr, "LP2 read failed\n"); rc = 1; }
    }

    jem_logoff(&cli);
    transport_close(&tp);
    free(lp);
    return rc;
}
