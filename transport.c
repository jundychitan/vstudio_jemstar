#include "transport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <time.h>
#include <sys/time.h>

/* ---------- helpers ---------- */

static void hex_dump(const char *prefix, const uint8_t *data, size_t len)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm *t = localtime(&tv.tv_sec);

    printf("[%02d:%02d:%02d.%03d] %s %zu bytes:\n",
           t->tm_hour, t->tm_min, t->tm_sec, (int)(tv.tv_usec / 1000),
           prefix, len);

    for (size_t i = 0; i < len; i += 16) {
        printf("  %04zx: ", i);
        /* hex */
        for (size_t j = i; j < i + 16; j++) {
            if (j < len)
                printf("%02x ", data[j]);
            else
                printf("   ");
            if (j == i + 7) printf(" ");
        }
        /* ascii */
        printf(" |");
        for (size_t j = i; j < i + 16 && j < len; j++) {
            uint8_t c = data[j];
            printf("%c", (c >= 0x20 && c < 0x7f) ? c : '.');
        }
        printf("|\n");
    }
}

static speed_t baudrate_to_speed(int baud)
{
    switch (baud) {
        case 300:    return B300;
        case 600:    return B600;
        case 1200:   return B1200;
        case 2400:   return B2400;
        case 4800:   return B4800;
        case 9600:   return B9600;
        case 19200:  return B19200;
        case 38400:  return B38400;
        case 57600:  return B57600;
        case 115200: return B115200;
        default:     return B9600;
    }
}

/* ---------- serial ---------- */

int transport_open_serial(transport_t *tp, const char *device, const serial_params_t *params)
{
    int saved_verbose = tp->verbose;
    memset(tp, 0, sizeof(*tp));
    tp->verbose = saved_verbose;
    tp->type = TRANSPORT_SERIAL;
    tp->fd = -1;

    int fd = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        perror("open serial");
        return -1;
    }

    /* Clear non-blocking after open */
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

    tp->fd = fd;

    if (transport_set_serial_params(tp, params) < 0) {
        close(fd);
        tp->fd = -1;
        return -1;
    }

    return 0;
}

int transport_set_serial_params(transport_t *tp, const serial_params_t *params)
{
    if (tp->type != TRANSPORT_SERIAL || tp->fd < 0) return -1;

    struct termios tio;
    memset(&tio, 0, sizeof(tio));

    tio.c_cflag = CREAD | CLOCAL;

    /* data bits */
    if (params->databits == 7)
        tio.c_cflag |= CS7;
    else
        tio.c_cflag |= CS8;

    /* parity */
    if (params->parity == PARITY_EVEN) {
        tio.c_cflag |= PARENB;
    } else if (params->parity == PARITY_ODD) {
        tio.c_cflag |= PARENB | PARODD;
    }

    /* stop bits */
    if (params->stopbits == 2)
        tio.c_cflag |= CSTOPB;

    /* baud rate */
    speed_t spd = baudrate_to_speed(params->baudrate);
    cfsetispeed(&tio, spd);
    cfsetospeed(&tio, spd);

    /* raw mode */
    tio.c_iflag = 0;
    tio.c_oflag = 0;
    tio.c_lflag = 0;
    tio.c_cc[VMIN]  = 0;
    tio.c_cc[VTIME] = 1; /* 100ms inter-byte timeout */

    tcflush(tp->fd, TCIOFLUSH);

    if (tcsetattr(tp->fd, TCSANOW, &tio) < 0) {
        perror("tcsetattr");
        return -1;
    }

    return 0;
}

/* ---------- TCP ---------- */

int transport_open_tcp(transport_t *tp, const char *host, int port)
{
    int saved_verbose = tp->verbose;
    memset(tp, 0, sizeof(*tp));
    tp->verbose = saved_verbose;
    tp->type = TRANSPORT_TCP;
    tp->fd = -1;

    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    if (getaddrinfo(host, port_str, &hints, &res) != 0) {
        perror("getaddrinfo");
        return -1;
    }

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        perror("socket");
        freeaddrinfo(res);
        return -1;
    }

    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        perror("connect");
        close(fd);
        freeaddrinfo(res);
        return -1;
    }

    freeaddrinfo(res);
    tp->fd = fd;
    return 0;
}

/* ---------- read / write ---------- */

int transport_write(transport_t *tp, const uint8_t *buf, size_t len)
{
    if (tp->fd < 0) return -1;

    if (tp->verbose)
        hex_dump("TX", buf, len);

    size_t written = 0;
    while (written < len) {
        ssize_t n = write(tp->fd, buf + written, len - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("transport write");
            return -1;
        }
        written += (size_t)n;
    }

    if (tp->type == TRANSPORT_SERIAL)
        tcdrain(tp->fd);

    return 0;
}

int transport_read(transport_t *tp, uint8_t *buf, size_t max_len, int timeout_ms)
{
    if (tp->fd < 0) return -1;

    fd_set fds;
    struct timeval tv;
    FD_ZERO(&fds);
    FD_SET(tp->fd, &fds);
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    int ret = select(tp->fd + 1, &fds, NULL, NULL, &tv);
    if (ret < 0) {
        if (errno == EINTR) return 0;
        perror("select");
        return -1;
    }
    if (ret == 0) return 0; /* timeout */

    ssize_t n = read(tp->fd, buf, max_len);
    if (n < 0) {
        if (errno == EINTR || errno == EAGAIN) return 0;
        perror("transport read");
        return -1;
    }

    /* Only dump if verbose AND we're reading more than 1 byte
     * (single-byte reads are aggregated by read_until) */
    if (tp->verbose && n > 1)
        hex_dump("RX", buf, (size_t)n);

    return (int)n;
}

int transport_read_until(transport_t *tp, uint8_t *buf, size_t max_len,
                         uint8_t terminator, int timeout_ms)
{
    int saved_verbose = tp->verbose;
    tp->verbose = 0; /* suppress per-byte dumps */

    size_t pos = 0;
    while (pos < max_len) {
        int n = transport_read(tp, buf + pos, 1, timeout_ms);
        if (n < 0) { tp->verbose = saved_verbose; return -1; }
        if (n == 0) break; /* timeout */
        pos++;
        if (buf[pos - 1] == terminator) break;
    }

    tp->verbose = saved_verbose;
    if (tp->verbose && pos > 0)
        hex_dump("RX", buf, pos);

    return (int)pos;
}

/* ---------- misc ---------- */

void transport_flush_input(transport_t *tp)
{
    if (tp->type == TRANSPORT_SERIAL && tp->fd >= 0)
        tcflush(tp->fd, TCIFLUSH);

    /* Drain any pending bytes on TCP too */
    uint8_t tmp[256];
    while (transport_read(tp, tmp, sizeof(tmp), 50) > 0)
        ;
}

void transport_close(transport_t *tp)
{
    if (tp->fd >= 0) {
        close(tp->fd);
        tp->fd = -1;
    }
}
