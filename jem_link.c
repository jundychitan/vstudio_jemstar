#include "jem_link.h"

#include <stdio.h>
#include <string.h>

/* ---------- init ---------- */

void jem_link_init(jem_link_t *lnk, transport_t *tp,
                   uint8_t master_addr, uint8_t meter_addr)
{
    lnk->tp          = tp;
    lnk->master_addr = master_addr ? master_addr : JEM_ADDR_MASTER;
    lnk->meter_addr  = meter_addr  ? meter_addr  : JEM_ADDR_METER;
    lnk->timeout_ms  = JEM_DEFAULT_TIMEOUT_MS;
}

/* ---------- CRC-16/ARC (reflected, poly 0x8005 == reversed 0xA001) ---------- */

uint16_t jem_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0x0000;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            if (crc & 1)
                crc = (uint16_t)((crc >> 1) ^ 0xA001);
            else
                crc = (uint16_t)(crc >> 1);
        }
    }
    return crc;
}

/* ---------- verbose helper ---------- */

static void frame_dump(const jem_link_t *lnk, const char *pfx,
                       const uint8_t *d, size_t n)
{
    if (!lnk->tp || !lnk->tp->verbose) return;
    printf("%s %zu bytes:", pfx, n);
    for (size_t i = 0; i < n; i++) {
        if (i % 16 == 0) printf("\n  ");
        printf("%02x ", d[i]);
    }
    printf("\n");
}

/* ---------- frame build (pure) ---------- */

static size_t stuff(uint8_t *out, size_t pos, uint8_t b)
{
    if (b == JEM_DLE) out[pos++] = JEM_DLE;   /* escape 0x10 as 10 10 */
    out[pos++] = b;
    return pos;
}

int jem_build_frame(uint8_t addr, uint8_t func, uint8_t sel,
                    const uint8_t *data, size_t data_len,
                    uint8_t *out, size_t out_max)
{
    /* logical (CRC) content: ADDR func sel STX data... ETX */
    uint8_t logical[JEM_MAX_FRAME];
    if (data_len + 5 > sizeof(logical)) return -1;

    size_t ln = 0;
    logical[ln++] = addr;
    logical[ln++] = func;
    logical[ln++] = sel;
    logical[ln++] = JEM_STX;
    if (data_len) { memcpy(logical + ln, data, data_len); ln += data_len; }
    logical[ln++] = JEM_ETX;

    uint16_t crc = jem_crc16(logical, ln);

    /* wire: 10 01 | stuffed(addr func sel) | 10 02 | stuffed(data) | 10 03 | crc */
    size_t w = 0;
    /* worst case: every byte stuffed (2x) plus framing/CRC */
    if (2 + 2 * (3 + data_len) + 2 + 2 + 2 > out_max) return -1;

    out[w++] = JEM_DLE; out[w++] = JEM_SOH;
    w = stuff(out, w, addr);
    w = stuff(out, w, func);
    w = stuff(out, w, sel);
    out[w++] = JEM_DLE; out[w++] = JEM_STX;
    for (size_t i = 0; i < data_len; i++)
        w = stuff(out, w, data[i]);
    out[w++] = JEM_DLE; out[w++] = JEM_ETX;
    /* CRC little-endian, transmitted raw (not stuffed) */
    out[w++] = (uint8_t)(crc & 0xFF);
    out[w++] = (uint8_t)((crc >> 8) & 0xFF);

    return (int)w;
}

/* ---------- frame parse (pure) ---------- */

int jem_parse_frame(const uint8_t *raw, size_t raw_len,
                    uint8_t *out_func, uint8_t *out_sel,
                    uint8_t *out_data, size_t out_max, int *is_final)
{
    /* Need at least: DLE SOH ADDR FUNC SEL DLE STX DLE ETX CRClo CRChi */
    if (raw_len < 11) return -1;
    if (raw[0] != JEM_DLE || raw[1] != JEM_SOH) return -1;

    uint8_t logical[JEM_MAX_FRAME];
    size_t  n = 0;
    int     terminator = -1;

    /* Destuff raw[2 .. raw_len-3]; the last 2 bytes are the CRC. */
    size_t i = 2;
    size_t end = raw_len - 2;
    while (i < end) {
        uint8_t c = raw[i++];
        if (c == JEM_DLE) {
            if (i >= end) return -1;
            uint8_t d = raw[i++];
            if (d == JEM_DLE) {
                if (n >= sizeof(logical)) return -1;
                logical[n++] = JEM_DLE;                 /* escaped data 0x10 */
            } else if (d == JEM_ETX || d == JEM_ETB) {
                if (n >= sizeof(logical)) return -1;
                logical[n++] = d;                       /* terminator in CRC */
                terminator = d;
                break;
            } else {
                if (n >= sizeof(logical)) return -1;
                logical[n++] = d;                       /* STX (0x02), etc. */
            }
        } else {
            if (n >= sizeof(logical)) return -1;
            logical[n++] = c;
        }
    }

    if (terminator < 0) return -1;                       /* no ETX/ETB found */

    uint16_t rx_crc = (uint16_t)(raw[raw_len - 2] | (raw[raw_len - 1] << 8));
    uint16_t calc   = jem_crc16(logical, n);
    if (calc != rx_crc) {
        fprintf(stderr, "jem: CRC mismatch (got %04x, want %04x)\n", calc, rx_crc);
        return -1;
    }

    /* logical = [ADDR, FUNC, SEL, STX, data..., terminator] */
    if (n < 5 || logical[3] != JEM_STX) return -1;

    if (out_func) *out_func = logical[1];
    if (out_sel)  *out_sel  = logical[2];
    if (is_final) *is_final = (terminator == JEM_ETX);

    size_t data_len = n - 5;                             /* drop ADDR,FUNC,SEL,STX,term */
    if (data_len > out_max) data_len = out_max;
    if (out_data && data_len) memcpy(out_data, &logical[4], data_len);
    return (int)data_len;
}

/* ---------- I/O ---------- */

static int read_one(jem_link_t *lnk)
{
    uint8_t b;
    int n = transport_read(lnk->tp, &b, 1, lnk->timeout_ms);
    if (n == 1) return b;
    return -1;
}

int jem_send_command(jem_link_t *lnk, uint8_t func, uint8_t sel,
                     const uint8_t *data, size_t data_len)
{
    uint8_t wire[JEM_MAX_FRAME];
    int wn = jem_build_frame(lnk->master_addr, func, sel, data, data_len,
                             wire, sizeof(wire));
    if (wn < 0) return -1;
    frame_dump(lnk, "TX", wire, (size_t)wn);
    return transport_write(lnk->tp, wire, (size_t)wn);
}

int jem_send_ack(jem_link_t *lnk)
{
    uint8_t a = JEM_ACK;
    frame_dump(lnk, "TX", &a, 1);
    return transport_write(lnk->tp, &a, 1);
}

int jem_read_byte(jem_link_t *lnk)
{
    return read_one(lnk);
}

int jem_read_frame(jem_link_t *lnk, uint8_t *out_func, uint8_t *out_sel,
                   uint8_t *out_data, size_t out_max, int *is_final)
{
    uint8_t raw[JEM_MAX_FRAME];
    size_t  r = 0;
    int     c;

    /* 1. Sync on DLE SOH (skip any stray bytes). */
    for (;;) {
        c = read_one(lnk);
        if (c < 0) return -1;
        if (c != JEM_DLE) continue;
        c = read_one(lnk);
        if (c < 0) return -1;
        if (c == JEM_SOH) break;
        /* if c == DLE, loop treats it as a fresh DLE candidate next iter */
    }
    raw[r++] = JEM_DLE;
    raw[r++] = JEM_SOH;

    /* 2. Collect until DLE ETX/ETB, then 2 CRC bytes. DLE pairs are copied
     *    verbatim; a lone DLE only precedes DLE/ETX/ETB/STX in this protocol. */
    for (;;) {
        c = read_one(lnk);
        if (c < 0) return -1;
        if (r >= sizeof(raw)) return -1;
        raw[r++] = (uint8_t)c;

        if (c == JEM_DLE) {
            int d = read_one(lnk);
            if (d < 0) return -1;
            if (r >= sizeof(raw)) return -1;
            raw[r++] = (uint8_t)d;
            if (d == JEM_ETX || d == JEM_ETB) {
                int lo = read_one(lnk);
                int hi = read_one(lnk);
                if (lo < 0 || hi < 0) return -1;
                if (r + 2 > sizeof(raw)) return -1;
                raw[r++] = (uint8_t)lo;
                raw[r++] = (uint8_t)hi;
                break;
            }
        }
    }

    frame_dump(lnk, "RX", raw, r);
    return jem_parse_frame(raw, r, out_func, out_sel, out_data, out_max, is_final);
}

int jem_read_multiblock(jem_link_t *lnk, uint8_t func, uint8_t sel,
                        const uint8_t *req, size_t req_len,
                        uint8_t *out_data, size_t out_max)
{
    if (jem_send_command(lnk, func, sel, req, req_len) < 0) return -1;

    size_t total = 0;
    for (;;) {
        uint8_t fbuf[JEM_MAX_FRAME];
        uint8_t rf = 0, rs = 0;
        int     final = 0;

        int n = jem_read_frame(lnk, &rf, &rs, fbuf, sizeof(fbuf), &final);
        if (n < 0) return -1;

        size_t copy = (size_t)n;
        if (total + copy > out_max) copy = out_max - total;
        if (copy) { memcpy(out_data + total, fbuf, copy); total += copy; }

        if (final) break;
        if (total >= out_max) {
            fprintf(stderr, "jem: payload exceeds buffer (%zu bytes)\n", out_max);
            return -1;
        }
        if (jem_send_ack(lnk) < 0) return -1;   /* request next block */
    }
    return (int)total;
}

/* ---------- self-test ---------- */

static int hexparse(const char *hex, uint8_t *out, size_t out_max)
{
    size_t n = 0;
    const char *p = hex;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        unsigned v;
        if (sscanf(p, "%2x", &v) != 1) return -1;
        if (n >= out_max) return -1;
        out[n++] = (uint8_t)v;
        p += 2;
    }
    return (int)n;
}

int jem_link_selftest(void)
{
    int fails = 0;

    /* --- CRC over destuffed logical of the identity request --- */
    {
        uint8_t logical[] = {0xFE, 0x06, 0x01, JEM_STX, JEM_ETX};
        uint16_t crc = jem_crc16(logical, sizeof(logical));
        int ok = (crc == 0xFD39);   /* CRC bytes in log: 39 fd (LE) */
        printf("  [%s] crc16(identity req) = %04x (want fd39)\n",
               ok ? "PASS" : "FAIL", crc);
        if (!ok) fails++;
    }

    /* --- build identity request frame --- */
    {
        uint8_t exp[64]; int en = hexparse("10 01 fe 06 01 10 02 10 03 39 fd", exp, sizeof(exp));
        uint8_t got[64]; int gn = jem_build_frame(0xFE, 0x06, 0x01, NULL, 0, got, sizeof(got));
        int ok = (gn == en && en > 0 && memcmp(exp, got, (size_t)en) == 0);
        printf("  [%s] build(identity req) len=%d\n", ok ? "PASS" : "FAIL", gn);
        if (!ok) fails++;
    }

    /* --- build logon frame with password "000000" --- */
    {
        uint8_t exp[64];
        int en = hexparse("10 01 fe 50 01 10 02 30 30 30 30 30 30 10 03 96 1a", exp, sizeof(exp));
        uint8_t pw[6] = {'0','0','0','0','0','0'};
        uint8_t got[64];
        int gn = jem_build_frame(0xFE, 0x50, 0x01, pw, sizeof(pw), got, sizeof(got));
        int ok = (gn == en && en > 0 && memcmp(exp, got, (size_t)en) == 0);
        printf("  [%s] build(logon '000000') len=%d\n", ok ? "PASS" : "FAIL", gn);
        if (!ok) fails++;
    }

    /* --- parse the final ETX-terminated LP data block --- */
    {
        uint8_t raw[128];
        int rn = hexparse("10 01 01 44 01 10 02 00 00 00 00 22 4c 34 01 fd 00 01 00 01 00 01 00 10 03 c1 73",
                          raw, sizeof(raw));
        uint8_t func = 0, sel = 0, data[64]; int final = -1;
        int dn = jem_parse_frame(raw, (size_t)rn, &func, &sel, data, sizeof(data), &final);
        int ok = (dn == 16 && func == 0x44 && sel == 0x01 && final == 1);
        printf("  [%s] parse(final block) datalen=%d func=%02x sel=%02x final=%d\n",
               ok ? "PASS" : "FAIL", dn, func, sel, final);
        if (!ok) fails++;
    }

    /* --- parse an ETB (non-final) block that also contains an escaped 10 10 --- */
    {
        const char *hex =
            "10 01 01 44 01 10 02 00 00 00 00 00 00 00 00 e5 24 46 25 67 25 01 00 00 00 01 00 "
            "00 00 00 00 00 00 00 00 f6 24 5a 25 79 25 01 00 00 00 01 00 00 00 00 00 00 00 00 00 "
            "06 25 69 25 88 25 01 00 00 00 01 00 00 00 00 00 00 00 00 00 10 10 25 73 25 8e 25 01 "
            "00 00 00 01 00 00 00 00 00 00 00 00 00 25 25 86 25 a0 25 01 00 00 00 01 00 10 17 67 7a";
        uint8_t raw[256];
        int rn = hexparse(hex, raw, sizeof(raw));
        uint8_t func = 0, sel = 0, data[256]; int final = -1;
        int dn = jem_parse_frame(raw, (size_t)rn, &func, &sel, data, sizeof(data), &final);
        /* one 0x10 data byte should survive destuffing */
        int has10 = 0;
        for (int k = 0; k < dn; k++) if (data[k] == 0x10) has10 = 1;
        int ok = (dn == 100 && func == 0x44 && sel == 0x01 && final == 0 && has10);
        printf("  [%s] parse(ETB block, escaped 10) datalen=%d final=%d has0x10=%d\n",
               ok ? "PASS" : "FAIL", dn, final, has10);
        if (!ok) fails++;
    }

    printf("jem_link selftest: %s\n", fails ? "FAILURES" : "all passed");
    return fails;
}
