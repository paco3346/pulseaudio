#include <sys/soundcard.h>
#include <sys/ioctl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>

#include "iochannel.h"
#include "sink.h"
#include "source.h"
#include "module.h"
#include "oss-util.h"
#include "sample-util.h"

struct userdata {
    struct sink *sink;
    struct source *source;
    struct iochannel *io;
    struct core *core;

    struct memchunk memchunk, silence;

    uint32_t in_fragment_size, out_fragment_size, sample_size;

    int fd;
};

static void do_write(struct userdata *u) {
    struct memchunk *memchunk;
    ssize_t r;
    assert(u);

    if (!u->sink || !iochannel_is_writable(u->io))
        return;

    if (!u->memchunk.length) {
        if (sink_render(u->sink, u->out_fragment_size, &u->memchunk) < 0)
            memchunk = &u->silence;
        else
            memchunk = &u->memchunk;
    }

    assert(memchunk->memblock && memchunk->length);
    
    if ((r = iochannel_write(u->io, memchunk->memblock->data + memchunk->index, memchunk->length)) < 0) {
        fprintf(stderr, "write() failed: %s\n", strerror(errno));
        return;
    }

    if (memchunk == &u->silence)
        assert(r % u->sample_size == 0);
    else {
        u->memchunk.index += r;
        u->memchunk.length -= r;
        
        if (u->memchunk.length <= 0) {
            memblock_unref(u->memchunk.memblock);
            u->memchunk.memblock = NULL;
        }
    }
}

static void do_read(struct userdata *u) {
    struct memchunk memchunk;
    ssize_t r;
    assert(u);
    
    if (!u->source || !iochannel_is_readable(u->io))
        return;

    memchunk.memblock = memblock_new(u->in_fragment_size);
    assert(memchunk.memblock);
    if ((r = iochannel_read(u->io, memchunk.memblock->data, memchunk.memblock->length)) < 0) {
        memblock_unref(memchunk.memblock);
        fprintf(stderr, "read() failed: %s\n", strerror(errno));
        return;
    }

    assert(r <= (ssize_t) memchunk.memblock->length);
    memchunk.length = memchunk.memblock->length = r;
    memchunk.index = 0;

    source_post(u->source, &memchunk);
    memblock_unref(memchunk.memblock);
};

static void io_callback(struct iochannel *io, void*userdata) {
    struct userdata *u = userdata;
    assert(u);
    do_write(u);
    do_read(u);
}

static uint32_t sink_get_latency_cb(struct sink *s) {
    int arg;
    struct userdata *u = s->userdata;
    assert(s && u && u->sink);

    if (ioctl(u->fd, SNDCTL_DSP_GETODELAY, &arg) < 0) {
        fprintf(stderr, "module-oss: device doesn't support SNDCTL_DSP_GETODELAY.\n");
        s->get_latency = NULL;
        return 0;
    }

    return pa_samples_usec(arg, &s->sample_spec);
}

int module_init(struct core *c, struct module*m) {
    struct audio_buf_info info;
    struct userdata *u = NULL;
    char *p;
    int fd = -1;
    int frag_size, in_frag_size, out_frag_size;
    int mode;
    struct pa_sample_spec ss;
    assert(c && m);

    p = m->argument ? m->argument : "/dev/dsp";
    if ((fd = open(p, (mode = O_RDWR)|O_NDELAY)) >= 0) {
        int caps;

        ioctl(fd, SNDCTL_DSP_SETDUPLEX, 0);
        
        if (ioctl(fd, SNDCTL_DSP_GETCAPS, &caps) < 0) {
            fprintf(stderr, "SNDCTL_DSP_GETCAPS: %s\n", strerror(errno));
            goto fail;
        }

        if (!(caps & DSP_CAP_DUPLEX)) {
            close(fd);
            fd = -1;
        }
    }

    if (fd < 0) {
        if ((fd = open(p, (mode = O_WRONLY)|O_NDELAY)) < 0) {
            if ((fd = open(p, (mode = O_RDONLY)|O_NDELAY)) < 0) {
                fprintf(stderr, "open('%s'): %s\n", p, strerror(errno));
                goto fail;
            }
        }
    }

    fprintf(stderr, "module-oss: device opened in %s mode.\n", mode == O_WRONLY ? "O_WRONLY" : (mode == O_RDONLY ? "O_RDONLY" : "O_RDWR"));
    
    frag_size = ((int) 12 << 16) | 10; /* nfrags = 12; frag_size = 2^10 */
    if (ioctl(fd, SNDCTL_DSP_SETFRAGMENT, &frag_size) < 0) {
        fprintf(stderr, "SNDCTL_DSP_SETFRAGMENT: %s\n", strerror(errno));
        goto fail;
    }

    if (oss_auto_format(fd, &ss) < 0)
        goto fail;

    if (ioctl(fd, SNDCTL_DSP_GETBLKSIZE, &frag_size) < 0) {
        fprintf(stderr, "SNDCTL_DSP_GETBLKSIZE: %s\n", strerror(errno));
        goto fail;
    }
    assert(frag_size);
    in_frag_size = out_frag_size = frag_size;

    if (ioctl(fd, SNDCTL_DSP_GETISPACE, &info) >= 0) {
        fprintf(stderr, "module-oss: input -- %u fragments of size %u.\n", info.fragstotal, info.fragsize);
        in_frag_size = info.fragsize;
    }

    if (ioctl(fd, SNDCTL_DSP_GETOSPACE, &info) >= 0) {
        fprintf(stderr, "module-oss: output -- %u fragments of size %u.\n", info.fragstotal, info.fragsize);
        out_frag_size = info.fragsize;
    }

    u = malloc(sizeof(struct userdata));
    assert(u);

    u->core = c;

    if (mode != O_RDONLY) {
        u->sink = sink_new(c, "dsp", 0, &ss);
        assert(u->sink);
        u->sink->get_latency = sink_get_latency_cb;
        u->sink->userdata = u;
    } else
        u->sink = NULL;

    if (mode != O_WRONLY) {
        u->source = source_new(c, "dsp", 0, &ss);
        assert(u->source);
        u->source->userdata = u;
    } else
        u->source = NULL;

    assert(u->source || u->sink);

    u->io = iochannel_new(c->mainloop, u->source ? fd : -1, u->sink ? fd : 0);
    assert(u->io);
    iochannel_set_callback(u->io, io_callback, u);
    u->fd = fd;

    u->memchunk.memblock = NULL;
    u->memchunk.length = 0;
    u->sample_size = pa_sample_size(&ss);

    u->out_fragment_size = out_frag_size;
    u->in_fragment_size = in_frag_size;
    u->silence.memblock = memblock_new(u->silence.length = u->out_fragment_size);
    assert(u->silence.memblock);
    silence_memblock(u->silence.memblock, &ss);
    u->silence.index = 0;
    
    m->userdata = u;

    return 0;

fail:
    if (fd >= 0)
        close(fd);

    return -1;
}

void module_done(struct core *c, struct module*m) {
    struct userdata *u;
    assert(c && m);

    u = m->userdata;
    assert(u);
    
    if (u->memchunk.memblock)
        memblock_unref(u->memchunk.memblock);
    if (u->silence.memblock)
        memblock_unref(u->silence.memblock);

    if (u->sink)
        sink_free(u->sink);
    if (u->source)
        source_free(u->source);
    iochannel_free(u->io);
    free(u);
}
