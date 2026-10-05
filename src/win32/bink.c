/* binkw32: the movies, decoded with FFmpeg's Bink video and audio decoders. The game reads Width,
 * Height, Frames and FrameNum (+0, +4, +8, +12) of the BINK struct in guest memory; the decoder state
 * stays on the host side. Video is paced by the clock at the file's frame rate, and the soundtrack
 * goes through the DirectSound mixer's FIFO (dsound.c), which plays it in step with that clock. */
#include "../runtime/rt.h"
#include <SDL2/SDL.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>

void ds_track_write(const float *lr, int frames);
void ds_track_clear(void);
void ds_track_pause(int on);

typedef struct {
    uint32_t guest;
    AVFormatContext *fmt;
    AVCodecContext *vdec, *adec;
    int vs, as, eof, sound;
    AVFrame *frame, *aframe;  /* the frame BinkCopyToBuffer copies; audio scratch */
    AVPacket *pkt;
    struct SwsContext *sws;
    SwrContext *swr;
    Uint64 t0, paused_at;     /* ms: when frame 1 was shown; when BinkPause stopped the clock */
} Movie;

static Movie movies[4];

static Movie *movie(uint32_t b) {
    for (int i = 0; i < 4; i++)
        if (b && movies[i].guest == b) return &movies[i];
    return NULL;
}

static AVCodecContext *open_decoder(AVStream *st) {
    const AVCodec *c = avcodec_find_decoder(st->codecpar->codec_id);
    AVCodecContext *d = c ? avcodec_alloc_context3(c) : NULL;
    if (d && (avcodec_parameters_to_context(d, st->codecpar) < 0 || avcodec_open2(d, c, NULL) < 0))
        avcodec_free_context(&d);
    return d;
}

static void close_movie(Movie *m) {
    avcodec_free_context(&m->vdec);
    avcodec_free_context(&m->adec);
    avformat_close_input(&m->fmt);
    av_frame_free(&m->frame);
    av_frame_free(&m->aframe);
    av_packet_free(&m->pkt);
    sws_freeContext(m->sws);
    swr_free(&m->swr);
    memset(m, 0, sizeof *m);
}

static void decode_audio(Movie *m) {
    if (avcodec_send_packet(m->adec, m->pkt) < 0) return;
    while (avcodec_receive_frame(m->adec, m->aframe) == 0) {
        int cap = swr_get_out_samples(m->swr, m->aframe->nb_samples);
        float *lr = malloc((size_t)cap * 8);
        uint8_t *out[1] = {(uint8_t *)lr};
        int n = swr_convert(m->swr, out, cap, (const uint8_t **)m->aframe->extended_data, m->aframe->nb_samples);
        if (n > 0 && m->sound) ds_track_write(lr, n);
        free(lr);
    }
}

/* Reads packets until the next video frame comes out, feeding the soundtrack on the way. */
static int decode_video(Movie *m) {
    for (;;) {
        int r = avcodec_receive_frame(m->vdec, m->frame);
        if (r == 0) return 1;
        if (r != AVERROR(EAGAIN) || m->eof) return 0;
        if (av_read_frame(m->fmt, m->pkt) < 0) {
            m->eof = 1;
            avcodec_send_packet(m->vdec, NULL);  /* drain */
            continue;
        }
        if (m->pkt->stream_index == m->vs) avcodec_send_packet(m->vdec, m->pkt);
        else if (m->pkt->stream_index == m->as && m->adec) decode_audio(m);
        av_packet_unref(m->pkt);
    }
}

WINAPI(BinkOpen, "_BinkOpen@8", 2) {
    char path[512];
    rt_path(GSTR(ARG(0)), path, sizeof path);
    FILE *f = fopen(path, "rb");
    uint32_t h[11];  /* the header gives Frames (+8) and the frame rate (+28, +32) */
    int ok = f && fread(h, 4, 11, f) == 11 && (h[0] & 0xFFFFFF) == 0x4B4942;  /* "BIK" */
    if (f) fclose(f);
    Movie *m = NULL;
    for (int i = 0; i < 4 && !m; i++)
        if (!movies[i].guest) m = &movies[i];
    if (!ok || !m) return 0;
    av_log_set_level(AV_LOG_ERROR);
    if (avformat_open_input(&m->fmt, path, NULL, NULL) < 0 || avformat_find_stream_info(m->fmt, NULL) < 0 ||
        (m->vs = av_find_best_stream(m->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0)) < 0 ||
        !(m->vdec = open_decoder(m->fmt->streams[m->vs]))) {
        rt_log("BinkOpen(%s): FFmpeg cannot play it", path);
        close_movie(m);
        return 0;
    }
    m->as = av_find_best_stream(m->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (m->as >= 0 && (m->adec = open_decoder(m->fmt->streams[m->as]))) {
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        if (swr_alloc_set_opts2(&m->swr, &stereo, AV_SAMPLE_FMT_FLT, 44100, &m->adec->ch_layout,
                                m->adec->sample_fmt, m->adec->sample_rate, 0, NULL) < 0 || swr_init(m->swr) < 0)
            avcodec_free_context(&m->adec);
    }
    m->frame = av_frame_alloc();
    m->aframe = av_frame_alloc();
    m->pkt = av_packet_alloc();
    m->sound = 1;
    m->guest = galloc(0x200);
    MEM32(m->guest) = h[5];       /* Width */
    MEM32(m->guest + 4) = h[6];   /* Height */
    MEM32(m->guest + 8) = h[2];   /* Frames */
    MEM32(m->guest + 12) = 1;     /* FrameNum, 1-based */
    MEM32(m->guest + 20) = h[7];  /* FrameRate */
    MEM32(m->guest + 24) = h[8];  /* FrameRateDiv */
    ds_track_clear();
    rt_log("BinkOpen(%s): %ux%u, %u frames at %.2f fps%s", GSTR(ARG(0)), h[5], h[6], h[2],
           h[8] ? (double)h[7] / h[8] : 0.0, m->adec ? " with sound" : "");
    return m->guest;
}

WINAPI(BinkClose, "_BinkClose@4", 1) {
    Movie *m = movie(ARG(0));
    if (m) {
        gfree(m->guest);
        close_movie(m);
        ds_track_clear();
    }
    return 0;
}

WINAPI(BinkDoFrame, "_BinkDoFrame@4", 1) {
    Movie *m = movie(ARG(0));
    if (m && !m->t0) m->t0 = SDL_GetTicks64();
    if (m) decode_video(m);  /* past the end, the last frame stays */
    return 0;
}

WINAPI(BinkNextFrame, "_BinkNextFrame@4", 1) {
    MEM32(ARG(0) + 16) = MEM32(ARG(0) + 12);  /* LastFrameNum */
    MEM32(ARG(0) + 12)++;
    return 0;
}

/* Nonzero while it is too early to show the next frame. */
WINAPI(BinkWait, "_BinkWait@4", 1) {
    Movie *m = movie(ARG(0));
    uint32_t b = ARG(0), rate = MEM32(b + 20), div = MEM32(b + 24);
    if (!m || !m->t0 || !rate || m->paused_at) return m && m->paused_at;
    Uint64 due = m->t0 + (Uint64)(MEM32(b + 12) - 1) * 1000 * div / rate;
    return SDL_GetTicks64() < due;
}

WINAPI(BinkPause, "_BinkPause@8", 2) {
    Movie *m = movie(ARG(0));
    if (!m || !ARG(1) == !m->paused_at) return ARG(1);
    if (ARG(1)) m->paused_at = SDL_GetTicks64();
    else {
        if (m->t0) m->t0 += SDL_GetTicks64() - m->paused_at;
        m->paused_at = 0;
    }
    ds_track_pause(ARG(1) != 0);
    return ARG(1);
}

/* BinkCopyToBuffer(bink, dest, pitch, height, x, y, flags): flags & 15 is the surface type. */
WINAPI(BinkCopyToBuffer, "_BinkCopyToBuffer@28", 7) {
    Movie *m = movie(ARG(0));
    uint32_t dest = ARG(1), pitch = ARG(2), rows = ARG(3), x = ARG(4), y = ARG(5), type = ARG(6) & 15;
    if (!m || !m->frame->data[0] || y >= rows) return 0;
    static const struct { enum AVPixelFormat pf; int bpp; uint16_t set; } fmts[16] = {
        [1] = {AV_PIX_FMT_BGR24, 3, 0},       [2] = {AV_PIX_FMT_RGB24, 3, 0},
        [3] = {AV_PIX_FMT_BGRA, 4, 0},        [4] = {AV_PIX_FMT_RGBA, 4, 0},
        [5] = {AV_PIX_FMT_BGRA, 4, 0},        [6] = {AV_PIX_FMT_RGBA, 4, 0},
        [7] = {AV_PIX_FMT_RGB444LE, 2, 0xF000}, [8] = {AV_PIX_FMT_RGB555LE, 2, 0x8000},
        [9] = {AV_PIX_FMT_RGB555LE, 2, 0},    [10] = {AV_PIX_FMT_RGB565LE, 2, 0},
    };
    if (!fmts[type].bpp) {
        static int warned;
        if (!warned++) rt_log("BinkCopyToBuffer: surface type %u not supported", type);
        return 0;
    }
    AVFrame *f = m->frame;
    int w = f->width, h = f->height < (int)(rows - y) ? f->height : (int)(rows - y);
    m->sws = sws_getCachedContext(m->sws, f->width, f->height, f->format, f->width, f->height, fmts[type].pf,
                                  SWS_POINT, NULL, NULL, NULL);
    uint8_t *dst[4] = {GPTR(dest + y * pitch + x * fmts[type].bpp)};
    int dst_pitch[4] = {(int)pitch};
    if (m->sws) sws_scale(m->sws, (const uint8_t *const *)f->data, f->linesize, 0, h, dst, dst_pitch);
    if (fmts[type].set)  /* the alpha bits a 16-bit format with alpha needs to show at all */
        for (int r = 0; r < h; r++)
            for (int c = 0; c < w; c++) ((uint16_t *)(dst[0] + r * pitch))[c] |= fmts[type].set;
    return 0;
}

WINAPI(BinkSetSoundOnOff, "_BinkSetSoundOnOff@8", 2) {
    Movie *m = movie(ARG(0));
    if (m && !(m->sound = ARG(1) != 0)) ds_track_clear();
    return ARG(1);
}
/* The soundtrack goes to the mixer directly, so DirectSound needs no setting up for Bink. */
WINAPI(BinkSetSoundSystem, "_BinkSetSoundSystem@8", 2) { return 1; }
WINAPI(BinkOpenDirectSound, "_BinkOpenDirectSound@4", 1) { return 0; }
WINAPI(BinkSetSoundTrack, "_BinkSetSoundTrack@8", 2) { return 0; }
