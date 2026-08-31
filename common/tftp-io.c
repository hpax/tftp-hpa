/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2026 H. Peter Anvin
 */

#include "config.h"
#include "tftp-io.h"

#ifdef HAVE_PTHREAD
#include <pthread.h>
#endif

enum io_direction {
    IO_READ,
    IO_WRITE
};

enum slot_state {
    SLOT_EMPTY,
    SLOT_RESERVED,
    SLOT_READY,
    SLOT_WRITING
};

struct tftp_io_ops {
    int (*read_packet)(struct tftp_io *, struct tftphdr *);
    int (*write_packet)(struct tftp_io *, const struct tftphdr *, int);
    int (*write_finish)(struct tftp_io *);
};

struct tftp_io {
    FILE *file;
    const struct tftp_io_ops *ops;
    char *packets;
    int *lengths;
    enum slot_state *states;
    unsigned int slots;
    unsigned int window;
    unsigned int blocksize;
    unsigned int packetsize;
    unsigned int head;
    unsigned int tail;
    unsigned int count;
    unsigned int held;
    int error;
    int eof;
    int stopped;
    int threaded;
    int newline;
    int prevchar;
    int write_cr;
#ifdef HAVE_PTHREAD
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t changed;
    int lock_initialized;
    int cond_initialized;
#endif
};

static unsigned int io_next(const struct tftp_io *io, unsigned int n)
{
    return n + 1 == io->slots ? 0 : n + 1;
}

static struct tftphdr *io_packet(const struct tftp_io *io, unsigned int n)
{
    return (struct tftphdr *)(io->packets + (size_t)n * io->packetsize);
}

static void io_error(struct tftp_io *io, int error)
{
    if (!error)
        error = EIO;
    if (!io->error)
        io->error = error;
}

static int octet_read_packet(struct tftp_io *io, struct tftphdr *dp)
{
    size_t n;

    n = fread(dp->th_data, 1, io->blocksize, io->file);
    if (n < io->blocksize && ferror(io->file)) {
        errno = EIO;
        return -1;
    }
    return (int)n;
}

static int netascii_read_packet(struct tftp_io *io, struct tftphdr *dp)
{
    char *p;
    int c;
    unsigned int i;

    p = dp->th_data;
    for (i = 0; i < io->blocksize; i++) {
        if (io->newline) {
            c = io->prevchar == '\n' ? '\n' : '\0';
            io->newline = 0;
        } else {
            c = getc(io->file);
            if (c == EOF) {
                if (ferror(io->file)) {
                    errno = EIO;
                    return -1;
                }
                break;
            }
            if (c == '\n' || c == '\r') {
                io->prevchar = c;
                c = '\r';
                io->newline = 1;
            }
        }
        *p++ = c;
    }
    return (int)(p - dp->th_data);
}

static int octet_write_packet(struct tftp_io *io, const struct tftphdr *dp,
                              int count)
{
    if (fwrite(dp->th_data, 1, count, io->file) != (size_t)count) {
        errno = EIO;
        return -1;
    }
    return count;
}

static int netascii_write_packet(struct tftp_io *io,
                                 const struct tftphdr *dp, int count)
{
    const char *p;
    int c;
    int ct;

    p = dp->th_data;
    ct = count;
    while (ct--) {
        c = *p++;
        if (io->write_cr) {
            if (c == '\n') {
                c = '\n';
            } else if (c == '\0') {
                c = '\r';
            } else if (putc('\r', io->file) == EOF) {
                errno = EIO;
                return -1;
            }
            io->write_cr = 0;
            if (putc(c, io->file) == EOF) {
                errno = EIO;
                return -1;
            }
            continue;
        }
        if (c == '\r') {
            io->write_cr = 1;
            continue;
        }
        if (putc(c, io->file) == EOF) {
            errno = EIO;
            return -1;
        }
    }
    return count;
}

static int octet_write_finish(struct tftp_io *io)
{
    errno = 0;
    if (fflush(io->file)) {
        if (!errno)
            errno = EIO;
        return -1;
    }
    return 0;
}

static int netascii_write_finish(struct tftp_io *io)
{
    if (io->write_cr) {
        if (putc('\r', io->file) == EOF) {
            errno = EIO;
            return -1;
        }
        io->write_cr = 0;
    }
    errno = 0;
    if (fflush(io->file)) {
        if (!errno)
            errno = EIO;
        return -1;
    }
    return 0;
}

static const struct tftp_io_ops octet_io_ops = {
    octet_read_packet,
    octet_write_packet,
    octet_write_finish
};

static const struct tftp_io_ops netascii_io_ops = {
    netascii_read_packet,
    netascii_write_packet,
    netascii_write_finish
};

#ifdef HAVE_PTHREAD
static void *reader_thread(void *arg)
{
    struct tftp_io *io = arg;
    struct tftphdr *dp;
    int length;

    for (;;) {
        pthread_mutex_lock(&io->lock);
        while (io->count == io->slots && !io->stopped)
            pthread_cond_wait(&io->changed, &io->lock);
        if (io->stopped) {
            pthread_mutex_unlock(&io->lock);
            break;
        }
        dp = io_packet(io, io->tail);
        pthread_mutex_unlock(&io->lock);

        length = io->ops->read_packet(io, dp);

        pthread_mutex_lock(&io->lock);
        if (length < 0) {
            io_error(io, errno);
            pthread_cond_broadcast(&io->changed);
            pthread_mutex_unlock(&io->lock);
            break;
        }
        io->lengths[io->tail] = length + 4;
        io->tail = io_next(io, io->tail);
        io->count++;
        if (length != (int)io->blocksize)
            io->eof = 1;
        pthread_cond_broadcast(&io->changed);
        if (io->eof || io->stopped) {
            pthread_mutex_unlock(&io->lock);
            break;
        }
        pthread_mutex_unlock(&io->lock);
    }
    return NULL;
}

static void *writer_thread(void *arg)
{
    struct tftp_io *io = arg;
    struct tftphdr *dp;
    int length;

    for (;;) {
        pthread_mutex_lock(&io->lock);
        while (io->states[io->head] != SLOT_READY &&
               !io->stopped && !io->error)
            pthread_cond_wait(&io->changed, &io->lock);
        if (io->stopped || io->error) {
            pthread_mutex_unlock(&io->lock);
            break;
        }
        io->states[io->head] = SLOT_WRITING;
        dp = io_packet(io, io->head);
        length = io->lengths[io->head];
        pthread_mutex_unlock(&io->lock);

        if (io->ops->write_packet(io, dp, length) != length) {
            pthread_mutex_lock(&io->lock);
            io_error(io, errno);
            pthread_cond_broadcast(&io->changed);
            pthread_mutex_unlock(&io->lock);
            break;
        }

        pthread_mutex_lock(&io->lock);
        io->states[io->head] = SLOT_EMPTY;
        io->head = io_next(io, io->head);
        io->count--;
        pthread_cond_broadcast(&io->changed);
        pthread_mutex_unlock(&io->lock);
    }
    return NULL;
}
#endif

static struct tftp_io *io_start(FILE *file, int convert, unsigned int slots,
                                unsigned int window, unsigned int blocksize,
                                enum io_direction direction, int threaded)
{
    struct tftp_io *io;
#ifdef HAVE_PTHREAD
    int error;
#endif

    if (!slots || !blocksize || (direction == IO_READ && slots < window)) {
        errno = EINVAL;
        return NULL;
    }
#ifndef HAVE_PTHREAD
    if (threaded) {
        errno = ENOTSUP;
        return NULL;
    }
#endif

    io = xcalloc(1, sizeof(*io));
    io->file = file;
    io->slots = slots;
    io->window = window;
    io->blocksize = blocksize;
    io->packetsize = ((size_t)blocksize + 5) & ~(size_t)1;
    io->ops = convert ? &netascii_io_ops : &octet_io_ops;
    io->threaded = threaded;
    io->prevchar = -1;
    io->packets = xcalloc(io->slots, io->packetsize);
    io->lengths = xcalloc(io->slots, sizeof(*io->lengths));
    if (direction == IO_WRITE)
        io->states = xcalloc(io->slots, sizeof(*io->states));

#ifdef HAVE_PTHREAD
    if (!threaded)
        return io;

    error = pthread_mutex_init(&io->lock, NULL);
    if (!error)
        io->lock_initialized = 1;
    if (!error)
        error = pthread_cond_init(&io->changed, NULL);
    if (!error)
        io->cond_initialized = 1;
    if (!error)
        error = pthread_create(&io->thread, NULL,
                               direction == IO_READ ? reader_thread :
                                                      writer_thread, io);
    if (!error)
        return io;

    errno = error;
    if (io->cond_initialized)
        pthread_cond_destroy(&io->changed);
    if (io->lock_initialized)
        pthread_mutex_destroy(&io->lock);
    xfree(io->states);
    xfree(io->lengths);
    xfree(io->packets);
    xfree(io);
    return NULL;
#else
    return io;
#endif
}

struct tftp_io *tftp_io_reader_start(FILE *file, int convert,
                                     unsigned int window, unsigned int slots,
                                     unsigned int blocksize, int threaded)
{
    return io_start(file, convert, slots, window, blocksize, IO_READ,
                    threaded);
}

struct tftp_io *tftp_io_writer_start(FILE *file, int convert,
                                     unsigned int slots, unsigned int blocksize,
                                     int threaded)
{
    return io_start(file, convert, slots, 0, blocksize, IO_WRITE, threaded);
}

static int tftp_io_read_window(void *vctx, unsigned int *count, int *final)
{
    struct tftp_io *io = vctx;
    struct tftphdr *dp;
    int length;

#ifdef HAVE_PTHREAD
    if (io->threaded) {
        pthread_mutex_lock(&io->lock);
        while (io->count < io->window && !io->eof && !io->error)
            pthread_cond_wait(&io->changed, &io->lock);
    } else
#endif
    {
        while (io->count < io->window && !io->eof && !io->error) {
            dp = io_packet(io, io->tail);
            length = io->ops->read_packet(io, dp);
            if (length < 0) {
                io_error(io, errno);
                break;
            }
            io->lengths[io->tail] = length + 4;
            io->tail = io_next(io, io->tail);
            io->count++;
            if (length != (int)io->blocksize)
                io->eof = 1;
        }
    }
    if (io->error) {
        errno = io->error;
#ifdef HAVE_PTHREAD
        if (io->threaded)
            pthread_mutex_unlock(&io->lock);
#endif
        return -1;
    }

    io->held = io->count < io->window ? io->count : io->window;
    *count = io->held;
    *final = io->eof && io->held == io->count;
#ifdef HAVE_PTHREAD
    if (io->threaded)
        pthread_mutex_unlock(&io->lock);
#endif
    return 0;
}

static struct tftphdr *tftp_io_read_packet(void *vctx, unsigned int n)
{
    struct tftp_io *io = vctx;

    if (n >= io->held) {
        errno = EINVAL;
        return NULL;
    }
    return io_packet(io, (io->head + n) % io->slots);
}

static int tftp_io_read_length(void *vctx, unsigned int n)
{
    struct tftp_io *io = vctx;

    if (n >= io->held) {
        errno = EINVAL;
        return -1;
    }
    return io->lengths[(io->head + n) % io->slots];
}

static void tftp_io_read_release(void *vctx)
{
    struct tftp_io *io = vctx;

#ifdef HAVE_PTHREAD
    if (io->threaded)
        pthread_mutex_lock(&io->lock);
#endif
    while (io->held) {
        io->head = io_next(io, io->head);
        io->count--;
        io->held--;
    }
#ifdef HAVE_PTHREAD
    if (io->threaded) {
        pthread_cond_broadcast(&io->changed);
        pthread_mutex_unlock(&io->lock);
    }
#endif
}

static struct tftphdr *tftp_io_writer_reserve(struct tftp_io *io)
{
    struct tftphdr *dp;

#ifdef HAVE_PTHREAD
    if (io->threaded) {
        pthread_mutex_lock(&io->lock);
        while (io->states[io->tail] != SLOT_EMPTY && !io->error)
            pthread_cond_wait(&io->changed, &io->lock);
    } else
#endif
    if (io->states[io->tail] != SLOT_EMPTY && !io->error) {
        errno = EIO;
        return NULL;
    }
    if (io->error) {
        errno = io->error;
#ifdef HAVE_PTHREAD
        if (io->threaded)
            pthread_mutex_unlock(&io->lock);
#endif
        return NULL;
    }
    io->states[io->tail] = SLOT_RESERVED;
    dp = io_packet(io, io->tail);
#ifdef HAVE_PTHREAD
    if (io->threaded)
        pthread_mutex_unlock(&io->lock);
#endif
    return dp;
}

static int tftp_io_write_publish(void *vctx, int length)
{
    struct tftp_io *io = vctx;

    if (length < 0 || length > (int)io->blocksize) {
        errno = EINVAL;
        return -1;
    }
#ifdef HAVE_PTHREAD
    if (io->threaded)
        pthread_mutex_lock(&io->lock);
#endif
    if (io->states[io->tail] != SLOT_RESERVED) {
        errno = EIO;
#ifdef HAVE_PTHREAD
        if (io->threaded)
            pthread_mutex_unlock(&io->lock);
#endif
        return -1;
    }
    io->lengths[io->tail] = length;
    io->states[io->tail] = SLOT_READY;
    io->tail = io_next(io, io->tail);
    io->count++;
#ifdef HAVE_PTHREAD
    if (io->threaded) {
        pthread_cond_broadcast(&io->changed);
        pthread_mutex_unlock(&io->lock);
    }
#endif
    return 0;
}

static struct tftphdr *tftp_io_xfer_write_reserve(void *vctx)
{
    return tftp_io_writer_reserve(vctx);
}

static int tftp_io_write_drain(void *vctx)
{
    struct tftp_io *io = vctx;
    struct tftphdr *dp;
    int length;
    int error;

#ifdef HAVE_PTHREAD
    if (io->threaded) {
        pthread_mutex_lock(&io->lock);
        while (io->count && !io->error)
            pthread_cond_wait(&io->changed, &io->lock);
        error = io->error;
        pthread_mutex_unlock(&io->lock);
    } else
#endif
    {
        while (io->count && !io->error) {
            if (io->states[io->head] != SLOT_READY) {
                io_error(io, EIO);
                break;
            }
            io->states[io->head] = SLOT_WRITING;
            dp = io_packet(io, io->head);
            length = io->lengths[io->head];
            if (io->ops->write_packet(io, dp, length) != length) {
                io_error(io, errno);
                break;
            }
            io->states[io->head] = SLOT_EMPTY;
            io->head = io_next(io, io->head);
            io->count--;
        }
        error = io->error;
    }
    if (!error && fflush(io->file)) {
        error = errno ? errno : EIO;
#ifdef HAVE_PTHREAD
    if (io->threaded)
        pthread_mutex_lock(&io->lock);
#endif
    io_error(io, error);
#ifdef HAVE_PTHREAD
    if (io->threaded)
        pthread_mutex_unlock(&io->lock);
#endif
    }
    if (error)
        errno = error;
    return error ? -1 : 0;
}

static int tftp_io_write_finish(void *vctx)
{
    struct tftp_io *io = vctx;

    if (io->ops->write_finish(io) < 0) {
        io_error(io, errno);
        errno = io->error;
        return -1;
    }
    return 0;
}

void tftp_io_stop(struct tftp_io *io)
{
    if (!io)
        return;
#ifdef HAVE_PTHREAD
    if (io->threaded) {
        pthread_mutex_lock(&io->lock);
        io->stopped = 1;
        pthread_cond_broadcast(&io->changed);
        pthread_mutex_unlock(&io->lock);
        (void)pthread_join(io->thread, NULL);
        pthread_cond_destroy(&io->changed);
        pthread_mutex_destroy(&io->lock);
    }
#endif
    xfree(io->states);
    xfree(io->lengths);
    xfree(io->packets);
    xfree(io);
}

const struct tftp_xfer_io_ops tftp_io_xfer_ops = {
    tftp_io_read_window,
    tftp_io_read_packet,
    tftp_io_read_length,
    tftp_io_read_release,
    tftp_io_xfer_write_reserve,
    tftp_io_write_publish,
    tftp_io_write_drain,
    tftp_io_write_finish
};
