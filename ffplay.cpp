/*
 * Copyright (c) 2003 Fabrice Bellard
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * simple media player based on the FFmpeg libraries
 */

/***
 以后如果要更新ffplay.c的新代码时,按照下面步骤做:
 1.
 用新的ffplay.c的代码覆盖这个工程的ffplay.c的代码,
 然后找出不同点.
 2.
 根据不同点的那个方法一个一个同步到这个文件中.

 所以这里已经定义的方法名不要去改变

 ffplay version 4.2.2 Copyright (c) 2003-2020 the FFmpeg developers
 ...
 此log是在cmdutils.cpp文件的print_program_info(int flags, int level)方法中打印出来的
 PRINT_LIB_INFO
 */

#include <sys/time.h>
#include <pthread.h>
#include <inttypes.h>
#include <math.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include "config.h"
// 使用C语言写的代码,如果要在C++中使用,那么需要使用这种方式导入头文件
#ifdef __cplusplus
extern "C" {
#endif

#include "libavutil/avstring.h"
#include "libavutil/eval.h"
#include "libavutil/mathematics.h"
#include "libavutil/pixdesc.h"
#include "libavutil/imgutils.h"
#include "libavutil/dict.h"
#include "libavutil/parseutils.h"
#include "libavutil/samplefmt.h"
#include "libavutil/avassert.h"
#include "libavutil/time.h"
#include "libavutil/bprint.h"
#include "libavformat/avformat.h"
#include "libavdevice/avdevice.h"
#include "libswscale/swscale.h"
#include "libavutil/opt.h"
#include "libavcodec/avfft.h"
#include "libswresample/swresample.h"

#if CONFIG_AVFILTER
# include "libavfilter/avfilter.h"
# include "libavfilter/buffersink.h"
# include "libavfilter/buffersrc.h"
#endif

#include <SDL.h>
#include <SDL_thread.h>

#include "cmdutils.h"

#ifdef __cplusplus
}
#endif

#define MAX_QUEUE_SIZE (15 * 1024 * 1024)
//#define MAX_QUEUE_SIZE (1024 * 1024 * 1024)
//#define MIN_FRAMES 25
#define MIN_FRAMES 10000
#define EXTERNAL_CLOCK_MIN_FRAMES 2
#define EXTERNAL_CLOCK_MAX_FRAMES 10

/* Minimum SDL audio buffer size, in samples. */
#define SDL_AUDIO_MIN_BUFFER_SIZE 512
/* Calculate actual buffer size keeping in mind not cause too frequent audio callbacks */
#define SDL_AUDIO_MAX_CALLBACKS_PER_SEC 30

/* Step size for volume control in dB */
#define SDL_VOLUME_STEP (0.75)

/* no AV sync correction is done if below the minimum AV sync threshold */
#define AV_SYNC_THRESHOLD_MIN 0.04
/* AV sync correction is done if above the maximum AV sync threshold */
#define AV_SYNC_THRESHOLD_MAX 0.1
/* If a frame duration is longer than this, it will not be duplicated to compensate AV sync */
#define AV_SYNC_FRAMEDUP_THRESHOLD 0.1
/* no AV correction is done if too big error */
#define AV_NOSYNC_THRESHOLD 10.0

/* maximum audio speed change to get correct sync */
#define SAMPLE_CORRECTION_PERCENT_MAX 10

/* external clock speed adjustment constants for realtime sources based on buffer fullness */
#define EXTERNAL_CLOCK_SPEED_MIN  0.900
#define EXTERNAL_CLOCK_SPEED_MAX  1.010
#define EXTERNAL_CLOCK_SPEED_STEP 0.001

/* we use about AUDIO_DIFF_AVG_NB A-V differences to make the average */
#define AUDIO_DIFF_AVG_NB   20

/* polls for possible required screen refresh at least this often, should be less than 1/fps */
#define REFRESH_RATE 0.01

/* NOTE: the size must be big enough to compensate the hardware audio buffersize size */
/* TODO: We assume that a decoded and resampled frame fits into this buffer */
#define SAMPLE_ARRAY_SIZE (8 * 65536)

#define CURSOR_HIDE_DELAY 1000000 // 1秒

#define USE_ONEPASS_SUBTITLE_RENDER 1

#define OS_ANDROID
#define MAX_AUDIO_FRAME_SIZE 19200

static unsigned sws_flags = SWS_BICUBIC;

enum {
    AV_SYNC_AUDIO_MASTER, /* default choice */
    AV_SYNC_VIDEO_MASTER,
    AV_SYNC_EXTERNAL_CLOCK, /* synchronize to an external clock */
};

// 就是一个节点(node)
typedef struct MyAVPacketList {
    AVPacket pkt;
    struct MyAVPacketList *next;
    int serial;
} MyAVPacketList;

typedef struct PacketQueue {
    // first_pkt是flush_pkt,在packet_queue_start中实现
    MyAVPacketList *first_pkt, *last_pkt;
    // packet_queue_init(0) MyAVPacketList的个数
    int nb_packets;
    // init(0)
    int size;
    // init(0)
    int64_t duration;
    // packet_queue_init(1) packet_queue_start(0)
    int abort_request;
    // init(0) packet_queue_put_private(++)
    int serial;
    // packet_queue_init
    pthread_mutex_t pmutex;
    // packet_queue_init
    pthread_cond_t pcond;
} PacketQueue;

// 保存解码帧的个数
#define VIDEO_PICTURE_QUEUE_SIZE 3
#define SAMPLE_QUEUE_SIZE 9
#define SUBPICTURE_QUEUE_SIZE 16
#define FRAME_QUEUE_SIZE FFMAX(SAMPLE_QUEUE_SIZE, FFMAX(VIDEO_PICTURE_QUEUE_SIZE, SUBPICTURE_QUEUE_SIZE))

typedef struct AudioParams {
    int freq;
    int channels;
    int64_t channel_layout;
    enum AVSampleFormat fmt;
    int frame_size;
    int bytes_per_sec;
} AudioParams;

typedef struct Clock {
    double pts;           /* clock base */
    double pts_drift;     /* clock base minus time at which we updated the clock */
    double last_updated;
    // init(1.0)
    double speed;
    int serial;           /* clock is based on a packet with this serial */
    // init(0)
    int paused;
    int *queue_serial;    /* pointer to the current packet queue serial, used for obsolete clock detection */
} Clock;

/* Common struct for handling all types of decoded data and allocated render buffers. */
typedef struct Frame {
    AVFrame *frame;
    AVSubtitle sub;
    int serial;
    double pts;           /* presentation timestamp for the frame */
    double duration;      /* estimated duration of the frame */
    int64_t pos;          /* byte position of the frame in the input file */
    int width;
    int height;
    int format;
    AVRational sar;
    int uploaded;
    int flip_v;
} Frame;

// 存放解码帧
typedef struct FrameQueue {
    Frame queue[FRAME_QUEUE_SIZE];
    int rindex;
    int windex;
    int size;
    // video(3) audio(9) subtitle(16)
    int max_size;
    // video(1) audio(1) subtitle(0)
    int keep_last;
    int rindex_shown;
    PacketQueue *pktq;
    // frame_queue_init
    pthread_mutex_t pmutex;
    // frame_queue_init
    pthread_cond_t pcond;
} FrameQueue;

typedef struct Decoder {
    AVPacket pkt;
    PacketQueue *queue;
    AVCodecContext *avctx;
    // decoder_init(-1)
    int pkt_serial;
    int finished;
    int packet_pending;
    // 指向is->continue_read_thread
    int64_t start_pts;
    AVRational start_pts_tb;
    int64_t next_pts;
    AVRational next_pts_tb;
    // decoder_init 指针指向VideoState::continue_read_thread
    pthread_cond_t *pempty_queue_cond;
    // decoder_start
    SDL_Thread *decoder_tid;
} Decoder;

typedef struct VideoState {
    AVFormatContext *ic;
    AVInputFormat *iformat;
    // 是否强制停止(0运行1停止) init(0) stream_close(1)
    int abort_request;
    int force_refresh;
    int paused;
    int last_paused;
    int queue_attachments_req;
    // 需要seek时为1,否则为0
    int seek_req;
    int seek_flags;
    int64_t seek_pos;
    int64_t seek_rel;
    int read_pause_return;
    int realtime;
    // stream_component_open(0)
    int eof;
    // init(以audio为基准进行音视频同步)
    int av_sync_type;// AV_SYNC_AUDIO_MASTER

    Clock vidclk;
    Clock audclk;
    Clock extclk;

    FrameQueue pictq;
    FrameQueue sampq;
    FrameQueue subpq;

    PacketQueue videoq;
    PacketQueue audioq;
    PacketQueue subtitleq;

    Decoder viddec;
    Decoder auddec;
    Decoder subdec;

    // stream_component_open
    // 对应的值都是相等的
    int video_stream, audio_stream, subtitle_stream;
    int last_video_stream, last_audio_stream, last_subtitle_stream;
    // stream_component_open
    AVStream *video_st, *audio_st, *subtitle_st;


    double audio_clock;
    // stream_open(-1)
    int audio_clock_serial;
    double audio_diff_cum; /* used for AV difference average computation */
    double audio_diff_avg_coef;
    double audio_diff_threshold;
    int audio_diff_avg_count;
    int audio_hw_buf_size;
    uint8_t *audio_buf;
    uint8_t *audio_buf1;
    unsigned int audio_buf_size; /* in bytes */
    unsigned int audio_buf1_size;
    int audio_buf_index; /* in bytes */
    int audio_write_buf_size;
    // 音量大小
    int audio_volume;
    // init(0)
    int muted;
    struct AudioParams audio_src;
#if CONFIG_AVFILTER
    struct AudioParams audio_filter_src;
#endif
    struct AudioParams audio_tgt;
    struct SwrContext *swr_ctx;
    int frame_drops_early;
    int frame_drops_late;


    int16_t sample_array[SAMPLE_ARRAY_SIZE];
    int sample_array_index;
    int last_i_start;
    RDFTContext *rdft;
    int rdft_bits;
    FFTSample *rdft_data;
    int xpos;
    double last_vis_time;

    double frame_timer;
    double frame_last_returned_time;
    double frame_last_filter_delay;
    double max_frame_duration;      // maximum duration of a frame - above this, we consider the jump a timestamp discontinuity
    struct SwsContext *img_convert_ctx;
    struct SwsContext *sub_convert_ctx;

    // 媒体播放路径
    char *filename;
    int width, height, xleft, ytop;
    int step;

#if CONFIG_AVFILTER
    int vfilter_idx;
    AVFilterContext *in_video_filter;   // the first filter in the video chain
    AVFilterContext *out_video_filter;  // the last filter in the video chain
    AVFilterContext *in_audio_filter;   // the first filter in the audio chain
    AVFilterContext *out_audio_filter;  // the last filter in the audio chain
    AVFilterGraph *agraph;              // audio filter graph
#endif

#ifdef OS_ANDROID
    // audio
    unsigned char *audioOutBuffer = nullptr;
    size_t audioOutBufferSize = 0;
    // video
    unsigned char *videoOutBuffer = nullptr;
    size_t videoOutBufferSize = 0;
    AVFrame *rgbAVFrame = nullptr;
#endif

    enum ShowMode {
        SHOW_MODE_NONE = -1, SHOW_MODE_VIDEO = 0, SHOW_MODE_WAVES, SHOW_MODE_RDFT, SHOW_MODE_NB
    } show_mode;

    // SDL
    SDL_Thread *read_tid;
    SDL_Texture *vis_texture;
    SDL_Texture *sub_texture;
    SDL_Texture *vid_texture;
    // stream_open
    pthread_cond_t pcontinue_read_thread;

} VideoState;

static VideoState *video_state;

/* options specified by the user */
static AVInputFormat *file_iformat;
static const char *input_filename;
static const char *window_title;
static int default_width = 640;
static int default_height = 480;
static int screen_width = 0;
static int screen_height = 0;
static int screen_left = SDL_WINDOWPOS_CENTERED;
static int screen_top = SDL_WINDOWPOS_CENTERED;
static int audio_disable;
static int video_disable;
static int subtitle_disable;
static const char *wanted_stream_spec[AVMEDIA_TYPE_NB] = {0};
static int seek_by_bytes = -1;
static float seek_interval = 10;
static int display_disable;
static int borderless;
static int alwaysontop;
static int startup_volume = 100;
static int show_status = -1;
static int av_sync_type = AV_SYNC_AUDIO_MASTER;
// 开始播放时需要seek到的那个时间点
static int64_t start_time = AV_NOPTS_VALUE;
static int64_t duration = AV_NOPTS_VALUE;
static int fast = 0;
static int genpts = 0;
static int lowres = 0;
static int decoder_reorder_pts = -1;
static int autoexit;
static int exit_on_keydown;
static int exit_on_mousedown;
static int loop = 1;
static int framedrop = -1;
static int infinite_buffer = -1;
static enum VideoState::ShowMode show_mode = VideoState::SHOW_MODE_NONE;
static const char *video_codec_name;
static const char *audio_codec_name;
static const char *subtitle_codec_name;
//double rdftspeed = 0.02;
static int64_t cursor_last_shown;
// 是否显示鼠标(0表示显示,1表示隐藏)
static int cursor_hidden = 0;
#if CONFIG_AVFILTER
static const char **vfilters_list = nullptr;
static int nb_vfilters = 0;
static char *afilters = nullptr;
#endif
static int autorotate = 1;
static int find_stream_info = 1;
static int filter_nbthreads = 0;

/* current context */
static int is_full_screen;
static int64_t audio_callback_time;

static AVPacket flush_pkt;

static long media_duration = -1;
static int audio_packets = 0;
static int video_packets = 0;
static int subtitle_packets = 0;

#define FF_QUIT_EVENT    (SDL_USEREVENT + 2)

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_RendererInfo renderer_info = {0};
static SDL_AudioDeviceID audio_dev;

static const struct TextureFormatEntry {
    enum AVPixelFormat format;
    int texture_fmt;
} sdl_texture_format_map[] = {
        {AV_PIX_FMT_RGB8,           SDL_PIXELFORMAT_RGB332},
        {AV_PIX_FMT_RGB444,         SDL_PIXELFORMAT_RGB444},
        {AV_PIX_FMT_RGB555,         SDL_PIXELFORMAT_RGB555},
        {AV_PIX_FMT_BGR555,         SDL_PIXELFORMAT_BGR555},
        {AV_PIX_FMT_RGB565,         SDL_PIXELFORMAT_RGB565},
        {AV_PIX_FMT_BGR565,         SDL_PIXELFORMAT_BGR565},
        {AV_PIX_FMT_RGB24,          SDL_PIXELFORMAT_RGB24},
        {AV_PIX_FMT_BGR24,          SDL_PIXELFORMAT_BGR24},
        {AV_PIX_FMT_0RGB32,         SDL_PIXELFORMAT_RGB888},
        {AV_PIX_FMT_0BGR32,         SDL_PIXELFORMAT_BGR888},
        {AV_PIX_FMT_NE(RGB0, 0BGR), SDL_PIXELFORMAT_RGBX8888},
        {AV_PIX_FMT_NE(BGR0, 0RGB), SDL_PIXELFORMAT_BGRX8888},
        {AV_PIX_FMT_RGB32,          SDL_PIXELFORMAT_ARGB8888},
        {AV_PIX_FMT_RGB32_1,        SDL_PIXELFORMAT_RGBA8888},
        {AV_PIX_FMT_BGR32,          SDL_PIXELFORMAT_ABGR8888},
        {AV_PIX_FMT_BGR32_1,        SDL_PIXELFORMAT_BGRA8888},
        {AV_PIX_FMT_YUV420P,        SDL_PIXELFORMAT_IYUV},
        {AV_PIX_FMT_YUYV422,        SDL_PIXELFORMAT_YUY2},
        {AV_PIX_FMT_UYVY422,        SDL_PIXELFORMAT_UYVY},
        {AV_PIX_FMT_NONE,           SDL_PIXELFORMAT_UNKNOWN},
};

const char program_name[] = "ffplay";
const int program_birth_year = 2003;
double rdftspeed = 0.02;


#if CONFIG_AVFILTER

static int opt_add_vfilter(void *optctx, const char *opt, const char *arg) {
    //GROW_ARRAY(vfilters_list, nb_vfilters);
    vfilters_list = static_cast<const char **>(grow_array(vfilters_list, sizeof(*vfilters_list),
                                                          &nb_vfilters, nb_vfilters + 1));

    vfilters_list[nb_vfilters - 1] = arg;
    return 0;
}

#endif

static inline
int cmp_audio_fmts(enum AVSampleFormat fmt1, int64_t channel_count1,
                   enum AVSampleFormat fmt2, int64_t channel_count2) {
    /* If channel count == 1, planar and non-planar formats are the same */
    if (channel_count1 == 1 && channel_count2 == 1)
        return av_get_packed_sample_fmt(fmt1) != av_get_packed_sample_fmt(fmt2);
    else
        return channel_count1 != channel_count2 || fmt1 != fmt2;
}

static inline
int64_t get_valid_channel_layout(int64_t channel_layout, int channels) {
    if (channel_layout && av_get_channel_layout_nb_channels(channel_layout) == channels)
        return channel_layout;
    else
        return 0;
}

static int packet_queue_put_private(PacketQueue *q, AVPacket *pkt) {
    if (q->abort_request)
        return -1;

    MyAVPacketList *pkt1;
    pkt1 = static_cast<MyAVPacketList *>(av_malloc(sizeof(MyAVPacketList)));
    if (!pkt1)
        return -1;

    pkt1->pkt = *pkt;
    pkt1->next = nullptr;
    if (pkt == &flush_pkt) {
        q->serial++;
        printf("packet_queue_put_private() q->serial = %d\n", q->serial);
    }
    pkt1->serial = q->serial;

    if (!q->last_pkt)
        q->first_pkt = pkt1;
    else
        q->last_pkt->next = pkt1;

    q->last_pkt = pkt1;
    q->nb_packets++;
    q->size += pkt1->pkt.size + sizeof(*pkt1);
    q->duration += pkt1->pkt.duration;
    /* XXX: should duplicate packet data in DV case */
    pthread_cond_signal(&q->pcond);

    return 0;
}

static int packet_queue_put(PacketQueue *q, AVPacket *pkt) {
    int ret;

    pthread_mutex_lock(&q->pmutex);
    // 停止播放或者申请内存失败时返回-1,否则返回0
    ret = packet_queue_put_private(q, pkt);
    if (seek_by_bytes && pkt != &flush_pkt) {
        if (pkt->stream_index == video_state->audio_stream
            && audio_packets != q->nb_packets
            && q->nb_packets % 100 == 0) {
            audio_packets = q->nb_packets;
            printf("packet_queue_put() audio    packets = %d\n", q->nb_packets);
        } else if (pkt->stream_index == video_state->video_stream
                   && video_packets != q->nb_packets
                   && q->nb_packets % 100 == 0) {
            video_packets = q->nb_packets;
            printf("packet_queue_put() video    packets = %d\n", q->nb_packets);
        } else if (pkt->stream_index == video_state->subtitle_stream
                   && subtitle_packets != q->nb_packets
                   && q->nb_packets % 100 == 0) {
            subtitle_packets = q->nb_packets;
            printf("packet_queue_put() subtitle packets = %d\n", q->nb_packets);
        }
    }
    pthread_mutex_unlock(&q->pmutex);

    if (pkt != &flush_pkt && ret < 0)
        av_packet_unref(pkt);

    return ret;
}

static int packet_queue_put_nullpacket(PacketQueue *q, int stream_index) {
    AVPacket pkt1, *pkt = &pkt1;
    av_init_packet(pkt);
    pkt->data = nullptr;
    pkt->size = 0;
    pkt->stream_index = stream_index;
    return packet_queue_put(q, pkt);
}

/* packet queue handling */
static int packet_queue_init(PacketQueue *q) {
    printf("packet_queue_init() start\n");
    memset(q, 0, sizeof(PacketQueue));
    q->pmutex = PTHREAD_MUTEX_INITIALIZER;
    q->pcond = PTHREAD_COND_INITIALIZER;
    q->abort_request = 1;
    printf("packet_queue_init() end\n");
    return 0;
}

static void packet_queue_flush(PacketQueue *q) {
    MyAVPacketList *pkt, *pkt1;

    pthread_mutex_lock(&q->pmutex);
    for (pkt = q->first_pkt; pkt; pkt = pkt1) {
        pkt1 = pkt->next;
        av_packet_unref(&pkt->pkt);
        av_freep(&pkt);
    }
    q->first_pkt = nullptr;
    q->last_pkt = nullptr;
    q->nb_packets = 0;
    q->size = 0;
    q->duration = 0;
    pthread_mutex_unlock(&q->pmutex);
}

static void packet_queue_destroy(PacketQueue *q) {
    packet_queue_flush(q);
    pthread_mutex_destroy(&q->pmutex);
    pthread_cond_destroy(&q->pcond);
}

static void packet_queue_abort(PacketQueue *q) {
    pthread_mutex_lock(&q->pmutex);
    q->abort_request = 1;
    pthread_cond_signal(&q->pcond);
    pthread_mutex_unlock(&q->pmutex);
}

static void packet_queue_start(PacketQueue *q) {
    printf("packet_queue_start() start\n");
    pthread_mutex_lock(&q->pmutex);
    q->abort_request = 0;
    packet_queue_put_private(q, &flush_pkt);
    pthread_mutex_unlock(&q->pmutex);
    printf("packet_queue_start() end\n");
}

/* return < 0 if aborted, 0 if no packet and > 0 if packet.  */
static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block, int *serial) {
    MyAVPacketList *pkt1;
    int ret;

    pthread_mutex_lock(&q->pmutex);

    for (;;) {
        if (q->abort_request) {
            ret = -1;
            break;
        }

        pkt1 = q->first_pkt;
        if (pkt1) {
            q->first_pkt = pkt1->next;
            if (!q->first_pkt)
                q->last_pkt = nullptr;
            q->nb_packets--;
            q->size -= pkt1->pkt.size + sizeof(*pkt1);
            q->duration -= pkt1->pkt.duration;
            *pkt = pkt1->pkt;
            if (serial)
                *serial = pkt1->serial;
            av_free(pkt1);
            ret = 1;
            break;
        } else if (!block) {
            ret = 0;
            break;
        } else {
            pthread_cond_wait(&q->pcond, &q->pmutex);
        }
    }

    if (seek_by_bytes && pkt != &flush_pkt) {
        if (pkt->stream_index == video_state->audio_stream
            && audio_packets != q->nb_packets
            && q->nb_packets % 100 == 0) {
            audio_packets = q->nb_packets;
            printf("packet_queue_get() audio    packets = %d\n", q->nb_packets);
        } else if (pkt->stream_index == video_state->video_stream
                   && video_packets != q->nb_packets
                   && q->nb_packets % 100 == 0) {
            video_packets = q->nb_packets;
            printf("packet_queue_get() video    packets = %d\n", q->nb_packets);
        } else if (pkt->stream_index == video_state->subtitle_stream
                   && subtitle_packets != q->nb_packets
                   && q->nb_packets % 100 == 0) {
            subtitle_packets = q->nb_packets;
            printf("packet_queue_get() subtitle packets = %d\n", q->nb_packets);
        }
    }

    pthread_mutex_unlock(&q->pmutex);
    return ret;
}

static void decoder_init(Decoder *d, AVCodecContext *avctx, PacketQueue *queue, pthread_cond_t *empty_queue_cond) {
    memset(d, 0, sizeof(Decoder));
    d->avctx = avctx;
    d->queue = queue;
    d->pempty_queue_cond = empty_queue_cond;
    d->start_pts = AV_NOPTS_VALUE;
    d->pkt_serial = -1;
}

// 解码
static int decoder_decode_frame(Decoder *d, AVFrame *frame, AVSubtitle *sub) {
    int ret = AVERROR(EAGAIN);

    for (;;) {
        AVPacket pkt;

        if (d->queue->serial == d->pkt_serial) {
            do {
                if (d->queue->abort_request)
                    return -1;

                switch (d->avctx->codec_type) {
                    case AVMEDIA_TYPE_VIDEO:
                        ret = avcodec_receive_frame(d->avctx, frame);
                        if (ret >= 0) {
                            if (decoder_reorder_pts == -1) {
                                frame->pts = frame->best_effort_timestamp;
                            } else if (!decoder_reorder_pts) {
                                frame->pts = frame->pkt_dts;
                            }
                        }
                        break;
                    case AVMEDIA_TYPE_AUDIO:
                        ret = avcodec_receive_frame(d->avctx, frame);
                        if (ret >= 0) {
                            AVRational tb = (AVRational) {1, frame->sample_rate};
                            if (frame->pts != AV_NOPTS_VALUE)
                                frame->pts = av_rescale_q(frame->pts, d->avctx->pkt_timebase, tb);
                            else if (d->next_pts != AV_NOPTS_VALUE)
                                frame->pts = av_rescale_q(d->next_pts, d->next_pts_tb, tb);
                            if (frame->pts != AV_NOPTS_VALUE) {
                                d->next_pts = frame->pts + frame->nb_samples;
                                d->next_pts_tb = tb;
                            }
                        }
                        break;
                }
                if (ret == AVERROR_EOF) {
                    d->finished = d->pkt_serial;
                    avcodec_flush_buffers(d->avctx);
                    return 0;
                }
                if (ret >= 0)
                    return 1;
            } while (ret != AVERROR(EAGAIN));
        }

        do {
            if (d->queue->nb_packets == 0)
                pthread_cond_signal(d->pempty_queue_cond);
            if (d->packet_pending) {
                av_packet_move_ref(&pkt, &d->pkt);
                d->packet_pending = 0;
            } else {
                if (packet_queue_get(d->queue, &pkt, 1, &d->pkt_serial) < 0)
                    return -1;
            }
            if (d->queue->serial == d->pkt_serial)
                break;
            av_packet_unref(&pkt);
        } while (1);

        if (pkt.data == flush_pkt.data) {
            avcodec_flush_buffers(d->avctx);
            d->finished = 0;
            d->next_pts = d->start_pts;
            d->next_pts_tb = d->start_pts_tb;
        } else {
            if (d->avctx->codec_type == AVMEDIA_TYPE_SUBTITLE) {
                int got_frame = 0;
                ret = avcodec_decode_subtitle2(d->avctx, sub, &got_frame, &pkt);
                if (ret < 0) {
                    ret = AVERROR(EAGAIN);
                } else {
                    if (got_frame && !pkt.data) {
                        d->packet_pending = 1;
                        av_packet_move_ref(&d->pkt, &pkt);
                    }
                    ret = got_frame ? 0 : (pkt.data ? AVERROR(EAGAIN) : AVERROR_EOF);
                }
            } else {
                if (avcodec_send_packet(d->avctx, &pkt) == AVERROR(EAGAIN)) {
                    printf("decoder_decode_frame() "
                           "Receive_frame and send_packet both returned EAGAIN, which is an API violation.\n");
                    d->packet_pending = 1;
                    av_packet_move_ref(&d->pkt, &pkt);
                }
            }
            av_packet_unref(&pkt);
        }
    }// for (;;) end
}

static void decoder_destroy(Decoder *d) {
    av_packet_unref(&d->pkt);
    avcodec_free_context(&d->avctx);
}

static void frame_queue_unref_item(Frame *vp) {
    av_frame_unref(vp->frame);
    avsubtitle_free(&vp->sub);
}

static int frame_queue_init(FrameQueue *f, PacketQueue *pktq, int max_size, int keep_last) {
    memset(f, 0, sizeof(FrameQueue));
    f->pmutex = PTHREAD_MUTEX_INITIALIZER;
    f->pcond = PTHREAD_COND_INITIALIZER;
    f->pktq = pktq;
    f->max_size = FFMIN(max_size, FRAME_QUEUE_SIZE);
    f->keep_last = !!keep_last;
    printf("frame_queue_init()  max_size = %d\n", f->max_size);
    printf("frame_queue_init() keep_last = %d\n", f->keep_last);
    for (int i = 0; i < f->max_size; i++)
        if (!(f->queue[i].frame = av_frame_alloc()))
            return AVERROR(ENOMEM);// -12
    return 0;
}

static void frame_queue_destory(FrameQueue *f) {
    int i;
    for (i = 0; i < f->max_size; i++) {
        Frame *vp = &f->queue[i];
        frame_queue_unref_item(vp);
        av_frame_free(&vp->frame);
    }
    pthread_mutex_destroy(&f->pmutex);
    pthread_cond_destroy(&f->pcond);
}

static void frame_queue_signal(FrameQueue *f) {
    pthread_mutex_lock(&f->pmutex);
    pthread_cond_signal(&f->pcond);
    pthread_mutex_unlock(&f->pmutex);
}

static Frame *frame_queue_peek(FrameQueue *f) {
    return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}

static Frame *frame_queue_peek_next(FrameQueue *f) {
    return &f->queue[(f->rindex + f->rindex_shown + 1) % f->max_size];
}

static Frame *frame_queue_peek_last(FrameQueue *f) {
    return &f->queue[f->rindex];
}

static Frame *frame_queue_peek_writable(FrameQueue *f) {
    /* wait until we have space to put a new frame */
    pthread_mutex_lock(&f->pmutex);
    while (f->size >= f->max_size &&
           !f->pktq->abort_request) {
        pthread_cond_wait(&f->pcond, &f->pmutex);
    }
    pthread_mutex_unlock(&f->pmutex);

    if (f->pktq->abort_request)
        return nullptr;

    return &f->queue[f->windex];
}

static Frame *frame_queue_peek_readable(FrameQueue *f) {
    /* wait until we have a readable a new frame */
    pthread_mutex_lock(&f->pmutex);
    while (f->size - f->rindex_shown <= 0 &&
           !f->pktq->abort_request) {
        pthread_cond_wait(&f->pcond, &f->pmutex);
    }
    pthread_mutex_unlock(&f->pmutex);

    if (f->pktq->abort_request)
        return nullptr;

    return &f->queue[(f->rindex + f->rindex_shown) % f->max_size];
}

static void frame_queue_push(FrameQueue *f) {
    if (++f->windex == f->max_size)
        f->windex = 0;
    pthread_mutex_lock(&f->pmutex);
    f->size++;
    pthread_cond_signal(&f->pcond);
    pthread_mutex_unlock(&f->pmutex);
}

static void frame_queue_next(FrameQueue *f) {
    if (f->keep_last && !f->rindex_shown) {
        f->rindex_shown = 1;
        return;
    }
    frame_queue_unref_item(&f->queue[f->rindex]);
    if (++f->rindex == f->max_size)
        f->rindex = 0;
    pthread_mutex_lock(&f->pmutex);
    f->size--;
    pthread_cond_signal(&f->pcond);
    pthread_mutex_unlock(&f->pmutex);
}

/* return the number of undisplayed frames in the queue */
static int frame_queue_nb_remaining(FrameQueue *f) {
    return f->size - f->rindex_shown;
}

/* return last shown position */
static int64_t frame_queue_last_pos(FrameQueue *f) {
    Frame *fp = &f->queue[f->rindex];
    if (f->rindex_shown && fp->serial == f->pktq->serial)
        return fp->pos;
    else
        return -1;
}

static void decoder_abort(Decoder *d, FrameQueue *fq) {
    packet_queue_abort(d->queue);
    frame_queue_signal(fq);
    SDL_WaitThread(d->decoder_tid, nullptr);
    d->decoder_tid = nullptr;
    packet_queue_flush(d->queue);
}

static inline void fill_rectangle(int x, int y, int w, int h) {
    SDL_Rect rect;
    rect.x = x;
    rect.y = y;
    rect.w = w;
    rect.h = h;
    if (w && h)
        SDL_RenderFillRect(renderer, &rect);
}

static int
realloc_texture(SDL_Texture **texture, Uint32 new_format, int new_width, int new_height, SDL_BlendMode blendmode,
                int init_texture) {
    Uint32 format;
    int access, w, h;
    if (!*texture || SDL_QueryTexture(*texture, &format, &access, &w, &h) < 0 || new_width != w || new_height != h ||
        new_format != format) {
        void *pixels;
        int pitch;
        if (*texture)
            SDL_DestroyTexture(*texture);
        if (!(*texture = SDL_CreateTexture(renderer, new_format, SDL_TEXTUREACCESS_STREAMING, new_width, new_height)))
            return -1;
        if (SDL_SetTextureBlendMode(*texture, blendmode) < 0)
            return -1;
        if (init_texture) {
            if (SDL_LockTexture(*texture, nullptr, &pixels, &pitch) < 0)
                return -1;
            memset(pixels, 0, pitch * new_height);
            SDL_UnlockTexture(*texture);
        }
        av_log(nullptr, AV_LOG_VERBOSE, "Created %dx%d texture with %s.\n", new_width, new_height,
               SDL_GetPixelFormatName(new_format));
    }
    return 0;
}

static void calculate_display_rect(SDL_Rect *rect,
                                   int scr_xleft, int scr_ytop, int scr_width, int scr_height,
                                   int pic_width, int pic_height, AVRational pic_sar) {
    AVRational aspect_ratio = pic_sar;
    int64_t width, height, x, y;

    if (av_cmp_q(aspect_ratio, av_make_q(0, 1)) <= 0) {
        aspect_ratio = av_make_q(1, 1);
    }

    aspect_ratio = av_mul_q(aspect_ratio, av_make_q(pic_width, pic_height));

    /* XXX: we suppose the screen has a 1.0 pixel ratio */
    height = scr_height;
    width = av_rescale(height, aspect_ratio.num, aspect_ratio.den) & ~1;
    if (width > scr_width) {
        width = scr_width;
        height = av_rescale(width, aspect_ratio.den, aspect_ratio.num) & ~1;
    }
    x = (scr_width - width) / 2;
    y = (scr_height - height) / 2;
    rect->x = scr_xleft + x;
    rect->y = scr_ytop + y;
    rect->w = FFMAX((int) width, 1);
    rect->h = FFMAX((int) height, 1);
}

static void get_sdl_pix_fmt_and_blendmode(int format, Uint32 *sdl_pix_fmt, SDL_BlendMode *sdl_blendmode) {
    int i;
    *sdl_blendmode = SDL_BLENDMODE_NONE;
    *sdl_pix_fmt = SDL_PIXELFORMAT_UNKNOWN;
    if (format == AV_PIX_FMT_RGB32 ||
        format == AV_PIX_FMT_RGB32_1 ||
        format == AV_PIX_FMT_BGR32 ||
        format == AV_PIX_FMT_BGR32_1)
        *sdl_blendmode = SDL_BLENDMODE_BLEND;
    for (i = 0; i < FF_ARRAY_ELEMS(sdl_texture_format_map) - 1; i++) {
        if (format == sdl_texture_format_map[i].format) {
            *sdl_pix_fmt = sdl_texture_format_map[i].texture_fmt;
            return;
        }
    }
}

// 渲染
static int upload_texture(SDL_Texture **tex, AVFrame *frame, struct SwsContext **img_convert_ctx) {
    int ret = 0;
    Uint32 sdl_pix_fmt;
    SDL_BlendMode sdl_blendmode;
    get_sdl_pix_fmt_and_blendmode(frame->format, &sdl_pix_fmt, &sdl_blendmode);
    if (realloc_texture(tex,
                        sdl_pix_fmt == SDL_PIXELFORMAT_UNKNOWN ? SDL_PIXELFORMAT_ARGB8888 : sdl_pix_fmt,
                        frame->width, frame->height, sdl_blendmode, 0) < 0) {
        return -1;
    }
    switch (sdl_pix_fmt) {
        case SDL_PIXELFORMAT_UNKNOWN:
            /* This should only happen if we are not using avfilter... */
            *img_convert_ctx = sws_getCachedContext(*img_convert_ctx,
                                                    frame->width, frame->height,
                                                    static_cast<AVPixelFormat>(frame->format), frame->width,
                                                    frame->height,
                                                    AV_PIX_FMT_BGRA, sws_flags, nullptr, nullptr, nullptr);
            if (*img_convert_ctx != nullptr) {
                uint8_t *pixels[4];
                int pitch[4];
                if (!SDL_LockTexture(*tex, nullptr, (void **) pixels, pitch)) {
                    sws_scale(*img_convert_ctx, (const uint8_t *const *) frame->data, frame->linesize,
                              0, frame->height, pixels, pitch);
                    SDL_UnlockTexture(*tex);
                }
            } else {
                av_log(nullptr, AV_LOG_FATAL, "Cannot initialize the conversion context\n");
                ret = -1;
            }
            break;
        case SDL_PIXELFORMAT_IYUV:
            if (frame->linesize[0] > 0 && frame->linesize[1] > 0 && frame->linesize[2] > 0) {
                ret = SDL_UpdateYUVTexture(*tex, nullptr, frame->data[0], frame->linesize[0],
                                           frame->data[1], frame->linesize[1],
                                           frame->data[2], frame->linesize[2]);
            } else if (frame->linesize[0] < 0 && frame->linesize[1] < 0 && frame->linesize[2] < 0) {
                ret = SDL_UpdateYUVTexture(*tex, nullptr, frame->data[0] + frame->linesize[0] * (frame->height - 1),
                                           -frame->linesize[0],
                                           frame->data[1] + frame->linesize[1] * (AV_CEIL_RSHIFT(frame->height, 1) - 1),
                                           -frame->linesize[1],
                                           frame->data[2] + frame->linesize[2] * (AV_CEIL_RSHIFT(frame->height, 1) - 1),
                                           -frame->linesize[2]);
            } else {
                av_log(nullptr, AV_LOG_ERROR, "Mixed negative and positive linesizes are not supported.\n");
                return -1;
            }
            break;
        default:
            if (frame->linesize[0] < 0) {
                ret = SDL_UpdateTexture(*tex, nullptr, frame->data[0] + frame->linesize[0] * (frame->height - 1),
                                        -frame->linesize[0]);
            } else {
                ret = SDL_UpdateTexture(*tex, nullptr, frame->data[0], frame->linesize[0]);
            }
            break;
    }
    return ret;
}

static void set_sdl_yuv_conversion_mode(AVFrame *frame) {
#if SDL_VERSION_ATLEAST(2, 0, 8)
    SDL_YUV_CONVERSION_MODE mode = SDL_YUV_CONVERSION_AUTOMATIC;
    if (frame && (frame->format == AV_PIX_FMT_YUV420P || frame->format == AV_PIX_FMT_YUYV422 ||
                  frame->format == AV_PIX_FMT_UYVY422)) {
        if (frame->color_range == AVCOL_RANGE_JPEG)
            mode = SDL_YUV_CONVERSION_JPEG;
        else if (frame->colorspace == AVCOL_SPC_BT709)
            mode = SDL_YUV_CONVERSION_BT709;
        else if (frame->colorspace == AVCOL_SPC_BT470BG || frame->colorspace == AVCOL_SPC_SMPTE170M ||
                 frame->colorspace == AVCOL_SPC_SMPTE240M)
            mode = SDL_YUV_CONVERSION_BT601;
    }
    SDL_SetYUVConversionMode(mode);
#endif
}

static void video_image_display(VideoState *is) {
    Frame *vp = nullptr;
    Frame *sp = nullptr;
    SDL_Rect rect;
    // 取要显示的视频帧
    vp = frame_queue_peek_last(&is->pictq);

    // region is->subtitle_st
    if (is->subtitle_st) {
        if (frame_queue_nb_remaining(&is->subpq) > 0) {
            sp = frame_queue_peek(&is->subpq);

            if (vp->pts >= sp->pts + ((float) sp->sub.start_display_time / 1000)) {
                if (!sp->uploaded) {
                    uint8_t *pixels[4];
                    int pitch[4];
                    int i;
                    if (!sp->width || !sp->height) {
                        sp->width = vp->width;
                        sp->height = vp->height;
                    }
                    if (realloc_texture(&is->sub_texture, SDL_PIXELFORMAT_ARGB8888, sp->width, sp->height,
                                        SDL_BLENDMODE_BLEND, 1) < 0)
                        return;

                    for (i = 0; i < sp->sub.num_rects; i++) {
                        AVSubtitleRect *sub_rect = sp->sub.rects[i];

                        sub_rect->x = av_clip(sub_rect->x, 0, sp->width);
                        sub_rect->y = av_clip(sub_rect->y, 0, sp->height);
                        sub_rect->w = av_clip(sub_rect->w, 0, sp->width - sub_rect->x);
                        sub_rect->h = av_clip(sub_rect->h, 0, sp->height - sub_rect->y);

                        is->sub_convert_ctx = sws_getCachedContext(is->sub_convert_ctx,
                                                                   sub_rect->w, sub_rect->h, AV_PIX_FMT_PAL8,
                                                                   sub_rect->w, sub_rect->h, AV_PIX_FMT_BGRA,
                                                                   0, nullptr, nullptr, nullptr);
                        if (!is->sub_convert_ctx) {
                            av_log(nullptr, AV_LOG_FATAL, "Cannot initialize the conversion context\n");
                            return;
                        }
                        if (!SDL_LockTexture(is->sub_texture, (SDL_Rect *) sub_rect, (void **) pixels, pitch)) {
                            sws_scale(is->sub_convert_ctx, (const uint8_t *const *) sub_rect->data, sub_rect->linesize,
                                      0, sub_rect->h, pixels, pitch);
                            SDL_UnlockTexture(is->sub_texture);
                        }
                    }
                    sp->uploaded = 1;
                }
            } else
                sp = nullptr;
        }
    }
    // endregion

    calculate_display_rect(&rect, is->xleft, is->ytop, is->width, is->height, vp->width, vp->height, vp->sar);
    // 如果是重复显示上一帧，那么uploaded就是1
    if (!vp->uploaded) {
        // 渲染
        if (upload_texture(&is->vid_texture, vp->frame, &is->img_convert_ctx) < 0) {
            return;
        }
        vp->uploaded = 1;
        vp->flip_v = vp->frame->linesize[0] < 0;
    }

    // region SDL_RenderCopyEx
    set_sdl_yuv_conversion_mode(vp->frame);
    SDL_RenderCopyEx(renderer, is->vid_texture, nullptr, &rect, 0, nullptr,
                     static_cast<const SDL_RendererFlip>(vp->flip_v ? SDL_FLIP_VERTICAL : 0));
    set_sdl_yuv_conversion_mode(nullptr);
    // endregion

    // region sp is null
    if (sp) {
#if USE_ONEPASS_SUBTITLE_RENDER
        SDL_RenderCopy(renderer, is->sub_texture, nullptr, &rect);
#else
        int i;
        double xratio = (double)rect.w / (double)sp->width;
        double yratio = (double)rect.h / (double)sp->height;
        for (i = 0; i < sp->sub.num_rects; i++) {
            SDL_Rect *sub_rect = (SDL_Rect*)sp->sub.rects[i];
            SDL_Rect target = {.x = rect.x + sub_rect->x * xratio,
                               .y = rect.y + sub_rect->y * yratio,
                               .w = sub_rect->w * xratio,
                               .h = sub_rect->h * yratio};
            SDL_RenderCopy(renderer, is->sub_texture, sub_rect, &target);
        }
#endif
    }
    // endregion
}

static inline int compute_mod(int a, int b) {
    return a < 0 ? a % b + b : a % b;
}

static void video_audio_display(VideoState *s) {
    int i, i_start, x, y1, y, ys, delay, n, nb_display_channels;
    int ch, channels, h, h2;
    int64_t time_diff;
    int rdft_bits, nb_freq;

    for (rdft_bits = 1; (1 << rdft_bits) < 2 * s->height; rdft_bits++);
    nb_freq = 1 << (rdft_bits - 1);

    /* compute display index : center on currently output samples */
    channels = s->audio_tgt.channels;
    nb_display_channels = channels;
    if (!s->paused) {
        int data_used = s->show_mode == VideoState::SHOW_MODE_WAVES ? s->width : (2 * nb_freq);
        n = 2 * channels;
        delay = s->audio_write_buf_size;
        delay /= n;

        /* to be more precise, we take into account the time spent since
           the last buffer computation */
        if (audio_callback_time) {
            time_diff = av_gettime_relative() - audio_callback_time;
            delay -= (time_diff * s->audio_tgt.freq) / 1000000;
        }

        delay += 2 * data_used;
        if (delay < data_used)
            delay = data_used;

        i_start = x = compute_mod(s->sample_array_index - delay * channels, SAMPLE_ARRAY_SIZE);
        if (s->show_mode == VideoState::SHOW_MODE_WAVES) {
            h = INT_MIN;
            for (i = 0; i < 1000; i += channels) {
                int idx = (SAMPLE_ARRAY_SIZE + x - i) % SAMPLE_ARRAY_SIZE;
                int a = s->sample_array[idx];
                int b = s->sample_array[(idx + 4 * channels) % SAMPLE_ARRAY_SIZE];
                int c = s->sample_array[(idx + 5 * channels) % SAMPLE_ARRAY_SIZE];
                int d = s->sample_array[(idx + 9 * channels) % SAMPLE_ARRAY_SIZE];
                int score = a - d;
                if (h < score && (b ^ c) < 0) {
                    h = score;
                    i_start = idx;
                }
            }
        }

        s->last_i_start = i_start;
    } else {
        i_start = s->last_i_start;
    }

    if (s->show_mode == VideoState::SHOW_MODE_WAVES) {
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);

        /* total height for one channel */
        h = s->height / nb_display_channels;
        /* graph height / 2 */
        h2 = (h * 9) / 20;
        for (ch = 0; ch < nb_display_channels; ch++) {
            i = i_start + ch;
            y1 = s->ytop + ch * h + (h / 2); /* position of center line */
            for (x = 0; x < s->width; x++) {
                y = (s->sample_array[i] * h2) >> 15;
                if (y < 0) {
                    y = -y;
                    ys = y1 - y;
                } else {
                    ys = y1;
                }
                fill_rectangle(s->xleft + x, ys, 1, y);
                i += channels;
                if (i >= SAMPLE_ARRAY_SIZE)
                    i -= SAMPLE_ARRAY_SIZE;
            }
        }

        SDL_SetRenderDrawColor(renderer, 0, 0, 255, 255);

        for (ch = 1; ch < nb_display_channels; ch++) {
            y = s->ytop + ch * h;
            fill_rectangle(s->xleft, y, s->width, 1);
        }
    } else {
        if (realloc_texture(&s->vis_texture, SDL_PIXELFORMAT_ARGB8888, s->width, s->height, SDL_BLENDMODE_NONE, 1) < 0)
            return;

        nb_display_channels = FFMIN(nb_display_channels, 2);
        if (rdft_bits != s->rdft_bits) {
            av_rdft_end(s->rdft);
            av_free(s->rdft_data);
            s->rdft = av_rdft_init(rdft_bits, DFT_R2C);
            s->rdft_bits = rdft_bits;
            s->rdft_data = static_cast<FFTSample *>(av_malloc_array(nb_freq, 4 * sizeof(*s->rdft_data)));
        }
        if (!s->rdft || !s->rdft_data) {
            av_log(nullptr, AV_LOG_ERROR, "Failed to allocate buffers for RDFT, switching to waves display\n");
            s->show_mode = VideoState::SHOW_MODE_WAVES;
        } else {
            FFTSample *data[2];
            SDL_Rect rect = {.x = s->xpos, .y = 0, .w = 1, .h = s->height};
            uint32_t *pixels;
            int pitch;
            for (ch = 0; ch < nb_display_channels; ch++) {
                data[ch] = s->rdft_data + 2 * nb_freq * ch;
                i = i_start + ch;
                for (x = 0; x < 2 * nb_freq; x++) {
                    double w = (x - nb_freq) * (1.0 / nb_freq);
                    data[ch][x] = s->sample_array[i] * (1.0 - w * w);
                    i += channels;
                    if (i >= SAMPLE_ARRAY_SIZE)
                        i -= SAMPLE_ARRAY_SIZE;
                }
                av_rdft_calc(s->rdft, data[ch]);
            }
            /* Least efficient way to do this, we should of course
             * directly access it but it is more than fast enough. */
            if (!SDL_LockTexture(s->vis_texture, &rect, (void **) &pixels, &pitch)) {
                pitch >>= 2;
                pixels += pitch * s->height;
                for (y = 0; y < s->height; y++) {
                    double w = 1 / sqrt(nb_freq);
                    int a = sqrt(w * sqrt(data[0][2 * y + 0] * data[0][2 * y + 0] +
                                          data[0][2 * y + 1] * data[0][2 * y + 1]));
                    int b = (nb_display_channels == 2)
                            ? sqrt(w * hypot(data[1][2 * y + 0], data[1][2 * y + 1]))
                            : a;
                    a = FFMIN(a, 255);
                    b = FFMIN(b, 255);
                    pixels -= pitch;
                    *pixels = (a << 16) + (b << 8) + ((a + b) >> 1);
                }
                SDL_UnlockTexture(s->vis_texture);
            }
            SDL_RenderCopy(renderer, s->vis_texture, nullptr, nullptr);
        }
        if (!s->paused)
            s->xpos++;
        if (s->xpos >= s->width)
            s->xpos = s->xleft;
    }
}

static void stream_component_close(VideoState *is, int stream_index) {
    AVFormatContext *ic = is->ic;
    AVCodecParameters *codecpar;

    if (stream_index < 0 || stream_index >= ic->nb_streams)
        return;
    codecpar = ic->streams[stream_index]->codecpar;

    switch (codecpar->codec_type) {
        case AVMEDIA_TYPE_AUDIO:
            decoder_abort(&is->auddec, &is->sampq);
            SDL_CloseAudioDevice(audio_dev);
            decoder_destroy(&is->auddec);
            swr_free(&is->swr_ctx);
            av_freep(&is->audio_buf1);
            is->audio_buf1_size = 0;
            is->audio_buf = nullptr;

            if (is->rdft) {
                av_rdft_end(is->rdft);
                av_freep(&is->rdft_data);
                is->rdft = nullptr;
                is->rdft_bits = 0;
            }
            break;
        case AVMEDIA_TYPE_VIDEO:
            decoder_abort(&is->viddec, &is->pictq);
            decoder_destroy(&is->viddec);
            break;
        case AVMEDIA_TYPE_SUBTITLE:
            decoder_abort(&is->subdec, &is->subpq);
            decoder_destroy(&is->subdec);
            break;
        default:
            break;
    }

    ic->streams[stream_index]->discard = AVDISCARD_ALL;
    switch (codecpar->codec_type) {
        case AVMEDIA_TYPE_AUDIO:
            is->audio_st = nullptr;
            is->audio_stream = -1;
            break;
        case AVMEDIA_TYPE_VIDEO:
            is->video_st = nullptr;
            is->video_stream = -1;
            break;
        case AVMEDIA_TYPE_SUBTITLE:
            is->subtitle_st = nullptr;
            is->subtitle_stream = -1;
            break;
        default:
            break;
    }
}

static void stream_close(VideoState *is) {
    printf("stream_close() start\n");
    /* XXX: use a special url_shutdown call to abort parse cleanly */
    is->abort_request = 1;
    SDL_WaitThread(is->read_tid, nullptr);

    /* close each stream */
    if (is->video_stream >= 0) {
        stream_component_close(is, is->video_stream);
    }
    if (is->audio_stream >= 0) {
        stream_component_close(is, is->audio_stream);
    }
    if (is->subtitle_stream >= 0) {
        stream_component_close(is, is->subtitle_stream);
    }

    if (is->ic) {
        avformat_close_input(&is->ic);
        is->ic = nullptr;
    }

    packet_queue_destroy(&is->videoq);
    packet_queue_destroy(&is->audioq);
    packet_queue_destroy(&is->subtitleq);

    /* free all pictures */
    frame_queue_destory(&is->pictq);
    frame_queue_destory(&is->sampq);
    frame_queue_destory(&is->subpq);
    pthread_cond_destroy(&is->pcontinue_read_thread);
    sws_freeContext(is->img_convert_ctx);
    sws_freeContext(is->sub_convert_ctx);
    av_free(is->filename);
    if (is->vis_texture) {
        SDL_DestroyTexture(is->vis_texture);
    }
    if (is->vid_texture) {
        SDL_DestroyTexture(is->vid_texture);
    }
    if (is->sub_texture) {
        SDL_DestroyTexture(is->sub_texture);
    }
    if (is) {
        av_free(is);
        is = nullptr;
    }
    video_state = nullptr;
    printf("stream_close() end\n");
}

static void do_exit(VideoState *is) {
    printf("do_exit() start\n");
    if (is) {
        stream_close(is);
    }
    if (renderer)
        SDL_DestroyRenderer(renderer);
    if (window)
        SDL_DestroyWindow(window);
    uninit_opts();
#if CONFIG_AVFILTER
    av_freep(&vfilters_list);
#endif
    avformat_network_deinit();
    if (show_status)
        printf("\n");
    SDL_Quit();
    av_log(nullptr, AV_LOG_QUIET, "%s", "");
    printf("do_exit() end\n");
    exit(0);
}

static void sigterm_handler(int sig) {
    exit(123);
}

static void set_default_window_size(int width, int height, AVRational sar) {
    int max_width = screen_width ? screen_width : INT_MAX;
    int max_height = screen_height ? screen_height : INT_MAX;
    if (max_width == INT_MAX && max_height == INT_MAX) {
        max_height = height;
    }
    SDL_Rect rect;
    calculate_display_rect(&rect, 0, 0, max_width, max_height, width, height, sar);
    default_width = rect.w;
    default_height = rect.h;
}

static int video_open(VideoState *is) {
    int w, h;
    w = screen_width ? screen_width : default_width;
    h = screen_height ? screen_height : default_height;
    is->width = w;
    is->height = h;
    if (!window_title) {
        window_title = input_filename;
    }
    SDL_SetWindowTitle(window, window_title);
    SDL_SetWindowSize(window, w, h);
    SDL_SetWindowPosition(window, screen_left, screen_top);
    if (is_full_screen) {
        SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
    }
    SDL_ShowWindow(window);
    return 0;
}

/* display the current picture, if any */
static void video_display(VideoState *is) {
    if (!is->width) {
        video_open(is);
    }

    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    if (is->show_mode != VideoState::SHOW_MODE_VIDEO && is->audio_st) {
        // 图形化显示仅有音频的音频帧
        video_audio_display(is);
    } else if (is->video_st) {
        // 图形化显示一帧视频画面
        video_image_display(is);
    }
    SDL_RenderPresent(renderer);
}

static double get_clock(Clock *c) {
    if (*c->queue_serial != c->serial)
        return NAN;
    if (c->paused) {
        return c->pts;
    } else {
        double time = av_gettime_relative() / 1000000.0;
        return c->pts_drift + time - (time - c->last_updated) * (1.0 - c->speed);
    }
}

static void set_clock_at(Clock *c, double pts, int serial, double time) {
    c->pts = pts;
    c->last_updated = time;
    c->pts_drift = c->pts - time;
    c->serial = serial;
}

static void set_clock(Clock *c, double pts, int serial) {
    //printf("set_clock() pts = %lf\n", pts);
    double time = av_gettime_relative() / 1000000.0;
    set_clock_at(c, pts, serial, time);
}

static void set_clock_speed(Clock *c, double speed) {
    set_clock(c, get_clock(c), c->serial);
    c->speed = speed;
}

static void init_clock(Clock *c, int *queue_serial) {
    printf("init_clock() queue_serial = %d\n", *queue_serial);
    c->speed = 1.0;
    c->paused = 0;
    c->queue_serial = queue_serial;
    set_clock(c, NAN, -1);// NAN = nan
}

static void sync_clock_to_slave(Clock *c, Clock *slave) {
    double clock = get_clock(c);
    double slave_clock = get_clock(slave);
    if (!isnan(slave_clock) && (isnan(clock) || fabs(clock - slave_clock) > AV_NOSYNC_THRESHOLD))
        set_clock(c, slave_clock, slave->serial);
}

static int get_master_sync_type(VideoState *is) {
    if (is->av_sync_type == AV_SYNC_VIDEO_MASTER) {
        if (is->video_st)
            return AV_SYNC_VIDEO_MASTER;
        else
            return AV_SYNC_AUDIO_MASTER;
    } else if (is->av_sync_type == AV_SYNC_AUDIO_MASTER) {
        if (is->audio_st)
            return AV_SYNC_AUDIO_MASTER;
        else
            return AV_SYNC_EXTERNAL_CLOCK;
    } else {
        return AV_SYNC_EXTERNAL_CLOCK;
    }
}

/* get the current master clock value */
static double get_master_clock(VideoState *is) {
    double val;

    switch (get_master_sync_type(is)) {
        case AV_SYNC_VIDEO_MASTER:
            val = get_clock(&is->vidclk);
            break;
        case AV_SYNC_AUDIO_MASTER:
            val = get_clock(&is->audclk);
            break;
        default:
            val = get_clock(&is->extclk);
            break;
    }
    return val;
}

static void check_external_clock_speed(VideoState *is) {
    if (is->video_stream >= 0 && is->videoq.nb_packets <= EXTERNAL_CLOCK_MIN_FRAMES ||
        is->audio_stream >= 0 && is->audioq.nb_packets <= EXTERNAL_CLOCK_MIN_FRAMES) {
        set_clock_speed(&is->extclk, FFMAX(EXTERNAL_CLOCK_SPEED_MIN, is->extclk.speed - EXTERNAL_CLOCK_SPEED_STEP));
    } else if ((is->video_stream < 0 || is->videoq.nb_packets > EXTERNAL_CLOCK_MAX_FRAMES)
               && (is->audio_stream < 0 || is->audioq.nb_packets > EXTERNAL_CLOCK_MAX_FRAMES)) {
        set_clock_speed(&is->extclk, FFMIN(EXTERNAL_CLOCK_SPEED_MAX, is->extclk.speed + EXTERNAL_CLOCK_SPEED_STEP));
    } else {
        double speed = is->extclk.speed;
        if (speed != 1.0) {
            set_clock_speed(&is->extclk, speed + EXTERNAL_CLOCK_SPEED_STEP * (1.0 - speed) / fabs(1.0 - speed));
        }
    }
}

/* seek in the stream */
static void stream_seek(VideoState *is, int64_t pos, int64_t rel, int seek_by_bytes) {
    printf("stream_seek() pos = %ld rel = %ld seek_by_bytes = %d\n", (long) pos, (long) rel, seek_by_bytes);
    if (!is->seek_req) {
        is->seek_req = 1;
        is->seek_pos = pos;
        is->seek_rel = rel;
        is->seek_flags &= ~AVSEEK_FLAG_BYTE;
        if (seek_by_bytes)
            is->seek_flags |= AVSEEK_FLAG_BYTE;
        pthread_cond_signal(&is->pcontinue_read_thread);
    }
}

/* pause or resume the video */
static void stream_toggle_pause(VideoState *is) {
    printf("stream_toggle_pause() before is->paused = %d\n", is->paused);
    if (is->paused) {
        is->frame_timer += av_gettime_relative() / 1000000.0 - is->vidclk.last_updated;
        if (is->read_pause_return != AVERROR(ENOSYS)) {
            is->vidclk.paused = 0;
        }
        set_clock(&is->vidclk, get_clock(&is->vidclk), is->vidclk.serial);
    }
    set_clock(&is->extclk, get_clock(&is->extclk), is->extclk.serial);
    is->paused = is->audclk.paused = is->vidclk.paused = is->extclk.paused = !is->paused;
    printf("stream_toggle_pause() after  is->paused = %d\n", is->paused);
}

static void toggle_pause(VideoState *is) {
    stream_toggle_pause(is);
    is->step = 0;
}

static void toggle_mute(VideoState *is) {
    is->muted = !is->muted;
}

static void update_volume(VideoState *is, int sign, double step) {
    double volume_level = is->audio_volume ? (20 * log(is->audio_volume / (double) SDL_MIX_MAXVOLUME) / log(10))
                                           : -1000.0;
    int new_volume = lrint(SDL_MIX_MAXVOLUME * pow(10.0, (volume_level + sign * step) / 20.0));
    is->audio_volume = av_clip(is->audio_volume == new_volume ? (is->audio_volume + sign) : new_volume, 0,
                               SDL_MIX_MAXVOLUME);
}

static void step_to_next_frame(VideoState *is) {
    /* if the stream is paused unpause it, then step */
    if (is->paused)
        stream_toggle_pause(is);
    is->step = 1;
}

static double compute_target_delay(double delay, VideoState *is) {
    double sync_threshold, diff = 0;

    /* update delay to follow master synchronisation source */
    if (get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER) {
        /* if video is slave, we try to correct big delays by
           duplicating or deleting a frame */
        diff = get_clock(&is->vidclk) - get_master_clock(is);

        /* skip or repeat frame. We take into account the
           delay to compute the threshold. I still don't know
           if it is the best guess */
        sync_threshold = FFMAX(AV_SYNC_THRESHOLD_MIN, FFMIN(AV_SYNC_THRESHOLD_MAX, delay));
        if (!isnan(diff) && fabs(diff) < is->max_frame_duration) {
            if (diff <= -sync_threshold)
                delay = FFMAX(0, delay + diff);
            else if (diff >= sync_threshold && delay > AV_SYNC_FRAMEDUP_THRESHOLD)
                delay = delay + diff;
            else if (diff >= sync_threshold)
                delay = 2 * delay;
        }
    }

    av_log(nullptr, AV_LOG_TRACE, "video: delay=%0.3f A-V=%f\n",
           delay, -diff);

    return delay;
}

static double vp_duration(VideoState *is, Frame *vp, Frame *nextvp) {
    if (vp->serial == nextvp->serial) {
        double duration = nextvp->pts - vp->pts;
        if (isnan(duration) || duration <= 0 || duration > is->max_frame_duration)
            return vp->duration;
        else
            return duration;
    } else {
        return 0.0;
    }
}

static void update_video_pts(VideoState *is, double pts, int64_t pos, int serial) {
    /* update current video pts */
    set_clock(&is->vidclk, pts, serial);
    sync_clock_to_slave(&is->extclk, &is->vidclk);
}

/* called to display each frame */
static void video_refresh(void *opaque, double *remaining_time) {
    VideoState *is = static_cast<VideoState *>(opaque);
    double time;
    Frame *sp = nullptr, *sp2 = nullptr;

    // region 第一个条件不满足
    if (is->realtime && !is->paused && get_master_sync_type(is) == AV_SYNC_EXTERNAL_CLOCK) {
        check_external_clock_speed(is);
    }
    // endregion

    // region 第一个条件不满足
    if (is->show_mode != VideoState::SHOW_MODE_VIDEO && !display_disable && is->audio_st) {
        time = av_gettime_relative() / 1000000.0;
        if (is->force_refresh || is->last_vis_time + rdftspeed < time) {
            video_display(is);
            is->last_vis_time = time;
        }
        *remaining_time = FFMIN(*remaining_time, is->last_vis_time + rdftspeed - time);
    }
    // endregion

    // region is->video_st
    if (is->video_st) {
        retry:
        if (frame_queue_nb_remaining(&is->pictq) == 0) {
            // nothing to do, no picture to display in the queue
        } else {
            double last_duration, duration, delay;
            // lastvp: 已经显示了的帧(也就是当前屏幕上看到的帧)
            //     vp: 将要显示的目标帧
            // nextvp: 下一次要显示的帧
            Frame *lastvp = nullptr, *vp = nullptr;

            /* dequeue the picture */
            lastvp = frame_queue_peek_last(&is->pictq);
            vp = frame_queue_peek(&is->pictq);

            if (vp->serial != is->videoq.serial) {
                frame_queue_next(&is->pictq);
                goto retry;
            }

            if (vp->serial != lastvp->serial) {
                is->frame_timer = av_gettime_relative() / 1000000.0;
            }

            // 如果是暂停操作,则进行重复播放最后一帧画面
            if (is->paused) {
                goto display;
            }

            /* compute nominal last_duration */
            // 获取当前帧播放需要持续的理论时长
            last_duration = vp_duration(is, lastvp, vp);
            // 获取播放当前帧需要的实际延迟时间
            delay = compute_target_delay(last_duration, is);
            // 获取当前时间
            time = av_gettime_relative() / 1000000.0;
            // 如果当前时间小于需要播放的时间.意味着将要显示的帧还没有到时间播放.故需要继续播放上一帧画面，继续延迟remaining_time时间
            if (time < is->frame_timer + delay) {
                *remaining_time = FFMIN(is->frame_timer + delay - time, *remaining_time);
                goto display;
            }

            is->frame_timer += delay;
            if (delay > 0 && time - is->frame_timer > AV_SYNC_THRESHOLD_MAX) {
                is->frame_timer = time;
            }

            pthread_mutex_lock(&is->pictq.pmutex);
            if (!isnan(vp->pts)) {
                update_video_pts(is, vp->pts, vp->pos, vp->serial);
            }
            pthread_mutex_unlock(&is->pictq.pmutex);
            // 判断是否要丢弃未能播放的视频帧
            if (frame_queue_nb_remaining(&is->pictq) > 1) {
                Frame *nextvp = frame_queue_peek_next(&is->pictq);
                duration = vp_duration(is, vp, nextvp);
                if (!is->step
                    && (framedrop > 0 || (framedrop && get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER))
                    && time > is->frame_timer + duration) {
                    is->frame_drops_late++;
                    frame_queue_next(&is->pictq);
                    goto retry;
                }
            }

            // region is->subtitle_st
            if (is->subtitle_st) {
                while (frame_queue_nb_remaining(&is->subpq) > 0) {
                    sp = frame_queue_peek(&is->subpq);

                    if (frame_queue_nb_remaining(&is->subpq) > 1)
                        sp2 = frame_queue_peek_next(&is->subpq);
                    else
                        sp2 = nullptr;

                    if (sp->serial != is->subtitleq.serial
                        || (is->vidclk.pts > (sp->pts + ((float) sp->sub.end_display_time / 1000)))
                        || (sp2 && is->vidclk.pts > (sp2->pts + ((float) sp2->sub.start_display_time / 1000)))) {
                        if (sp->uploaded) {
                            int i;
                            for (i = 0; i < sp->sub.num_rects; i++) {
                                AVSubtitleRect *sub_rect = sp->sub.rects[i];
                                uint8_t *pixels;
                                int pitch, j;

                                if (!SDL_LockTexture(
                                        is->sub_texture, (SDL_Rect *) sub_rect, (void **) &pixels, &pitch)) {
                                    for (j = 0; j < sub_rect->h; j++, pixels += pitch) {
                                        memset(pixels, 0, sub_rect->w << 2);
                                    }
                                    SDL_UnlockTexture(is->sub_texture);
                                }
                            }
                        }
                        frame_queue_next(&is->subpq);
                    } else {
                        break;
                    }
                }
            }// subtitle
            // endregion

            frame_queue_next(&is->pictq);
            is->force_refresh = 1;

            if (is->step && !is->paused) {
                stream_toggle_pause(is);
            }
        }

        display:
        /* display picture */
        if (!display_disable
            && is->force_refresh
            && is->show_mode == VideoState::SHOW_MODE_VIDEO
            && is->pictq.rindex_shown) {
            video_display(is);
        }
    }
    // endregion

    is->force_refresh = 0;

    // region 只是输出有关视频信息
    /*if (show_status) {// -1
        AVBPrint buf;
        static int64_t last_time;
        int64_t cur_time;
        int aqsize, vqsize, sqsize;
        double av_diff;

        cur_time = av_gettime_relative();
        if (!last_time || (cur_time - last_time) >= 30000) {
            aqsize = 0;
            vqsize = 0;
            sqsize = 0;
            if (is->audio_st) {
                aqsize = is->audioq.size;
            }
            if (is->video_st) {
                vqsize = is->videoq.size;
            }
            if (is->subtitle_st) {
                sqsize = is->subtitleq.size;
            }
            av_diff = 0;
            if (is->audio_st && is->video_st) {
                av_diff = get_clock(&is->audclk) - get_clock(&is->vidclk);
            } else if (is->video_st) {
                av_diff = get_master_clock(is) - get_clock(&is->vidclk);
            } else if (is->audio_st) {
                av_diff = get_master_clock(is) - get_clock(&is->audclk);
            }

            av_bprint_init(&buf, 0, AV_BPRINT_SIZE_AUTOMATIC);
            av_bprintf(&buf,
                       "%7.2f %s:%7.3f fd=%4d aq=%5dKB vq=%5dKB sq=%5dB f=%" PRId64"/%" PRId64"   \r\n",
                       get_master_clock(is),
                       (is->audio_st && is->video_st) ? "A-V" : (is->video_st ? "M-V" : (is->audio_st ? "M-A" : "   ")),
                       av_diff,
                       is->frame_drops_early + is->frame_drops_late,
                       aqsize / 1024,
                       vqsize / 1024,
                       sqsize,
                       is->video_st ? is->viddec.avctx->pts_correction_num_faulty_dts : 0,
                       is->video_st ? is->viddec.avctx->pts_correction_num_faulty_pts : 0);

            if (show_status == 1 && AV_LOG_INFO > av_log_get_level()) {
                fprintf(stderr, "%s\n", buf.str);
            } else {
                av_log(nullptr, AV_LOG_INFO, "%s\n", buf.str);
            }

            fflush(stderr);
            av_bprint_finalize(&buf, nullptr);

            last_time = cur_time;
        }
    }*/
    // endregion
}

static int queue_picture(VideoState *is, AVFrame *src_frame, double pts, double duration, int64_t pos, int serial) {
    Frame *vp;

#if defined(DEBUG_SYNC)
    printf("frame_type=%c pts=%0.3f\n",
           av_get_picture_type_char(src_frame->pict_type), pts);
#endif

    if (!(vp = frame_queue_peek_writable(&is->pictq)))
        return -1;

    vp->sar = src_frame->sample_aspect_ratio;
    vp->uploaded = 0;

    vp->width = src_frame->width;
    vp->height = src_frame->height;
    vp->format = src_frame->format;

    vp->pts = pts;
    vp->duration = duration;
    vp->pos = pos;
    vp->serial = serial;

    set_default_window_size(vp->width, vp->height, vp->sar);

    av_frame_move_ref(vp->frame, src_frame);
    frame_queue_push(&is->pictq);
    return 0;
}

static int get_video_frame(VideoState *is, AVFrame *frame) {
    int got_picture;

    if ((got_picture = decoder_decode_frame(&is->viddec, frame, nullptr)) < 0)
        return -1;

    if (got_picture) {
        double dpts = NAN;

        if (frame->pts != AV_NOPTS_VALUE)
            dpts = av_q2d(is->video_st->time_base) * frame->pts;

        frame->sample_aspect_ratio = av_guess_sample_aspect_ratio(is->ic, is->video_st, frame);

        if (framedrop > 0 || (framedrop && get_master_sync_type(is) != AV_SYNC_VIDEO_MASTER)) {
            if (frame->pts != AV_NOPTS_VALUE) {
                double diff = dpts - get_master_clock(is);
                if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD &&
                    diff - is->frame_last_filter_delay < 0 &&
                    is->viddec.pkt_serial == is->vidclk.serial &&
                    is->videoq.nb_packets) {
                    is->frame_drops_early++;
                    av_frame_unref(frame);
                    got_picture = 0;
                }
            }
        }
    }

    return got_picture;
}

#if CONFIG_AVFILTER

static int configure_filtergraph(AVFilterGraph *graph, const char *filtergraph,
                                 AVFilterContext *source_ctx, AVFilterContext *sink_ctx) {
    int ret, i;
    int nb_filters = graph->nb_filters;
    AVFilterInOut *outputs = nullptr, *inputs = nullptr;

    if (filtergraph) {
        outputs = avfilter_inout_alloc();
        inputs = avfilter_inout_alloc();
        if (!outputs || !inputs) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }

        outputs->name = av_strdup("in");
        outputs->filter_ctx = source_ctx;
        outputs->pad_idx = 0;
        outputs->next = nullptr;

        inputs->name = av_strdup("out");
        inputs->filter_ctx = sink_ctx;
        inputs->pad_idx = 0;
        inputs->next = nullptr;

        if ((ret = avfilter_graph_parse_ptr(graph, filtergraph, &inputs, &outputs, nullptr)) < 0)
            goto fail;
    } else {
        if ((ret = avfilter_link(source_ctx, 0, sink_ctx, 0)) < 0)
            goto fail;
    }

    /* Reorder the filters to ensure that inputs of the custom filters are merged first */
    for (i = 0; i < graph->nb_filters - nb_filters; i++)
        FFSWAP(AVFilterContext*, graph->filters[i], graph->filters[i + nb_filters]);

    ret = avfilter_graph_config(graph, nullptr);
    fail:
    avfilter_inout_free(&outputs);
    avfilter_inout_free(&inputs);
    return ret;
}

static int configure_video_filters(AVFilterGraph *graph, VideoState *is, const char *vfilters, AVFrame *frame) {
    enum AVPixelFormat pix_fmts[FF_ARRAY_ELEMS(sdl_texture_format_map)];
    char sws_flags_str[512] = "";
    char buffersrc_args[256];
    int ret;
    AVFilterContext *filt_src = nullptr, *filt_out = nullptr, *last_filter = nullptr;
    AVCodecParameters *codecpar = is->video_st->codecpar;
    AVRational fr = av_guess_frame_rate(is->ic, is->video_st, nullptr);
    AVDictionaryEntry *e = nullptr;
    int nb_pix_fmts = 0;
    int i, j;

    for (i = 0; i < renderer_info.num_texture_formats; i++) {
        for (j = 0; j < FF_ARRAY_ELEMS(sdl_texture_format_map) - 1; j++) {
            if (renderer_info.texture_formats[i] == sdl_texture_format_map[j].texture_fmt) {
                pix_fmts[nb_pix_fmts++] = sdl_texture_format_map[j].format;
                break;
            }
        }
    }
    pix_fmts[nb_pix_fmts] = AV_PIX_FMT_NONE;

    while ((e = av_dict_get(sws_dict, "", e, AV_DICT_IGNORE_SUFFIX))) {
        if (!strcmp(e->key, "sws_flags")) {
            av_strlcatf(sws_flags_str, sizeof(sws_flags_str), "%s=%s:", "flags", e->value);
        } else
            av_strlcatf(sws_flags_str, sizeof(sws_flags_str), "%s=%s:", e->key, e->value);
    }
    if (strlen(sws_flags_str))
        sws_flags_str[strlen(sws_flags_str) - 1] = '\0';

    graph->scale_sws_opts = av_strdup(sws_flags_str);

    snprintf(buffersrc_args, sizeof(buffersrc_args),
             "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
             frame->width, frame->height, frame->format,
             is->video_st->time_base.num, is->video_st->time_base.den,
             codecpar->sample_aspect_ratio.num, FFMAX(codecpar->sample_aspect_ratio.den, 1));
    if (fr.num && fr.den)
        av_strlcatf(buffersrc_args, sizeof(buffersrc_args), ":frame_rate=%d/%d", fr.num, fr.den);

    if ((ret = avfilter_graph_create_filter(&filt_src,
                                            avfilter_get_by_name("buffer"),
                                            "ffplay_buffer", buffersrc_args, nullptr,
                                            graph)) < 0)
        goto fail;

    ret = avfilter_graph_create_filter(&filt_out,
                                       avfilter_get_by_name("buffersink"),
                                       "ffplay_buffersink", nullptr, nullptr, graph);
    if (ret < 0)
        goto fail;

    if ((ret = av_opt_set_int_list(filt_out, "pix_fmts", pix_fmts, AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN)) < 0)
        goto fail;

    last_filter = filt_out;

/* Note: this macro adds a filter before the lastly added filter, so the
 * processing order of the filters is in reverse */
#define INSERT_FILT(name, arg) do {                                          \
    AVFilterContext *filt_ctx;                                               \
                                                                             \
    ret = avfilter_graph_create_filter(&filt_ctx,                            \
                                       avfilter_get_by_name(name),           \
                                       "ffplay_" name, arg, nullptr, graph);    \
    if (ret < 0)                                                             \
        goto fail;                                                           \
                                                                             \
    ret = avfilter_link(filt_ctx, 0, last_filter, 0);                        \
    if (ret < 0)                                                             \
        goto fail;                                                           \
                                                                             \
    last_filter = filt_ctx;                                                  \
} while (0)

    if (autorotate) {
        double theta = get_rotation(is->video_st);

        if (fabs(theta - 90) < 1.0) {
            INSERT_FILT("transpose", "clock");
        } else if (fabs(theta - 180) < 1.0) {
            INSERT_FILT("hflip", nullptr);
            INSERT_FILT("vflip", nullptr);
        } else if (fabs(theta - 270) < 1.0) {
            INSERT_FILT("transpose", "cclock");
        } else if (fabs(theta) > 1.0) {
            char rotate_buf[64];
            snprintf(rotate_buf, sizeof(rotate_buf), "%f*PI/180", theta);
            INSERT_FILT("rotate", rotate_buf);
        }
    }

    if ((ret = configure_filtergraph(graph, vfilters, filt_src, last_filter)) < 0)
        goto fail;

    is->in_video_filter = filt_src;
    is->out_video_filter = filt_out;

    fail:
    return ret;
}

static int configure_audio_filters(VideoState *is, const char *afilters, int force_output_format) {
    static const enum AVSampleFormat sample_fmts[] = {AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_NONE};
    int sample_rates[2] = {0, -1};
    int64_t channel_layouts[2] = {0, -1};
    int channels[2] = {0, -1};
    AVFilterContext *filt_asrc = nullptr, *filt_asink = nullptr;
    char aresample_swr_opts[512] = "";
    AVDictionaryEntry *e = nullptr;
    char asrc_args[256];
    int ret;

    avfilter_graph_free(&is->agraph);
    if (!(is->agraph = avfilter_graph_alloc()))
        return AVERROR(ENOMEM);
    is->agraph->nb_threads = filter_nbthreads;

    while ((e = av_dict_get(swr_opts, "", e, AV_DICT_IGNORE_SUFFIX)))
        av_strlcatf(aresample_swr_opts, sizeof(aresample_swr_opts), "%s=%s:", e->key, e->value);
    if (strlen(aresample_swr_opts))
        aresample_swr_opts[strlen(aresample_swr_opts) - 1] = '\0';
    av_opt_set(is->agraph, "aresample_swr_opts", aresample_swr_opts, 0);

    ret = snprintf(asrc_args, sizeof(asrc_args),
                   "sample_rate=%d:sample_fmt=%s:channels=%d:time_base=%d/%d",
                   is->audio_filter_src.freq, av_get_sample_fmt_name(is->audio_filter_src.fmt),
                   is->audio_filter_src.channels,
                   1, is->audio_filter_src.freq);
    if (is->audio_filter_src.channel_layout)
        snprintf(asrc_args + ret, sizeof(asrc_args) - ret,
                 ":channel_layout=0x%" PRIx64, is->audio_filter_src.channel_layout);

    ret = avfilter_graph_create_filter(&filt_asrc,
                                       avfilter_get_by_name("abuffer"), "ffplay_abuffer",
                                       asrc_args, nullptr, is->agraph);
    if (ret < 0)
        goto end;


    ret = avfilter_graph_create_filter(&filt_asink,
                                       avfilter_get_by_name("abuffersink"), "ffplay_abuffersink",
                                       nullptr, nullptr, is->agraph);
    if (ret < 0)
        goto end;

    if ((ret = av_opt_set_int_list(filt_asink, "sample_fmts", sample_fmts, AV_SAMPLE_FMT_NONE,
                                   AV_OPT_SEARCH_CHILDREN)) < 0)
        goto end;
    if ((ret = av_opt_set_int(filt_asink, "all_channel_counts", 1, AV_OPT_SEARCH_CHILDREN)) < 0)
        goto end;

    if (force_output_format) {
        channel_layouts[0] = is->audio_tgt.channel_layout;
        channels[0] = is->audio_tgt.channels;
        sample_rates[0] = is->audio_tgt.freq;
        if ((ret = av_opt_set_int(filt_asink, "all_channel_counts", 0, AV_OPT_SEARCH_CHILDREN)) < 0)
            goto end;
        if ((ret = av_opt_set_int_list(filt_asink, "channel_layouts", channel_layouts, -1, AV_OPT_SEARCH_CHILDREN)) < 0)
            goto end;
        if ((ret = av_opt_set_int_list(filt_asink, "channel_counts", channels, -1, AV_OPT_SEARCH_CHILDREN)) < 0)
            goto end;
        if ((ret = av_opt_set_int_list(filt_asink, "sample_rates", sample_rates, -1, AV_OPT_SEARCH_CHILDREN)) < 0)
            goto end;
    }


    if ((ret = configure_filtergraph(is->agraph, afilters, filt_asrc, filt_asink)) < 0)
        goto end;

    is->in_audio_filter = filt_asrc;
    is->out_audio_filter = filt_asink;

    end:
    if (ret < 0)
        avfilter_graph_free(&is->agraph);
    return ret;
}

#endif  /* CONFIG_AVFILTER */

static int decoder_start(Decoder *d, int (*fn)(void *), const char *thread_name, void *arg) {
    packet_queue_start(d->queue);
    if (!(d->decoder_tid = SDL_CreateThread(fn, thread_name, arg))) {
        av_log(nullptr, AV_LOG_ERROR, "SDL_CreateThread(): %s\n", SDL_GetError());
        return AVERROR(ENOMEM);
    }
    return 0;
}

static int audio_thread(void *arg) {
    AVFrame *frame = av_frame_alloc();
    if (!frame)
        return AVERROR(ENOMEM);

#if CONFIG_AVFILTER
    int last_serial = -1;
    int64_t dec_channel_layout;
    int reconfigure;
#endif

    VideoState *is = static_cast<VideoState *>(arg);
    Frame *af;
    AVRational tb;
    int got_frame = 0;
    int ret = 0;

    printf("audio_thread() start\n");
    do {// frame 解码后的帧
        got_frame = decoder_decode_frame(&is->auddec, frame, nullptr);
        //printf("audio_thread() got_frame = %d\n", got_frame);// 1
        if (got_frame < 0)
            goto the_end;

        if (got_frame) {
            tb = (AVRational) {1, frame->sample_rate};

#if CONFIG_AVFILTER
            dec_channel_layout = get_valid_channel_layout(frame->channel_layout, frame->channels);

            reconfigure =
                    cmp_audio_fmts(is->audio_filter_src.fmt, is->audio_filter_src.channels,
                                   static_cast<AVSampleFormat>(frame->format), frame->channels) ||
                    is->audio_filter_src.channel_layout != dec_channel_layout ||
                    is->audio_filter_src.freq != frame->sample_rate ||
                    is->auddec.pkt_serial != last_serial;

            if (reconfigure) {
                char buf1[1024], buf2[1024];
                av_get_channel_layout_string(buf1, sizeof(buf1), -1, is->audio_filter_src.channel_layout);
                av_get_channel_layout_string(buf2, sizeof(buf2), -1, dec_channel_layout);
                printf("audio_thread() Audio frame changed from rate:%d ch:%d fmt:%s layout:%s serial:%d to rate:%d ch:%d fmt:%s layout:%s serial:%d\n",
                       is->audio_filter_src.freq, is->audio_filter_src.channels,
                       av_get_sample_fmt_name(is->audio_filter_src.fmt), buf1, last_serial,
                       frame->sample_rate, frame->channels,
                       av_get_sample_fmt_name(static_cast<AVSampleFormat>(frame->format)),
                       buf2, is->auddec.pkt_serial);

                is->audio_filter_src.fmt = static_cast<AVSampleFormat>(frame->format);
                is->audio_filter_src.channels = frame->channels;
                is->audio_filter_src.channel_layout = dec_channel_layout;
                is->audio_filter_src.freq = frame->sample_rate;
                last_serial = is->auddec.pkt_serial;

                if ((ret = configure_audio_filters(is, afilters, 1)) < 0)
                    goto the_end;
            }

            if ((ret = av_buffersrc_add_frame(is->in_audio_filter, frame)) < 0)
                goto the_end;

            while ((ret = av_buffersink_get_frame_flags(is->out_audio_filter, frame, 0)) >= 0) {
                tb = av_buffersink_get_time_base(is->out_audio_filter);
#endif
                if (!(af = frame_queue_peek_writable(&is->sampq)))
                    goto the_end;

                af->pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
                af->pos = frame->pkt_pos;
                af->serial = is->auddec.pkt_serial;
                af->duration = av_q2d((AVRational) {frame->nb_samples, frame->sample_rate});

                av_frame_move_ref(af->frame, frame);
                frame_queue_push(&is->sampq);

#if CONFIG_AVFILTER
                if (is->audioq.serial != is->auddec.pkt_serial)
                    break;
            }
            if (ret == AVERROR_EOF)
                is->auddec.finished = is->auddec.pkt_serial;
#endif
        }
    } while (ret >= 0 || ret == AVERROR(EAGAIN) || ret == AVERROR_EOF);
    printf("audio_thread() end\n");

    the_end:
#if CONFIG_AVFILTER
    avfilter_graph_free(&is->agraph);
#endif
    av_frame_free(&frame);
    return ret;
}

static int video_thread(void *arg) {
    AVFrame *frame = av_frame_alloc();
    if (!frame)
        return AVERROR(ENOMEM);

#if CONFIG_AVFILTER
    AVFilterGraph *graph = nullptr;
    AVFilterContext *filt_out = nullptr, *filt_in = nullptr;
    int last_w = 0;
    int last_h = 0;
    //enum AVPixelFormat last_format = -2;
    enum AVPixelFormat last_format = AV_PIX_FMT_NONE;
    int last_serial = -1;
    int last_vfilter_idx = 0;
#endif

    VideoState *is = static_cast<VideoState *>(arg);
    double pts;
    double duration;
    int ret = 0;
    AVRational tb = is->video_st->time_base;
    AVRational frame_rate = av_guess_frame_rate(is->ic, is->video_st, nullptr);

    printf("video_thread() start\n");
    for (;;) {
        ret = get_video_frame(is, frame);
        if (ret < 0)
            goto the_end;
        if (!ret)
            continue;

#if CONFIG_AVFILTER
        if (last_w != frame->width
            || last_h != frame->height
            || last_format != frame->format
            || last_serial != is->viddec.pkt_serial
            || last_vfilter_idx != is->vfilter_idx) {
            av_log(nullptr, AV_LOG_DEBUG,
                   "Video frame changed from size:%dx%d format:%s serial:%d to size:%dx%d format:%s serial:%d\n",
                   last_w, last_h,
                   (const char *) av_x_if_null(av_get_pix_fmt_name(last_format), "none"), last_serial,
                   frame->width, frame->height,
                   (const char *) av_x_if_null(av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame->format)), "none"),
                   is->viddec.pkt_serial);
            avfilter_graph_free(&graph);
            graph = avfilter_graph_alloc();
            if (!graph) {
                ret = AVERROR(ENOMEM);
                goto the_end;
            }
            graph->nb_threads = filter_nbthreads;
            if ((ret = configure_video_filters(graph, is,
                                               vfilters_list ? vfilters_list[is->vfilter_idx] : nullptr, frame)) < 0) {
                SDL_Event event;
                event.type = FF_QUIT_EVENT;
                event.user.data1 = is;
                SDL_PushEvent(&event);
                goto the_end;
            }
            filt_in = is->in_video_filter;
            filt_out = is->out_video_filter;
            last_w = frame->width;
            last_h = frame->height;
            last_format = static_cast<AVPixelFormat>(frame->format);
            last_serial = is->viddec.pkt_serial;
            last_vfilter_idx = is->vfilter_idx;
            frame_rate = av_buffersink_get_frame_rate(filt_out);
        }

        ret = av_buffersrc_add_frame(filt_in, frame);
        if (ret < 0)
            goto the_end;

        while (ret >= 0) {
            is->frame_last_returned_time = av_gettime_relative() / 1000000.0;

            ret = av_buffersink_get_frame_flags(filt_out, frame, 0);
            if (ret < 0) {
                if (ret == AVERROR_EOF)
                    is->viddec.finished = is->viddec.pkt_serial;
                ret = 0;
                break;
            }

            is->frame_last_filter_delay = av_gettime_relative() / 1000000.0 - is->frame_last_returned_time;
            if (fabs(is->frame_last_filter_delay) > AV_NOSYNC_THRESHOLD / 10.0)
                is->frame_last_filter_delay = 0;
            tb = av_buffersink_get_time_base(filt_out);
#endif
            duration = (frame_rate.num && frame_rate.den ? av_q2d((AVRational) {frame_rate.den, frame_rate.num}) : 0);
            pts = (frame->pts == AV_NOPTS_VALUE) ? NAN : frame->pts * av_q2d(tb);
            ret = queue_picture(is, frame, pts, duration, frame->pkt_pos, is->viddec.pkt_serial);
            av_frame_unref(frame);
#if CONFIG_AVFILTER
            if (is->videoq.serial != is->viddec.pkt_serial)
                break;
        }
#endif

        if (ret < 0)
            goto the_end;
    }
    printf("video_thread() end\n");

    the_end:
#if CONFIG_AVFILTER
    avfilter_graph_free(&graph);
#endif
    av_frame_free(&frame);
    return 0;
}

static int subtitle_thread(void *arg) {
    VideoState *is = static_cast<VideoState *>(arg);
    Frame *sp;
    int got_subtitle;
    double pts;

    printf("subtitle_thread() start\n");
    for (;;) {
        if (!(sp = frame_queue_peek_writable(&is->subpq)))
            return 0;

        if ((got_subtitle = decoder_decode_frame(&is->subdec, nullptr, &sp->sub)) < 0)
            break;

        pts = 0;

        if (got_subtitle && sp->sub.format == 0) {
            if (sp->sub.pts != AV_NOPTS_VALUE)
                pts = sp->sub.pts / (double) AV_TIME_BASE;
            sp->pts = pts;
            sp->serial = is->subdec.pkt_serial;
            sp->width = is->subdec.avctx->width;
            sp->height = is->subdec.avctx->height;
            sp->uploaded = 0;

            /* now we can update the picture count */
            frame_queue_push(&is->subpq);
        } else if (got_subtitle) {
            avsubtitle_free(&sp->sub);
        }
    }
    printf("subtitle_thread() end\n");

    return 0;
}

/* copy samples for viewing in editor window */
static void update_sample_display(VideoState *is, short *samples, int samples_size) {
    int size, len;

    size = samples_size / sizeof(short);
    while (size > 0) {
        len = SAMPLE_ARRAY_SIZE - is->sample_array_index;
        if (len > size)
            len = size;
        memcpy(is->sample_array + is->sample_array_index, samples, len * sizeof(short));
        samples += len;
        is->sample_array_index += len;
        if (is->sample_array_index >= SAMPLE_ARRAY_SIZE)
            is->sample_array_index = 0;
        size -= len;
    }
}

/* return the wanted number of samples to get better sync if sync_type is video
 * or external master clock */
static int synchronize_audio(VideoState *is, int nb_samples) {
    int wanted_nb_samples = nb_samples;

    /* if not master, then we try to remove or add samples to correct the clock */
    if (get_master_sync_type(is) != AV_SYNC_AUDIO_MASTER) {
        double diff, avg_diff;
        int min_nb_samples, max_nb_samples;

        diff = get_clock(&is->audclk) - get_master_clock(is);

        if (!isnan(diff) && fabs(diff) < AV_NOSYNC_THRESHOLD) {
            is->audio_diff_cum = diff + is->audio_diff_avg_coef * is->audio_diff_cum;
            if (is->audio_diff_avg_count < AUDIO_DIFF_AVG_NB) {
                /* not enough measures to have a correct estimate */
                is->audio_diff_avg_count++;
            } else {
                /* estimate the A-V difference */
                avg_diff = is->audio_diff_cum * (1.0 - is->audio_diff_avg_coef);

                if (fabs(avg_diff) >= is->audio_diff_threshold) {
                    wanted_nb_samples = nb_samples + (int) (diff * is->audio_src.freq);
                    min_nb_samples = ((nb_samples * (100 - SAMPLE_CORRECTION_PERCENT_MAX) / 100));
                    max_nb_samples = ((nb_samples * (100 + SAMPLE_CORRECTION_PERCENT_MAX) / 100));
                    wanted_nb_samples = av_clip(wanted_nb_samples, min_nb_samples, max_nb_samples);
                }
                av_log(nullptr, AV_LOG_TRACE, "diff=%f adiff=%f sample_diff=%d apts=%0.3f %f\n",
                       diff, avg_diff, wanted_nb_samples - nb_samples,
                       is->audio_clock, is->audio_diff_threshold);
            }
        } else {
            /* too big difference : may be initial PTS errors, so
               reset A-V filter */
            is->audio_diff_avg_count = 0;
            is->audio_diff_cum = 0;
        }
    }

    return wanted_nb_samples;
}

/**
 * Decode one audio frame and return its uncompressed size.
 *
 * The processed audio frame is decoded, converted if required, and
 * stored in is->audio_buf, with size in bytes given by the return
 * value.
 */
static int audio_decode_frame(VideoState *is) {
    int data_size, resampled_data_size;
    int64_t dec_channel_layout;
    av_unused double audio_clock0;
    int wanted_nb_samples;
    Frame *af;

    if (is->paused)
        return -1;

    do {
#if defined(_WIN32)
        while (frame_queue_nb_remaining(&is->sampq) == 0) {
            if ((av_gettime_relative() - audio_callback_time) > 1000000LL * is->audio_hw_buf_size / is->audio_tgt.bytes_per_sec / 2)
                return -1;
            av_usleep (1000);
        }
#endif
        if (!(af = frame_queue_peek_readable(&is->sampq)))
            return -1;
        frame_queue_next(&is->sampq);
    } while (af->serial != is->audioq.serial);

    data_size = av_samples_get_buffer_size(nullptr, af->frame->channels,
                                           af->frame->nb_samples,
                                           static_cast<AVSampleFormat>(af->frame->format), 1);

    dec_channel_layout =
            (af->frame->channel_layout &&
             af->frame->channels == av_get_channel_layout_nb_channels(af->frame->channel_layout)) ?
            af->frame->channel_layout : av_get_default_channel_layout(af->frame->channels);
    wanted_nb_samples = synchronize_audio(is, af->frame->nb_samples);

    if (af->frame->format != is->audio_src.fmt ||
        dec_channel_layout != is->audio_src.channel_layout ||
        af->frame->sample_rate != is->audio_src.freq ||
        (wanted_nb_samples != af->frame->nb_samples && !is->swr_ctx)) {
        swr_free(&is->swr_ctx);
        is->swr_ctx = swr_alloc_set_opts(nullptr,
                                         is->audio_tgt.channel_layout, is->audio_tgt.fmt, is->audio_tgt.freq,
                                         dec_channel_layout, static_cast<AVSampleFormat>(af->frame->format),
                                         af->frame->sample_rate,
                                         0, nullptr);
        if (!is->swr_ctx || swr_init(is->swr_ctx) < 0) {
            av_log(nullptr, AV_LOG_ERROR,
                   "Cannot create sample rate converter for conversion of %d Hz %s %d channels to %d Hz %s %d channels!\n",
                   af->frame->sample_rate, av_get_sample_fmt_name(static_cast<AVSampleFormat>(af->frame->format)),
                   af->frame->channels,
                   is->audio_tgt.freq, av_get_sample_fmt_name(is->audio_tgt.fmt), is->audio_tgt.channels);
            swr_free(&is->swr_ctx);
            return -1;
        }
        is->audio_src.channel_layout = dec_channel_layout;
        is->audio_src.channels = af->frame->channels;
        is->audio_src.freq = af->frame->sample_rate;
        is->audio_src.fmt = static_cast<AVSampleFormat>(af->frame->format);
    }

    if (is->swr_ctx) {
        const uint8_t **in = (const uint8_t **) af->frame->extended_data;
        uint8_t **out = &is->audio_buf1;
        int out_count = (int64_t) wanted_nb_samples * is->audio_tgt.freq / af->frame->sample_rate + 256;
        int out_size = av_samples_get_buffer_size(nullptr, is->audio_tgt.channels, out_count, is->audio_tgt.fmt, 0);
        int len2;
        if (out_size < 0) {
            av_log(nullptr, AV_LOG_ERROR, "av_samples_get_buffer_size() failed\n");
            return -1;
        }
        if (wanted_nb_samples != af->frame->nb_samples) {
            if (swr_set_compensation(is->swr_ctx, (wanted_nb_samples - af->frame->nb_samples) * is->audio_tgt.freq /
                                                  af->frame->sample_rate,
                                     wanted_nb_samples * is->audio_tgt.freq / af->frame->sample_rate) < 0) {
                av_log(nullptr, AV_LOG_ERROR, "swr_set_compensation() failed\n");
                return -1;
            }
        }
        av_fast_malloc(&is->audio_buf1, &is->audio_buf1_size, out_size);
        if (!is->audio_buf1)
            return AVERROR(ENOMEM);
        len2 = swr_convert(is->swr_ctx, out, out_count, in, af->frame->nb_samples);
        if (len2 < 0) {
            av_log(nullptr, AV_LOG_ERROR, "swr_convert() failed\n");
            return -1;
        }
        if (len2 == out_count) {
            av_log(nullptr, AV_LOG_WARNING, "audio buffer is probably too small\n");
            if (swr_init(is->swr_ctx) < 0)
                swr_free(&is->swr_ctx);
        }
        is->audio_buf = is->audio_buf1;
        resampled_data_size = len2 * is->audio_tgt.channels * av_get_bytes_per_sample(is->audio_tgt.fmt);
    } else {
        is->audio_buf = af->frame->data[0];
        resampled_data_size = data_size;
    }

    audio_clock0 = is->audio_clock;
    /* update the audio clock with the pts */
    if (!isnan(af->pts))
        is->audio_clock = af->pts + (double) af->frame->nb_samples / af->frame->sample_rate;
    else
        is->audio_clock = NAN;
    is->audio_clock_serial = af->serial;
#ifdef DEBUG
    {
        static double last_clock;
        printf("audio: delay=%0.3f clock=%0.3f clock0=%0.3f\n",
               is->audio_clock - last_clock,
               is->audio_clock, audio_clock0);
        last_clock = is->audio_clock;
    }
#endif
    return resampled_data_size;
}

/* prepare a new audio buffer */
static void sdl_audio_callback(void *opaque, Uint8 *stream, int len) {
    //printf("sdl_audio_callback() start\n");
    VideoState *is = static_cast<VideoState *>(opaque);
    int audio_size, len1;

    audio_callback_time = av_gettime_relative();

    while (len > 0) {
        //printf("sdl_audio_callback() while\n");
        if (is->audio_buf_index >= is->audio_buf_size) {
            audio_size = audio_decode_frame(is);
            if (audio_size < 0) {
                /* if error, just output silence */
                is->audio_buf = nullptr;
                is->audio_buf_size = SDL_AUDIO_MIN_BUFFER_SIZE / is->audio_tgt.frame_size * is->audio_tgt.frame_size;
            } else {
                if (is->show_mode != VideoState::SHOW_MODE_VIDEO)
                    update_sample_display(is, (int16_t *) is->audio_buf, audio_size);
                is->audio_buf_size = audio_size;
            }
            is->audio_buf_index = 0;
        }
        len1 = is->audio_buf_size - is->audio_buf_index;
        if (len1 > len)
            len1 = len;
        if (!is->muted && is->audio_buf && is->audio_volume == SDL_MIX_MAXVOLUME)
            memcpy(stream, (uint8_t *) is->audio_buf + is->audio_buf_index, len1);
        else {
            memset(stream, 0, len1);
            if (!is->muted && is->audio_buf)
                SDL_MixAudioFormat(stream, (uint8_t *) is->audio_buf + is->audio_buf_index, AUDIO_S16SYS, len1,
                                   is->audio_volume);
        }
        len -= len1;
        stream += len1;
        is->audio_buf_index += len1;
    }
    is->audio_write_buf_size = is->audio_buf_size - is->audio_buf_index;
    /* Let's assume the audio driver that is used by SDL has two periods. */
    if (!isnan(is->audio_clock)) {
        set_clock_at(&is->audclk,
                     is->audio_clock -
                     (double) (2 * is->audio_hw_buf_size + is->audio_write_buf_size) / is->audio_tgt.bytes_per_sec,
                     is->audio_clock_serial,
                     audio_callback_time / 1000000.0);
        sync_clock_to_slave(&is->extclk, &is->audclk);
    }
}

static int audio_open(void *opaque, int64_t wanted_channel_layout, int wanted_nb_channels, int wanted_sample_rate,
                      struct AudioParams *audio_hw_params) {
    SDL_AudioSpec wanted_spec, spec;
    const char *env;
    static const int next_nb_channels[] = {0, 0, 1, 6, 2, 6, 4, 6};
    static const int next_sample_rates[] = {0, 44100, 48000, 96000, 192000};
    int next_sample_rate_idx = FF_ARRAY_ELEMS(next_sample_rates) - 1;

    env = SDL_getenv("SDL_AUDIO_CHANNELS");
    if (env) {
        wanted_nb_channels = atoi(env);
        wanted_channel_layout = av_get_default_channel_layout(wanted_nb_channels);
    }
    if (!wanted_channel_layout || wanted_nb_channels != av_get_channel_layout_nb_channels(wanted_channel_layout)) {
        wanted_channel_layout = av_get_default_channel_layout(wanted_nb_channels);
        wanted_channel_layout &= ~AV_CH_LAYOUT_STEREO_DOWNMIX;
    }
    wanted_nb_channels = av_get_channel_layout_nb_channels(wanted_channel_layout);
    wanted_spec.channels = wanted_nb_channels;
    wanted_spec.freq = wanted_sample_rate;
    if (wanted_spec.freq <= 0 || wanted_spec.channels <= 0) {
        av_log(nullptr, AV_LOG_ERROR, "Invalid sample rate or channel count!\n");
        return -1;
    }
    while (next_sample_rate_idx && next_sample_rates[next_sample_rate_idx] >= wanted_spec.freq)
        next_sample_rate_idx--;
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.silence = 0;
    wanted_spec.samples = FFMAX(SDL_AUDIO_MIN_BUFFER_SIZE,
                                2 << av_log2(wanted_spec.freq / SDL_AUDIO_MAX_CALLBACKS_PER_SEC));
    wanted_spec.callback = sdl_audio_callback;
    wanted_spec.userdata = opaque;
    while (!(audio_dev = SDL_OpenAudioDevice(nullptr, 0, &wanted_spec, &spec,
                                             SDL_AUDIO_ALLOW_FREQUENCY_CHANGE | SDL_AUDIO_ALLOW_CHANNELS_CHANGE))) {
        av_log(nullptr, AV_LOG_WARNING, "SDL_OpenAudio (%d channels, %d Hz): %s\n",
               wanted_spec.channels, wanted_spec.freq, SDL_GetError());
        wanted_spec.channels = next_nb_channels[FFMIN(7, wanted_spec.channels)];
        if (!wanted_spec.channels) {
            wanted_spec.freq = next_sample_rates[next_sample_rate_idx--];
            wanted_spec.channels = wanted_nb_channels;
            if (!wanted_spec.freq) {
                av_log(nullptr, AV_LOG_ERROR,
                       "No more combinations to try, audio open failed\n");
                return -1;
            }
        }
        wanted_channel_layout = av_get_default_channel_layout(wanted_spec.channels);
    }
    if (spec.format != AUDIO_S16SYS) {
        av_log(nullptr, AV_LOG_ERROR,
               "SDL advised audio format %d is not supported!\n", spec.format);
        return -1;
    }
    if (spec.channels != wanted_spec.channels) {
        wanted_channel_layout = av_get_default_channel_layout(spec.channels);
        if (!wanted_channel_layout) {
            av_log(nullptr, AV_LOG_ERROR,
                   "SDL advised channel count %d is not supported!\n", spec.channels);
            return -1;
        }
    }

    audio_hw_params->fmt = AV_SAMPLE_FMT_S16;
    audio_hw_params->freq = spec.freq;
    audio_hw_params->channel_layout = wanted_channel_layout;
    audio_hw_params->channels = spec.channels;
    audio_hw_params->frame_size = av_samples_get_buffer_size(nullptr, audio_hw_params->channels, 1,
                                                             audio_hw_params->fmt,
                                                             1);
    audio_hw_params->bytes_per_sec = av_samples_get_buffer_size(nullptr, audio_hw_params->channels,
                                                                audio_hw_params->freq,
                                                                audio_hw_params->fmt, 1);
    if (audio_hw_params->bytes_per_sec <= 0 || audio_hw_params->frame_size <= 0) {
        av_log(nullptr, AV_LOG_ERROR, "av_samples_get_buffer_size failed\n");
        return -1;
    }
    return spec.size;
}

/* open a given stream. Return 0 if OK */
static int stream_component_open(VideoState *is, int stream_index) {
    if (stream_index < 0 || stream_index >= is->ic->nb_streams)
        return -1;

    AVFormatContext *ic = is->ic;
    AVCodecContext *avctx;
    AVCodec *codec;
    const char *forced_codec_name = nullptr;
    AVDictionary *opts = nullptr;
    AVDictionaryEntry *t = nullptr;
    int ret = 0;
    int stream_lowres = lowres;

    // video
    int image_get_buffer_size, image_fill_arrays;
    AVPixelFormat wanted_pix_fmt = AV_PIX_FMT_RGBA;
    SwsContext *swsContext = nullptr;

    // audio
    // wanted_sample_fmt决定wanted_audio_format
    int sample_rate, wanted_sample_rate, nb_channels, wanted_channels, wanted_audio_format = 2;
    int64_t channel_layout, wanted_channel_layout = AV_CH_LAYOUT_STEREO;
    AVSampleFormat sample_fmt, wanted_sample_fmt = AV_SAMPLE_FMT_S16;
    SwrContext *swrContext = nullptr;

    avctx = avcodec_alloc_context3(nullptr);
    if (!avctx)
        return AVERROR(ENOMEM);

    ret = avcodec_parameters_to_context(avctx, ic->streams[stream_index]->codecpar);
    if (ret < 0)
        goto fail;
    avctx->pkt_timebase = ic->streams[stream_index]->time_base;
    codec = avcodec_find_decoder(avctx->codec_id);

    switch (avctx->codec_type) {
        case AVMEDIA_TYPE_VIDEO   :
            is->last_video_stream = stream_index;
            forced_codec_name = video_codec_name;
            break;
        case AVMEDIA_TYPE_AUDIO   :
            is->last_audio_stream = stream_index;
            forced_codec_name = audio_codec_name;
            break;
        case AVMEDIA_TYPE_SUBTITLE:
            is->last_subtitle_stream = stream_index;
            forced_codec_name = subtitle_codec_name;
            break;
        default:
            break;
    }
    if (forced_codec_name) {
        printf("create_avformat_context() forced_codec_name = %s\n", forced_codec_name);
        codec = avcodec_find_decoder_by_name(forced_codec_name);
    }
    if (!codec) {
        if (forced_codec_name)
            av_log(nullptr, AV_LOG_WARNING,
                   "No codec could be found with name '%s'\n", forced_codec_name);
        else
            av_log(nullptr, AV_LOG_WARNING,
                   "No decoder could be found for codec %s\n", avcodec_get_name(avctx->codec_id));
        ret = AVERROR(EINVAL);
        goto fail;
    }

    avctx->codec_id = codec->id;
    if (stream_lowres > codec->max_lowres) {
        av_log(avctx, AV_LOG_WARNING, "The maximum value for lowres supported by the decoder is %d\n",
               codec->max_lowres);
        stream_lowres = codec->max_lowres;
    }
    avctx->lowres = stream_lowres;

    if (fast)
        avctx->flags2 |= AV_CODEC_FLAG2_FAST;

    opts = filter_codec_opts(codec_opts, avctx->codec_id, ic, ic->streams[stream_index], codec);
    if (!av_dict_get(opts, "threads", nullptr, 0))
        av_dict_set(&opts, "threads", "auto", 0);
    if (stream_lowres)
        av_dict_set_int(&opts, "lowres", stream_lowres, 0);
    if (avctx->codec_type == AVMEDIA_TYPE_VIDEO || avctx->codec_type == AVMEDIA_TYPE_AUDIO)
        av_dict_set(&opts, "refcounted_frames", "1", 0);
    if ((ret = avcodec_open2(avctx, codec, &opts)) < 0) {
        goto fail;
    }
    if ((t = av_dict_get(opts, "", nullptr, AV_DICT_IGNORE_SUFFIX))) {
        av_log(nullptr, AV_LOG_ERROR, "Option %s not found.\n", t->key);
        ret = AVERROR_OPTION_NOT_FOUND;
        goto fail;
    }

    is->eof = 0;
    ic->streams[stream_index]->discard = AVDISCARD_DEFAULT;
    switch (avctx->codec_type) {
        case AVMEDIA_TYPE_VIDEO:
            is->video_stream = stream_index;
            is->video_st = ic->streams[stream_index];

            decoder_init(&is->viddec, avctx, &is->videoq, &is->pcontinue_read_thread);
            /*if ((ret = decoder_start(&is->viddec, video_thread, "video_decoder", is)) < 0)
                goto out;*/
            is->queue_attachments_req = 1;

#ifdef OS_ANDROID
            is->rgbAVFrame = av_frame_alloc();
            image_get_buffer_size = av_image_get_buffer_size(wanted_pix_fmt, avctx->width, avctx->height, 1);
            is->videoOutBufferSize = image_get_buffer_size * sizeof(unsigned char);
            is->videoOutBuffer = (unsigned char *) av_malloc(is->videoOutBufferSize);
            image_fill_arrays = av_image_fill_arrays(is->rgbAVFrame->data,
                                                     is->rgbAVFrame->linesize,
                                                     is->videoOutBuffer,
                                                     wanted_pix_fmt,
                                                     avctx->width,
                                                     avctx->height,
                                                     1);
            printf("stream_component_open()        avctx->pix_fmt = %d\n", av_get_pix_fmt_name(avctx->pix_fmt));
            printf("stream_component_open()        wanted_pix_fmt = %d\n", av_get_pix_fmt_name(wanted_pix_fmt));
            printf("stream_component_open() image_get_buffer_size = %d\n", image_get_buffer_size);
            printf("stream_component_open()    videoOutBufferSize = %d\n", is->videoOutBufferSize);
            printf("stream_component_open()     image_fill_arrays = %d\n", image_fill_arrays);
            if (image_fill_arrays < 0)
                goto fail;
            swsContext = sws_getContext(avctx->width, avctx->height, avctx->pix_fmt,
                                        avctx->width, avctx->height, wanted_pix_fmt,
                                        SWS_BICUBIC, nullptr, nullptr, nullptr);
            if (!swsContext) {
                printf("stream_component_open() swsContext is nullptr\n");
                goto fail;
            }
            if (is->rgbAVFrame) {
                av_frame_free(&is->rgbAVFrame);
                is->rgbAVFrame = nullptr;
            }
            if (is->videoOutBuffer) {
                av_free(is->videoOutBuffer);
                is->videoOutBuffer = nullptr;
            }
            if (swsContext) {
                sws_freeContext(swsContext);
                swsContext = nullptr;
            }
#else
#endif
            break;
        case AVMEDIA_TYPE_AUDIO:
#if CONFIG_AVFILTER
        {
            AVFilterContext *sink;

            is->audio_filter_src.freq = avctx->sample_rate;
            is->audio_filter_src.channels = avctx->channels;
            is->audio_filter_src.channel_layout = get_valid_channel_layout(avctx->channel_layout, avctx->channels);
            is->audio_filter_src.fmt = avctx->sample_fmt;
            if ((ret = configure_audio_filters(is, afilters, 0)) < 0)
                goto fail;
            sink = is->out_audio_filter;
            sample_rate = av_buffersink_get_sample_rate(sink);
            nb_channels = av_buffersink_get_channels(sink);
            sample_fmt = static_cast<AVSampleFormat>(av_buffersink_get_format(sink));
            channel_layout = av_buffersink_get_channel_layout(sink);
        }
#else
            sample_rate    = avctx->sample_rate;
            nb_channels    = avctx->channels;
            sample_fmt     = avctx->sample_fmt;
            channel_layout = avctx->channel_layout;
#endif

            /* prepare audio output */
            if ((ret = audio_open(is, channel_layout, nb_channels, sample_rate, &is->audio_tgt)) < 0)
                goto fail;
            is->audio_hw_buf_size = ret;
            is->audio_src = is->audio_tgt;
            is->audio_buf_size = 0;
            is->audio_buf_index = 0;

            /* init averaging filter */
            is->audio_diff_avg_coef = exp(log(0.01) / AUDIO_DIFF_AVG_NB);
            is->audio_diff_avg_count = 0;
            /* since we do not have a precise anough audio FIFO fullness,
               we correct audio sync only if larger than this threshold */
            is->audio_diff_threshold = (double) (is->audio_hw_buf_size) / is->audio_tgt.bytes_per_sec;

            is->audio_stream = stream_index;
            is->audio_st = ic->streams[stream_index];

            decoder_init(&is->auddec, avctx, &is->audioq, &is->pcontinue_read_thread);
            if ((is->ic->iformat->flags & (AVFMT_NOBINSEARCH | AVFMT_NOGENSEARCH | AVFMT_NO_BYTE_SEEK)) &&
                !is->ic->iformat->read_seek) {
                is->auddec.start_pts = is->audio_st->start_time;
                is->auddec.start_pts_tb = is->audio_st->time_base;
            }

            /*if ((ret = decoder_start(&is->auddec, audio_thread, "audio_decoder", is)) < 0)
                goto out;
            SDL_PauseAudioDevice(audio_dev, 0);*/

#ifdef OS_ANDROID
            // android
            wanted_sample_rate = sample_rate;
            wanted_channels = av_get_channel_layout_nb_channels(wanted_channel_layout);

            printf("stream_component_open()           sample_rate = %d\n", sample_rate);
            printf("stream_component_open()    avctx->sample_rate = %d\n", avctx->sample_rate);
            printf("stream_component_open()    wanted_sample_rate = %d\n", wanted_sample_rate);
            printf("stream_component_open()              channels = %d\n", nb_channels);
            printf("stream_component_open()       avctx->channels = %d\n", avctx->channels);
            printf("stream_component_open()       wanted_channels = %d\n", wanted_channels);
            printf("stream_component_open()            sample_fmt = %d\n", av_get_sample_fmt_name(sample_fmt));
            printf("stream_component_open()     avctx->sample_fmt = %d\n", av_get_sample_fmt_name(avctx->sample_fmt));
            printf("stream_component_open()     wanted_sample_fmt = %d\n", av_get_sample_fmt_name(wanted_sample_fmt));
            printf("stream_component_open()        channel_layout = %d\n", channel_layout);
            printf("stream_component_open() avctx->channel_layout = %d\n", avctx->channel_layout);
            printf("stream_component_open() wanted_channel_layout = %d\n", wanted_channel_layout);

            swrContext = swr_alloc();
            swr_alloc_set_opts(swrContext,
                               wanted_channel_layout,
                               wanted_sample_fmt,
                               wanted_sample_rate,
                               channel_layout,
                               static_cast<AVSampleFormat>(sample_fmt),// avctx->sample_fmt
                               sample_rate,
                               0, nullptr);
            if (!swrContext) {
                printf("stream_component_open() swrContext is nullptr\n");
                goto fail;
            } else {
                ret = swr_init(swrContext);
                if (ret != 0) {
                    printf("stream_component_open() swrContext swr_init failed\n");
                    goto fail;
                } else {
                    printf("stream_component_open() swrContext swr_init success\n");
                }
            }
            if (swrContext) {
                swr_free(&swrContext);
                swrContext = nullptr;
            }
            //createAudioTrack(wanted_sample_rate, wanted_channels, audioFormat);
            is->audioOutBuffer = (unsigned char *) av_malloc(MAX_AUDIO_FRAME_SIZE);
            is->audioOutBufferSize = MAX_AUDIO_FRAME_SIZE;
            if (is->audioOutBuffer) {
                av_free(is->audioOutBuffer);
                is->audioOutBuffer = nullptr;
            }
#else
#endif
            break;
        case AVMEDIA_TYPE_SUBTITLE:
            is->subtitle_stream = stream_index;
            is->subtitle_st = ic->streams[stream_index];

            decoder_init(&is->subdec, avctx, &is->subtitleq, &is->pcontinue_read_thread);
            /*if ((ret = decoder_start(&is->subdec, subtitle_thread, "subtitle_decoder", is)) < 0)
                goto out;*/
            break;
        default:
            break;
    }
    goto out;

    fail:
    avcodec_free_context(&avctx);
    out:
    av_dict_free(&opts);

    return ret;
}

static int decode_interrupt_cb(void *ctx) {
    VideoState *is = static_cast<VideoState *>(ctx);
    return is->abort_request;
}

static int stream_has_enough_packets(AVStream *st, int stream_id, PacketQueue *queue) {
    if (stream_id < 0 || queue->abort_request) {
        return 1;
    } else {
        // st->disposition & AV_DISPOSITION_ATTACHED_PIC;// 长时间为0
        // !queue->duration || av_q2d(st->time_base) * queue->duration > 1.0;// 长时间为1
        return (queue->nb_packets > MIN_FRAMES
                && (!queue->duration || av_q2d(st->time_base) * queue->duration > 1.0))
               || (st->disposition & AV_DISPOSITION_ATTACHED_PIC);
    }
    /*return stream_id < 0 ||
           queue->abort_request ||
           (st->disposition & AV_DISPOSITION_ATTACHED_PIC) ||
           queue->nb_packets > MIN_FRAMES && (!queue->duration || av_q2d(st->time_base) * queue->duration > 1.0);*/
}

static int is_realtime(AVFormatContext *s) {
    if (!strcmp(s->iformat->name, "rtp") ||
        !strcmp(s->iformat->name, "rtsp") ||
        !strcmp(s->iformat->name, "sdp")) {
        return 1;
    }

    if (s->pb && (!strncmp(s->url, "rtp:", 4) || !strncmp(s->url, "udp:", 4))) {
        return 1;
    }
    return 0;
}

static int read_thread(void *arg) {
    printf("read_thread() start\n");
    VideoState *is = static_cast<VideoState *>(arg);
    AVFormatContext *pAvFormatContext = is->ic;
    AVPacket pkt1, *pkt = &pkt1;
    int64_t stream_start_time;
    int64_t pkt_ts;
    int pkt_in_play_range = 0;
    int ret;
    pthread_mutex_t wait_mutex = PTHREAD_MUTEX_INITIALIZER;

    // seekTo
    /*double incr, pos;
    if (seek_by_bytes) {
        incr = 10.000000;
        pos = -1;
        if (pos < 0 && is->video_stream >= 0)
            pos = frame_queue_last_pos(&is->pictq);
        if (pos < 0 && is->audio_stream >= 0)
            pos = frame_queue_last_pos(&is->sampq);
        if (pos < 0)
            pos = avio_tell(is->ic->pb);
        if (is->ic->bit_rate)
            incr *= is->ic->bit_rate / 8.0;
        else
            incr *= 180000.0;
        printf("read_thread2()  pos = %lf incr = %lf\n", pos, incr);
        pos += incr;
        printf("read_thread()  pos = %lf incr = %lf\n", pos, incr);
        pos = 22000000.000000;
        stream_seek(is, pos, incr, 1);
    } else {
        incr = 10.000000;
        pos = 180.000000;
        // pos为需要seek到的那个时间点
        stream_seek(is, (int64_t) (pos * AV_TIME_BASE), (int64_t) (incr * AV_TIME_BASE), 0);
    }*/

    printf("read_thread() video_stream = %d\n", video_state->video_stream);
    printf("read_thread() audio_stream = %d\n", video_state->audio_stream);

    struct timespec abstime;
    struct timeval now;
    long timeout_ms = 10; // wait time 10ms

    for (;;) {
        // region is->abort_request
        if (is->abort_request) {
            // 调用stream_close后为1
            break;
        }
        // endregion

        // region is->paused != is->last_paused
        if (is->paused != is->last_paused) {
            printf("read_thread() is->paused = %d is->last_paused = %d\n", is->paused, is->last_paused);
            is->last_paused = is->paused;
            if (is->paused) {
                is->read_pause_return = av_read_pause(pAvFormatContext);
                printf("read_thread() av_read_pause read_pause_return = %d\n", is->read_pause_return);
            } else {
                av_read_play(pAvFormatContext);
                printf("read_thread() av_read_play\n");
            }
        }
        // endregion

        // region CONFIG_RTSP_DEMUXER || CONFIG_MMSH_PROTOCOL
#if CONFIG_RTSP_DEMUXER || CONFIG_MMSH_PROTOCOL
        if (is->paused &&
            (!strcmp(pAvFormatContext->iformat->name, "rtsp") ||
             (pAvFormatContext->pb && !strncmp(input_filename, "mmsh:", 5)))) {
            printf("read_thread() SDL_Delay(10)\n");
            /* wait 10 ms to avoid trying to get another packet */
            /* XXX: horrible */
            SDL_Delay(10);
            continue;
        }
#endif
        // endregion

        // region is->seek_req
        if (is->seek_req) {
            printf("read_thread() is->seek_req\n");
            // INT64_MIN -9223372036854775808
            // INT64_MAX  9223372036854775807
            int64_t seek_target = is->seek_pos;
            int64_t seek_min = is->seek_rel > 0 ? seek_target - is->seek_rel + 2 : INT64_MIN;
            int64_t seek_max = is->seek_rel < 0 ? seek_target - is->seek_rel - 2 : INT64_MAX;
            // FIXME the +-2 is due to rounding being not done in the correct direction in generation
            //      of the seek_pos/seek_rel variables
            printf("read_thread()    seek_min = %ld\n", (long) seek_min);
            printf("read_thread() seek_target = %ld\n", (long) seek_target);
            printf("read_thread()    seek_max = %ld\n", (long) seek_max);

            ret = avformat_seek_file(is->ic, -1, seek_min, seek_target, seek_max, is->seek_flags);
            printf("read_thread()         ret = %d\n", ret);
            if (ret < 0) {
                av_log(nullptr, AV_LOG_ERROR,
                       "%s: error while seeking\n", is->ic->url);
            } else {
                if (is->video_stream >= 0) {
                    packet_queue_flush(&is->videoq);
                    packet_queue_put(&is->videoq, &flush_pkt);
                }
                if (is->audio_stream >= 0) {
                    packet_queue_flush(&is->audioq);
                    packet_queue_put(&is->audioq, &flush_pkt);
                }
                if (is->subtitle_stream >= 0) {
                    packet_queue_flush(&is->subtitleq);
                    packet_queue_put(&is->subtitleq, &flush_pkt);
                }
                if (is->seek_flags & AVSEEK_FLAG_BYTE) {
                    set_clock(&is->extclk, NAN, 0);
                } else {
                    set_clock(&is->extclk, seek_target / (double) AV_TIME_BASE, 0);
                }
            }
            is->seek_req = 0;
            is->queue_attachments_req = 1;
            is->eof = 0;
            if (is->paused)
                step_to_next_frame(is);
        }
        // endregion

        // region is->queue_attachments_req
        if (is->queue_attachments_req) {
            printf("read_thread() is->queue_attachments_req\n");
            if (is->video_st && is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC) {
                AVPacket copy;
                if ((ret = av_packet_ref(&copy, &is->video_st->attached_pic)) < 0)
                    goto fail;
                packet_queue_put(&is->videoq, &copy);
                packet_queue_put_nullpacket(&is->videoq, is->video_stream);
            }
            is->queue_attachments_req = 0;
        }
        // endregion

        // region if the queue are full, no need to read more
        if (infinite_buffer < 1 &&
            (//is->audioq.size + is->videoq.size + is->subtitleq.size > MAX_QUEUE_SIZE ||
                    (stream_has_enough_packets(is->audio_st, is->audio_stream, &is->audioq) &&
                     stream_has_enough_packets(is->video_st, is->video_stream, &is->videoq) &&
                     stream_has_enough_packets(is->subtitle_st, is->subtitle_stream, &is->subtitleq)))) {
            //printf("read_thread() SDL_CondWaitTimeout(10)\n");
            /* wait 10 ms */
            pthread_mutex_lock(&wait_mutex);
            gettimeofday(&now, nullptr);
            long nsec = now.tv_usec * 1000 + (timeout_ms % 1000) * 1000000;
            abstime.tv_sec = now.tv_sec + nsec / 1000000000 + timeout_ms / 1000;
            abstime.tv_nsec = nsec % 1000000000;
            pthread_cond_timedwait(&is->pcontinue_read_thread, &wait_mutex, &abstime);
            pthread_mutex_unlock(&wait_mutex);

            continue;
        }
        // endregion

        // region
        if (!is->paused
            &&
            (!is->audio_st || (is->auddec.finished == is->audioq.serial && frame_queue_nb_remaining(&is->sampq) == 0))
            &&
            (!is->video_st || (is->viddec.finished == is->videoq.serial && frame_queue_nb_remaining(&is->pictq) == 0)))
            //
        {
            if (loop != 1 && (!loop || --loop)) {
                stream_seek(is, start_time != AV_NOPTS_VALUE ? start_time : 0, 0, 0);
            } else if (autoexit) {
                ret = AVERROR_EOF;
                goto fail;
            }
        }
        // endregion

        ret = av_read_frame(pAvFormatContext, pkt);
        if (ret < 0) {
            // region
            if ((ret == AVERROR_EOF || avio_feof(pAvFormatContext->pb)) && !is->eof) {
                if (is->video_stream >= 0) {
                    packet_queue_put_nullpacket(&is->videoq, is->video_stream);
                }
                if (is->audio_stream >= 0) {
                    packet_queue_put_nullpacket(&is->audioq, is->audio_stream);
                }
                if (is->subtitle_stream >= 0) {
                    packet_queue_put_nullpacket(&is->subtitleq, is->subtitle_stream);
                }
                is->eof = 1;
            }
            if (pAvFormatContext->pb && pAvFormatContext->pb->error) {
                break;
            }

            pthread_mutex_lock(&wait_mutex);
            gettimeofday(&now, nullptr);
            long nsec = now.tv_usec * 1000 + (timeout_ms % 1000) * 1000000;
            abstime.tv_sec = now.tv_sec + nsec / 1000000000 + timeout_ms / 1000;
            abstime.tv_nsec = nsec % 1000000000;
            pthread_cond_timedwait(&is->pcontinue_read_thread, &wait_mutex, &abstime);
            pthread_mutex_unlock(&wait_mutex);

            continue;
            // endregion
        } else {
            is->eof = 0;
        }

        /* check if packet is in play range specified by user, then queue, otherwise discard */
        stream_start_time = pAvFormatContext->streams[pkt->stream_index]->start_time;
        pkt_ts = pkt->pts == AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
        pkt_in_play_range =
                duration == AV_NOPTS_VALUE ||
                (pkt_ts - (stream_start_time != AV_NOPTS_VALUE ? stream_start_time : 0)) *
                av_q2d(pAvFormatContext->streams[pkt->stream_index]->time_base) -
                (double) (start_time != AV_NOPTS_VALUE ? start_time : 0) / 1000000
                <= ((double) duration / 1000000);

        // region save AVPacket
        if (pkt->stream_index == is->audio_stream && pkt_in_play_range) {
            packet_queue_put(&is->audioq, pkt);
        } else if (pkt->stream_index == is->video_stream
                   && pkt_in_play_range
                   && !(is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
            packet_queue_put(&is->videoq, pkt);
        } else if (pkt->stream_index == is->subtitle_stream && pkt_in_play_range) {
            packet_queue_put(&is->subtitleq, pkt);
        } else {
            av_packet_unref(pkt);
        }
        // endregion
    }// for (;;) end

    ret = 0;
    fail:

    if (ret != 0) {
        SDL_Event event;

        event.type = FF_QUIT_EVENT;
        event.user.data1 = is;
        SDL_PushEvent(&event);
    }
    pthread_mutex_destroy(&wait_mutex);
    printf("read_thread() end\n");
    return ret;
}

/* this thread gets the stream from the disk or the network */
static int create_avformat_context(void *arg) {
    printf("create_avformat_context() start\n");
    VideoState *is = static_cast<VideoState *>(arg);
    AVFormatContext *ic = nullptr;
    AVDictionaryEntry *t = nullptr;
    int ret;
    int scan_all_pmts_set = 0;

    int st_index[AVMEDIA_TYPE_NB];// 5
    memset(st_index, -1, sizeof(st_index));
    is->eof = 0;

    if (!(ic = avformat_alloc_context())) {
        av_log(nullptr, AV_LOG_FATAL, "Could not allocate context.\n");
        ret = AVERROR(ENOMEM);
        goto fail;
    }
    ic->interrupt_callback.callback = decode_interrupt_cb;
    ic->interrupt_callback.opaque = is;
    if (!av_dict_get(format_opts, "scan_all_pmts", nullptr, AV_DICT_MATCH_CASE)) {
        av_dict_set(&format_opts, "scan_all_pmts", "1", AV_DICT_DONT_OVERWRITE);
        scan_all_pmts_set = 1;
    }
    ret = avformat_open_input(&ic, is->filename, is->iformat, &format_opts);
    if (ret < 0) {
        print_error(is->filename, ret);
        //ret = -1;
        goto fail;
    }
    printf("create_avformat_context() scan_all_pmts_set = %d\n", scan_all_pmts_set);// 1
    if (scan_all_pmts_set)
        av_dict_set(&format_opts, "scan_all_pmts", nullptr, AV_DICT_MATCH_CASE);

    if ((t = av_dict_get(format_opts, "", nullptr, AV_DICT_IGNORE_SUFFIX))) {
        av_log(nullptr, AV_LOG_ERROR, "Option %s not found.\n", t->key);
        ret = AVERROR_OPTION_NOT_FOUND;
        goto fail;
    }
    is->ic = ic;

    media_duration = (long) (ic->duration / AV_TIME_BASE);
    printf("create_avformat_context() media_duration = %ld\n", media_duration);
    if (ic->duration != AV_NOPTS_VALUE) {
        // 得到的是秒数
        media_duration = (long) ((ic->duration + 5000) / AV_TIME_BASE);
        long hours, mins, seconds;
        seconds = media_duration;
        mins = seconds / 60;
        seconds %= 60;
        hours = mins / 60;
        mins %= 60;
        // 00:54:16
        // 单位: 秒
        printf("create_avformat_context() media  seconds = %ld\n", media_duration);
        printf("create_avformat_context() media          %02d:%02d:%02d\n", hours, mins, seconds);
    }

    printf("create_avformat_context() genpts = %d\n", genpts);// 0
    if (genpts)
        ic->flags |= AVFMT_FLAG_GENPTS;

    av_format_inject_global_side_data(ic);

    printf("create_avformat_context() find_stream_info = %d\n", find_stream_info);// 1
    if (find_stream_info) {
        AVDictionary **opts = setup_find_stream_info_opts(ic, codec_opts);
        int orig_nb_streams = ic->nb_streams;

        ret = avformat_find_stream_info(ic, opts);

        for (int i = 0; i < orig_nb_streams; i++)
            av_dict_free(&opts[i]);
        av_freep(&opts);

        if (ret < 0) {
            av_log(nullptr, AV_LOG_WARNING,
                   "%s: could not find codec parameters\n", is->filename);
            //ret = -1;
            goto fail;
        }
    }

    if (ic->pb)
        ic->pb->eof_reached = 0; // FIXME hack, ffplay maybe should not use avio_feof() to test for the end

    printf("create_avformat_context() 1 seek_by_bytes = %d\n", seek_by_bytes);// -1
    if (seek_by_bytes < 0) {
        int flag1 = ic->iformat->flags & AVFMT_TS_DISCONT;
        int flag2 = strcmp("ogg", ic->iformat->name);
        printf("create_avformat_context() flag1 = %d\n", flag1);
        printf("create_avformat_context() flag2 = %d\n", flag2);
        seek_by_bytes = !!(flag1) && flag2;
    }
    printf("create_avformat_context() 2 seek_by_bytes = %d\n", seek_by_bytes);// 0

    is->max_frame_duration = (ic->iformat->flags & AVFMT_TS_DISCONT) ? 10.0 : 3600.0;
    printf("create_avformat_context() max_frame_duration = %lf\n", is->max_frame_duration);

    if (!window_title && (t = av_dict_get(ic->metadata, "title", nullptr, 0)))
        window_title = av_asprintf("%s - %s", t->value, input_filename);
    printf("create_avformat_context() window_title = %s\n", window_title);

    printf("create_avformat_context() start_time = %ld\n", (long) start_time);
    /* if seeking requested, we execute it */
    if (start_time != AV_NOPTS_VALUE) {// -9223372036854775808
        int64_t timestamp;

        timestamp = start_time;
        /* add the stream start time */
        if (ic->start_time != AV_NOPTS_VALUE)
            timestamp += ic->start_time;
        ret = avformat_seek_file(ic, -1, INT64_MIN, timestamp, INT64_MAX, 0);
        if (ret < 0) {
            av_log(nullptr, AV_LOG_WARNING, "%s: could not seek to position %0.3f\n",
                   is->filename, (double) timestamp / AV_TIME_BASE);
        }
    }

    is->realtime = is_realtime(ic);
    printf("create_avformat_context() realtime = %d\n", is->realtime);// 0

    printf("create_avformat_context() show_status = %d\n", show_status);
    /*if (show_status)
        av_dump_format(ic, 0, is->filename, 0);*/

    for (int i = 0; i < ic->nb_streams; i++) {
        AVStream *st = ic->streams[i];
        enum AVMediaType type = st->codecpar->codec_type;
        st->discard = AVDISCARD_ALL;
        printf("create_avformat_context() wanted_stream_spec[%d] = %s\n", type, wanted_stream_spec[type]);
        if (type >= 0 && wanted_stream_spec[type] && st_index[type] == -1)
            if (avformat_match_stream_specifier(ic, st, wanted_stream_spec[type]) > 0)
                st_index[type] = i;
    }
    for (int i = 0; i < AVMEDIA_TYPE_NB; i++) {
        if (wanted_stream_spec[i] && st_index[i] == -1) {
            av_log(nullptr, AV_LOG_ERROR, "Stream specifier %s does not match any %s stream\n",
                   wanted_stream_spec[i],
                   av_get_media_type_string(static_cast<AVMediaType>(i)));
            st_index[i] = INT_MAX;
        }
        printf("create_avformat_context() st_index[%d] = %d\n", i, st_index[i]);
    }

    printf("create_avformat_context()    audio_disable = %d\n", audio_disable);
    printf("create_avformat_context()    video_disable = %d\n", video_disable);
    printf("create_avformat_context() subtitle_disable = %d\n", subtitle_disable);
    if (!video_disable)
        st_index[AVMEDIA_TYPE_VIDEO] =
                av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO,
                                    st_index[AVMEDIA_TYPE_VIDEO],
                                    -1, nullptr, 0);
    if (!audio_disable)
        st_index[AVMEDIA_TYPE_AUDIO] =
                av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO,
                                    st_index[AVMEDIA_TYPE_AUDIO],
                                    st_index[AVMEDIA_TYPE_VIDEO],
                                    nullptr, 0);
    if (!video_disable && !subtitle_disable)
        st_index[AVMEDIA_TYPE_SUBTITLE] =
                av_find_best_stream(ic, AVMEDIA_TYPE_SUBTITLE,
                                    st_index[AVMEDIA_TYPE_SUBTITLE],
                                    (st_index[AVMEDIA_TYPE_AUDIO] >= 0 ? st_index[AVMEDIA_TYPE_AUDIO]
                                                                       : st_index[AVMEDIA_TYPE_VIDEO]),
                                    nullptr, 0);
    for (int i = 0; i < AVMEDIA_TYPE_NB; i++) {
        printf("create_avformat_context() st_index[%d] = %d\n", i, st_index[i]);
    }

    ret = -1;
    is->show_mode = show_mode;
    printf("create_avformat_context() show_mode = %d\n", show_mode);
    if (st_index[AVMEDIA_TYPE_VIDEO] >= 0) {
        AVStream *st = ic->streams[st_index[AVMEDIA_TYPE_VIDEO]];
        AVCodecParameters *codecpar = st->codecpar;
        AVRational sar = av_guess_sample_aspect_ratio(ic, st, nullptr);
        if (codecpar->width)
            set_default_window_size(codecpar->width, codecpar->height, sar);
        printf("create_avformat_context() width = %d height = %d\n", codecpar->width, codecpar->height);
        ret = stream_component_open(is, st_index[AVMEDIA_TYPE_VIDEO]);
    }

    /* open the streams */
    if (st_index[AVMEDIA_TYPE_AUDIO] >= 0) {
        stream_component_open(is, st_index[AVMEDIA_TYPE_AUDIO]);
    }

    if (st_index[AVMEDIA_TYPE_SUBTITLE] >= 0) {// st_index[3] = -1381258232
        stream_component_open(is, st_index[AVMEDIA_TYPE_SUBTITLE]);
    }

    if (is->show_mode == VideoState::SHOW_MODE_NONE) {
        // ret >= 0时,表示有video,因此使用VideoState::SHOW_MODE_VIDEO模式
        is->show_mode = ret >= 0 ? VideoState::SHOW_MODE_VIDEO : VideoState::SHOW_MODE_RDFT;
    }

    if (is->video_stream < 0 && is->audio_stream < 0) {
        av_log(nullptr, AV_LOG_FATAL, "Failed to open file '%s' or configure filtergraph\n", is->filename);
        ret = -1;
        goto fail;
    }

    if (infinite_buffer < 0 && is->realtime) {
        infinite_buffer = 1;
    }
    printf("create_avformat_context() infinite_buffer = %d\n", infinite_buffer);// -1

    ret = 0;
    fail:
    if (ic && !is->ic) {
        avformat_close_input(&ic);
        ic = nullptr;
    }

    if (ret != 0) {
        SDL_Event event;

        event.type = FF_QUIT_EVENT;
        event.user.data1 = is;
        SDL_PushEvent(&event);
    }
    printf("create_avformat_context() ret = %d\n", ret);
    printf("create_avformat_context() end\n");
    return ret;
    //return 0;
}

static VideoState *stream_open(const char *filename, AVInputFormat *iformat) {
    printf("stream_open() start\n");
    printf("stream_open() filename: %s\n", filename);

    // 自己定义的参数进行初始化
    media_duration = -1;

    VideoState *is;
    is = static_cast<VideoState *>(av_mallocz(sizeof(VideoState)));
    if (!is)
        return nullptr;
    video_state = is;
    int ret = 0;

    //filename为需要拷贝的字符串
    //av_strdup返回一个指向新分配的内存，该内存拷贝了一份字符串，如果无法分配出空间，则返回nullptr
    //需要调用av_free释放空间
    if (!(is->filename = av_strdup(filename)))
        goto fail;

    is->pcontinue_read_thread = PTHREAD_COND_INITIALIZER;
    is->last_video_stream = is->video_stream = -1;
    is->last_audio_stream = is->audio_stream = -1;
    is->last_subtitle_stream = is->subtitle_stream = -1;
    is->ytop = 0;
    is->xleft = 0;
    is->audio_clock_serial = -1;
    is->iformat = iformat;
    if (!is->iformat) {
        printf("stream_open() is->iformat is nullptr\n");
    }

    /* start video display */
    if (frame_queue_init(&is->pictq, &is->videoq, VIDEO_PICTURE_QUEUE_SIZE, 1) < 0 ||
        frame_queue_init(&is->sampq, &is->audioq, SAMPLE_QUEUE_SIZE, 1) < 0 ||
        frame_queue_init(&is->subpq, &is->subtitleq, SUBPICTURE_QUEUE_SIZE, 0) < 0)
        goto fail;

    if (packet_queue_init(&is->videoq) < 0 ||
        packet_queue_init(&is->audioq) < 0 ||
        packet_queue_init(&is->subtitleq) < 0)
        goto fail;

    printf("stream_open() videoq.serial = %d\n", is->videoq.serial);
    printf("stream_open() audioq.serial = %d\n", is->audioq.serial);
    printf("stream_open() extclk.serial = %d\n", is->extclk.serial);

    init_clock(&is->vidclk, &is->videoq.serial);
    init_clock(&is->audclk, &is->audioq.serial);
    init_clock(&is->extclk, &is->extclk.serial);

    printf("stream_open() 1 startup_volume = %d\n", startup_volume);// 100
    if (startup_volume < 0)
        av_log(nullptr, AV_LOG_WARNING, "-volume=%d < 0, setting to 0\n", startup_volume);
    if (startup_volume > 100)
        av_log(nullptr, AV_LOG_WARNING, "-volume=%d > 100, setting to 100\n", startup_volume);
    startup_volume = av_clip(startup_volume, 0, 100);
    startup_volume = av_clip(SDL_MIX_MAXVOLUME * startup_volume / 100, 0, SDL_MIX_MAXVOLUME);
    is->audio_volume = startup_volume;
    printf("stream_open() 2 startup_volume = %d\n", startup_volume);// 128
    is->muted = 0;
    is->av_sync_type = av_sync_type;

    if ((ret = create_avformat_context(is)) < 0) {
        printf("stream_open() create_avformat_context(is) < 0\n");
        goto fail;
    }

    ///////////////////////创建线程///////////////////////

    if (!(is->read_tid = SDL_CreateThread(read_thread, "read_thread", is))) {
        av_log(nullptr, AV_LOG_FATAL, "SDL_CreateThread(): %s\n", SDL_GetError());
        ret = -1;
        goto fail;
    }

    if (is->video_stream >= 0) {
        if ((ret = decoder_start(&is->viddec, video_thread, "video_decoder", is)) < 0)
            goto fail;
    }

    if (is->audio_stream >= 0) {
        if (ret = decoder_start(&is->auddec, audio_thread, "audio_decoder", is) < 0)
            goto fail;
        SDL_PauseAudioDevice(audio_dev, 0);
    }

    if (is->subtitle_stream >= 0) {
        if ((ret = decoder_start(&is->subdec, subtitle_thread, "subtitle_decoder", is)) < 0)
            goto fail;
    }

    ret = 0;
    fail:
    if (ret < 0) {
        stream_close(is);
        return nullptr;
    }

    printf("stream_open() end\n");
    return is;
}

static void stream_cycle_channel(VideoState *is, int codec_type) {
    AVFormatContext *ic = is->ic;
    int start_index, stream_index;
    int old_index;
    AVStream *st;
    AVProgram *p = nullptr;
    int nb_streams = is->ic->nb_streams;

    if (codec_type == AVMEDIA_TYPE_VIDEO) {
        start_index = is->last_video_stream;
        old_index = is->video_stream;
    } else if (codec_type == AVMEDIA_TYPE_AUDIO) {
        start_index = is->last_audio_stream;
        old_index = is->audio_stream;
    } else {
        start_index = is->last_subtitle_stream;
        old_index = is->subtitle_stream;
    }
    stream_index = start_index;

    if (codec_type != AVMEDIA_TYPE_VIDEO && is->video_stream != -1) {
        p = av_find_program_from_stream(ic, nullptr, is->video_stream);
        if (p) {
            nb_streams = p->nb_stream_indexes;
            for (start_index = 0; start_index < nb_streams; start_index++)
                if (p->stream_index[start_index] == stream_index)
                    break;
            if (start_index == nb_streams)
                start_index = -1;
            stream_index = start_index;
        }
    }

    for (;;) {
        if (++stream_index >= nb_streams) {
            if (codec_type == AVMEDIA_TYPE_SUBTITLE) {
                stream_index = -1;
                is->last_subtitle_stream = -1;
                goto the_end;
            }
            if (start_index == -1)
                return;
            stream_index = 0;
        }
        if (stream_index == start_index)
            return;
        st = is->ic->streams[p ? p->stream_index[stream_index] : stream_index];
        if (st->codecpar->codec_type == codec_type) {
            /* check that parameters are OK */
            switch (codec_type) {
                case AVMEDIA_TYPE_AUDIO:
                    if (st->codecpar->sample_rate != 0 &&
                        st->codecpar->channels != 0)
                        goto the_end;
                    break;
                case AVMEDIA_TYPE_VIDEO:
                case AVMEDIA_TYPE_SUBTITLE:
                    goto the_end;
                default:
                    break;
            }
        }
    }
    the_end:
    if (p && stream_index != -1)
        stream_index = p->stream_index[stream_index];
    av_log(nullptr, AV_LOG_INFO, "Switch %s stream from #%d to #%d\n",
           av_get_media_type_string(static_cast<AVMediaType>(codec_type)),
           old_index,
           stream_index);

    stream_component_close(is, old_index);
    stream_component_open(is, stream_index);
}


static void toggle_full_screen(VideoState *is) {
    is_full_screen = !is_full_screen;
    SDL_SetWindowFullscreen(window, is_full_screen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

static void toggle_audio_display(VideoState *is) {
    int next = is->show_mode;
    do {
        next = (next + 1) % VideoState::SHOW_MODE_NB;
    } while (next != is->show_mode && (next == VideoState::SHOW_MODE_VIDEO && !is->video_st ||
                                       next != VideoState::SHOW_MODE_VIDEO && !is->audio_st));
    if (is->show_mode != next) {
        is->force_refresh = 1;
        is->show_mode = static_cast<VideoState::ShowMode>(next);
    }
}

/***
 * 显示视频
 *
 * 循环检测并优先处理用户输入事件
 * 内置刷新率控制，约10ms刷新一次
 */
static void refresh_loop_wait_event(VideoState *is, SDL_Event *event) {
    double remaining_time = 0.0;
    /* 从输入设备收集事件并放到事件队列中 */
    SDL_PumpEvents();
    //printf("refresh_loop_wait_event() start\n");
    while (1) {
        // region
        /***
         * SDL_PeepEvents接受到键盘,鼠标有关事件后就会跳出while循环处理其他事情,然后又进入循环
         *
         * SDL_PeepEvents
         * 从事件队列中提取事件，由于这里使用的是SDL_GETEVENT, 所以获取事件时会从队列中移除
         * 如果有事件发生，返回事件数量，则while循环不执行。
         * 如果出错，返回负数的错误码，则while循环不执行。
         * 如果当前没有事件发生，且没有出错，返回0，进入while循环。
         */
        if (!SDL_PeepEvents(event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT)) {
        } else {
            break;
        }
        // 鼠标显示着,但过了一段时间后,就隐藏鼠标
        if (!cursor_hidden && av_gettime_relative() - cursor_last_shown > CURSOR_HIDE_DELAY) {
            // 隐藏鼠标
            SDL_ShowCursor(0);
            cursor_hidden = 1;
        }
        // endregion

        // region
        /* 默认屏幕刷新率控制，REFRESH_RATE = 10ms */
        if (remaining_time > 0.0) {
            av_usleep((int64_t) (remaining_time * 1000000.0));
        }
        remaining_time = REFRESH_RATE;
        //printf("refresh_loop_wait_event() paused = %d force_refresh = %d\n", is->paused, is->force_refresh);
        if (is->show_mode != VideoState::SHOW_MODE_NONE && (!is->paused || is->force_refresh)) {
            //printf("video_refresh() remaining_time = %d\n", remaining_time);
            video_refresh(is, &remaining_time);
        }
        // endregion

        /* 再次检测输入事件 */
        SDL_PumpEvents();
    }
    //printf("refresh_loop_wait_event() end\n");
}

static void seek_chapter(VideoState *is, int incr) {
    int64_t pos = get_master_clock(is) * AV_TIME_BASE;
    int i;

    if (!is->ic->nb_chapters)
        return;

    /* find the current chapter */
    for (i = 0; i < is->ic->nb_chapters; i++) {
        AVChapter *ch = is->ic->chapters[i];
        if (av_compare_ts(pos, AV_TIME_BASE_Q, ch->start, ch->time_base) < 0) {
            i--;
            break;
        }
    }

    i += incr;
    i = FFMAX(i, 0);
    if (i >= is->ic->nb_chapters)
        return;

    av_log(nullptr, AV_LOG_VERBOSE, "Seeking to chapter %d.\n", i);
    stream_seek(is, av_rescale_q(is->ic->chapters[i]->start, is->ic->chapters[i]->time_base,
                                 AV_TIME_BASE_Q), 0, 0);
}

/* handle an event sent by the GUI */
static void event_loop(VideoState *is) {// 原来的参数名: cur_stream
    printf("event_loop()     seek_interval = %f\n", seek_interval);
    printf("event_loop()     seek_by_bytes = %d\n", seek_by_bytes);
    printf("event_loop()   exit_on_keydown = %d\n", exit_on_keydown);
    printf("event_loop() exit_on_mousedown = %d\n", exit_on_mousedown);

    SDL_Event event;
    double incr, pos, frac;
    printf("event_loop() for start\n");
    for (;;) {
        double x;
        refresh_loop_wait_event(is, &event);
        //printf("event_loop() event.type = %d\n", event.type);
        switch (event.type) {
            case SDL_KEYDOWN:// 768
                //printf("event_loop()         SDL_KEYDOWN = %d\n", SDL_KEYDOWN);
                if (exit_on_keydown ||
                    event.key.keysym.sym == SDLK_ESCAPE ||
                    event.key.keysym.sym == SDLK_q) {
                    do_exit(is);
                    break;
                }
                // If we don't yet have a window, skip all key events,
                // because create_avformat_context might still be initializing...
                if (!is->width) {
                    continue;
                }
                // 某些键的功能
                switch (event.key.keysym.sym) {
                    case SDLK_f:
                        // 进入全屏 退出全屏
                        toggle_full_screen(is);
                        is->force_refresh = 1;
                        break;
                    case SDLK_p:
                    case SDLK_SPACE:
                        // 暂停 播放
                        toggle_pause(is);
                        break;
                    case SDLK_m:
                        // 静音 出音
                        toggle_mute(is);
                        break;
                    case SDLK_KP_MULTIPLY:
                    case SDLK_0:
                        update_volume(is, 1, SDL_VOLUME_STEP);
                        break;
                    case SDLK_KP_DIVIDE:
                    case SDLK_9:
                        update_volume(is, -1, SDL_VOLUME_STEP);
                        break;
                    case SDLK_s: // S: Step to next frame
                        // 按一下"s"键播放一帧
                        step_to_next_frame(is);
                        break;
                    case SDLK_a:
                        stream_cycle_channel(is, AVMEDIA_TYPE_AUDIO);
                        break;
                    case SDLK_v:
                        stream_cycle_channel(is, AVMEDIA_TYPE_VIDEO);
                        break;
                    case SDLK_c:
                        stream_cycle_channel(is, AVMEDIA_TYPE_VIDEO);
                        stream_cycle_channel(is, AVMEDIA_TYPE_AUDIO);
                        stream_cycle_channel(is, AVMEDIA_TYPE_SUBTITLE);
                        break;
                    case SDLK_t:
                        stream_cycle_channel(is, AVMEDIA_TYPE_SUBTITLE);
                        break;
                    case SDLK_w:
#if CONFIG_AVFILTER
                        if (is->show_mode == VideoState::SHOW_MODE_VIDEO && is->vfilter_idx < nb_vfilters - 1) {
                            if (++is->vfilter_idx >= nb_vfilters)
                                is->vfilter_idx = 0;
                        } else {
                            is->vfilter_idx = 0;
                            toggle_audio_display(is);
                        }
#else
                        toggle_audio_display(cur_stream);
#endif
                        break;
                    case SDLK_PAGEUP: {
                        if (is->ic->nb_chapters <= 1) {
                            incr = 600.0;
                            goto do_seek;
                        }
                        seek_chapter(is, 1);
                        break;
                    }
                    case SDLK_PAGEDOWN: {
                        if (is->ic->nb_chapters <= 1) {
                            incr = -600.0;
                            goto do_seek;
                        }
                        seek_chapter(is, -1);
                        break;
                    }
                    case SDLK_LEFT: {
                        incr = seek_interval ? -seek_interval : -10.0;// -10.0
                        goto do_seek;
                    }
                    case SDLK_RIGHT: {
                        incr = seek_interval ? seek_interval : 10.0; //  10.0
                        goto do_seek;
                    }
                    case SDLK_UP: {
                        incr = 60.0;
                        goto do_seek;
                    }
                    case SDLK_DOWN:
                        incr = -60.0;
                    do_seek:

                        /*pos = get_master_clock(is);
                        if (isnan(pos))
                            pos = (double) is->seek_pos / AV_TIME_BASE;
                        pos += incr;
                        if (is->ic->start_time != AV_NOPTS_VALUE
                            && pos < is->ic->start_time / (double) AV_TIME_BASE)
                            pos = is->ic->start_time / (double) AV_TIME_BASE;
                        printf("event_loop()  pos = %lf incr = %lf seek_by_bytes = %d\n", pos, incr, seek_by_bytes);
                        stream_seek(is, (int64_t) (pos * AV_TIME_BASE), (int64_t) (incr * AV_TIME_BASE), 0);*/

                        if (seek_by_bytes) {
                            pos = -1;
                            if (pos < 0 && is->video_stream >= 0)
                                pos = frame_queue_last_pos(&is->pictq);
                            if (pos < 0 && is->audio_stream >= 0)
                                pos = frame_queue_last_pos(&is->sampq);
                            if (pos < 0)
                                pos = avio_tell(is->ic->pb);
                            if (is->ic->bit_rate)
                                incr *= is->ic->bit_rate / 8.0;
                            else
                                incr *= 180000.0;
                            pos += incr;
                            printf("event_loop()  pos = %lf incr = %lf seek_by_bytes = %d\n", pos, incr, seek_by_bytes);
                            stream_seek(is, pos, incr, 1);
                        } else {
                            pos = get_master_clock(is);
                            if (isnan(pos))
                                pos = (double) is->seek_pos / AV_TIME_BASE;
                            pos += incr;
                            if (is->ic->start_time != AV_NOPTS_VALUE
                                && pos < is->ic->start_time / (double) AV_TIME_BASE)
                                pos = is->ic->start_time / (double) AV_TIME_BASE;
                            printf("event_loop()  pos = %lf incr = %lf seek_by_bytes = %d\n", pos, incr, seek_by_bytes);
                            stream_seek(is, (int64_t) (pos * AV_TIME_BASE), (int64_t) (incr * AV_TIME_BASE), 0);
                        }
                        break;
                    default:
                        break;
                }
                break;
            case SDL_MOUSEBUTTONDOWN:// 1025
                printf("event_loop() SDL_MOUSEBUTTONDOWN = %d\n", SDL_MOUSEBUTTONDOWN);
                if (exit_on_mousedown) {
                    do_exit(is);
                    break;
                }
                // 鼠标左键双击
                if (event.button.button == SDL_BUTTON_LEFT) {
                    static int64_t last_mouse_left_click = 0;
                    if (av_gettime_relative() - last_mouse_left_click <= 500000) {
                        toggle_full_screen(is);
                        is->force_refresh = 1;
                        last_mouse_left_click = 0;
                    } else {
                        last_mouse_left_click = av_gettime_relative();
                    }
                }
            case SDL_MOUSEMOTION:// 1024 鼠标移动到播放界面上时
                //printf("event_loop()     SDL_MOUSEMOTION = %d\n", SDL_MOUSEMOTION);
                if (cursor_hidden) {
                    // 显示鼠标
                    SDL_ShowCursor(1);
                    cursor_hidden = 0;
                }
                cursor_last_shown = av_gettime_relative();
                if (event.type == SDL_MOUSEBUTTONDOWN) {
                    if (event.button.button != SDL_BUTTON_RIGHT)
                        break;
                    x = event.button.x;
                } else {
                    if (!(event.motion.state & SDL_BUTTON_RMASK))
                        break;
                    x = event.motion.x;
                }
                if (seek_by_bytes || is->ic->duration <= 0) {
                    uint64_t size = avio_size(is->ic->pb);
                    stream_seek(is, size * x / is->width, 0, 1);
                } else {
                    int64_t ts;
                    int ns, hh, mm, ss;
                    int tns, thh, tmm, tss;
                    tns = is->ic->duration / 1000000LL;
                    thh = tns / 3600;
                    tmm = (tns % 3600) / 60;
                    tss = (tns % 60);
                    frac = x / is->width;
                    ns = frac * tns;
                    hh = ns / 3600;
                    mm = (ns % 3600) / 60;
                    ss = (ns % 60);
                    av_log(nullptr, AV_LOG_INFO,
                           "Seek to %2.0f%% (%2d:%02d:%02d) of total duration (%2d:%02d:%02d)       \n", frac * 100,
                           hh, mm, ss, thh, tmm, tss);
                    ts = frac * is->ic->duration;
                    if (is->ic->start_time != AV_NOPTS_VALUE)
                        ts += is->ic->start_time;
                    stream_seek(is, ts, 0, 0);
                }
                break;
            case SDL_WINDOWEVENT:// 512
                //printf("event_loop()     SDL_WINDOWEVENT = %d\n", SDL_WINDOWEVENT);
                switch (event.window.event) {
                    case SDL_WINDOWEVENT_SIZE_CHANGED:
                        printf("event_loop() SDL_WINDOWEVENT SDL_WINDOWEVENT_SIZE_CHANGED\n");
                        screen_width = is->width = event.window.data1;
                        screen_height = is->height = event.window.data2;
                        if (is->vis_texture) {
                            SDL_DestroyTexture(is->vis_texture);
                            is->vis_texture = nullptr;
                        }
                    case SDL_WINDOWEVENT_EXPOSED:
                        printf("event_loop() SDL_WINDOWEVENT SDL_WINDOWEVENT_EXPOSED\n");
                        is->force_refresh = 1;
                    default:
                        break;
                }
                break;
            case SDL_QUIT:
                printf("event_loop()            SDL_QUIT = %d\n", SDL_QUIT);
                do_exit(is);
                break;
            case FF_QUIT_EVENT:
                printf("event_loop()       FF_QUIT_EVENT = %d\n", FF_QUIT_EVENT);
                do_exit(is);
                break;
            default:
                break;
        }
    }// for (;;) end
    printf("event_loop() for end\n");
}

static int opt_frame_size(void *optctx, const char *opt, const char *arg) {
    av_log(nullptr, AV_LOG_WARNING, "Option -s is deprecated, use -video_size.\n");
    return opt_default(nullptr, "video_size", arg);
}

static int opt_width(void *optctx, const char *opt, const char *arg) {
    screen_width = parse_number_or_die(opt, arg, OPT_INT64, 1, INT_MAX);
    return 0;
}

static int opt_height(void *optctx, const char *opt, const char *arg) {
    screen_height = parse_number_or_die(opt, arg, OPT_INT64, 1, INT_MAX);
    return 0;
}

static int opt_format(void *optctx, const char *opt, const char *arg) {
    file_iformat = av_find_input_format(arg);
    if (!file_iformat) {
        av_log(nullptr, AV_LOG_FATAL, "Unknown input format: %s\n", arg);
        return AVERROR(EINVAL);
    }
    return 0;
}

static int opt_frame_pix_fmt(void *optctx, const char *opt, const char *arg) {
    av_log(nullptr, AV_LOG_WARNING, "Option -pix_fmt is deprecated, use -pixel_format.\n");
    return opt_default(nullptr, "pixel_format", arg);
}

static int opt_sync(void *optctx, const char *opt, const char *arg) {
    if (!strcmp(arg, "audio"))
        av_sync_type = AV_SYNC_AUDIO_MASTER;
    else if (!strcmp(arg, "video"))
        av_sync_type = AV_SYNC_VIDEO_MASTER;
    else if (!strcmp(arg, "ext"))
        av_sync_type = AV_SYNC_EXTERNAL_CLOCK;
    else {
        av_log(nullptr, AV_LOG_ERROR, "Unknown value for %s: %s\n", opt, arg);
        exit(1);
    }
    return 0;
}

static int opt_seek(void *optctx, const char *opt, const char *arg) {
    start_time = parse_time_or_die(opt, arg, 1);
    return 0;
}

static int opt_duration(void *optctx, const char *opt, const char *arg) {
    duration = parse_time_or_die(opt, arg, 1);
    return 0;
}

static int opt_show_mode(void *optctx, const char *opt, const char *arg) {
    show_mode = static_cast<VideoState::ShowMode>(!strcmp(arg, "video") ? VideoState::SHOW_MODE_VIDEO :
                                                  !strcmp(arg, "waves") ? VideoState::SHOW_MODE_WAVES :
                                                  !strcmp(arg, "rdft") ? VideoState::SHOW_MODE_RDFT :
                                                  parse_number_or_die(opt, arg, OPT_INT, 0,
                                                                      VideoState::SHOW_MODE_NB - 1));
    return 0;
}

static void opt_input_file(void *optctx, const char *filename) {
    if (input_filename) {
        av_log(nullptr, AV_LOG_FATAL,
               "Argument '%s' provided as input filename, but '%s' was already specified.\n",
               filename, input_filename);
        exit(1);
    }
    if (!strcmp(filename, "-"))
        filename = "pipe:";
    input_filename = filename;
}

static int opt_codec(void *optctx, const char *opt, const char *arg) {
    const char *spec = strchr(opt, ':');
    if (!spec) {
        av_log(nullptr, AV_LOG_ERROR,
               "No media specifier was specified in '%s' in option '%s'\n",
               arg, opt);
        return AVERROR(EINVAL);
    }
    spec++;
    switch (spec[0]) {
        case 'a' :
            audio_codec_name = arg;
            break;
        case 's' :
            subtitle_codec_name = arg;
            break;
        case 'v' :
            video_codec_name = arg;
            break;
        default:
            av_log(nullptr, AV_LOG_ERROR,
                   "Invalid media specifier '%s' in option '%s'\n", spec, opt);
            return AVERROR(EINVAL);
    }
    return 0;
}

static int dummy;

static const OptionDef options[] = {
        CMDUTILS_COMMON_OPTIONS
        {"x", HAS_ARG, {.func_arg = opt_width}, "force displayed width", "width"},
        {"y", HAS_ARG, {.func_arg = opt_height}, "force displayed height", "height"},
        {"s", HAS_ARG | OPT_VIDEO, {.func_arg = opt_frame_size}, "set frame size (WxH or abbreviation)", "size"},
        {"fs", OPT_BOOL, {&is_full_screen}, "force full screen"},
        {"an", OPT_BOOL, {&audio_disable}, "disable audio"},
        {"vn", OPT_BOOL, {&video_disable}, "disable video"},
        {"sn", OPT_BOOL, {&subtitle_disable}, "disable subtitling"},
        {"ast", OPT_STRING | HAS_ARG | OPT_EXPERT, {&wanted_stream_spec[AVMEDIA_TYPE_AUDIO]},
         "select desired audio stream", "stream_specifier"},
        {"vst", OPT_STRING | HAS_ARG | OPT_EXPERT, {&wanted_stream_spec[AVMEDIA_TYPE_VIDEO]},
         "select desired video stream", "stream_specifier"},
        {"sst", OPT_STRING | HAS_ARG | OPT_EXPERT, {&wanted_stream_spec[AVMEDIA_TYPE_SUBTITLE]},
         "select desired subtitle stream", "stream_specifier"},
        {"ss", HAS_ARG, {.func_arg = opt_seek}, "seek to a given position in seconds", "pos"},
        {"t", HAS_ARG, {.func_arg = opt_duration}, "play  \"duration\" seconds of audio/video", "duration"},
        {"bytes", OPT_INT | HAS_ARG, {&seek_by_bytes}, "seek by bytes 0=off 1=on -1=auto", "val"},
        {"seek_interval", OPT_FLOAT | HAS_ARG, {&seek_interval}, "set seek interval for left/right keys, in seconds",
         "seconds"},
        {"nodisp", OPT_BOOL, {&display_disable}, "disable graphical display"},
        {"noborder", OPT_BOOL, {&borderless}, "borderless window"},
        {"alwaysontop", OPT_BOOL, {&alwaysontop}, "window always on top"},
        {"volume", OPT_INT | HAS_ARG, {&startup_volume}, "set startup volume 0=min 100=max", "volume"},
        {"f", HAS_ARG, {.func_arg = opt_format}, "force format", "fmt"},
        {"pix_fmt", HAS_ARG | OPT_EXPERT | OPT_VIDEO, {.func_arg = opt_frame_pix_fmt}, "set pixel format", "format"},
        {"stats", OPT_BOOL | OPT_EXPERT, {&show_status}, "show status", ""},
        {"fast", OPT_BOOL | OPT_EXPERT, {&fast}, "non spec compliant optimizations", ""},
        {"genpts", OPT_BOOL | OPT_EXPERT, {&genpts}, "generate pts", ""},
        {"drp", OPT_INT | HAS_ARG | OPT_EXPERT, {&decoder_reorder_pts}, "let decoder reorder pts 0=off 1=on -1=auto",
         ""},
        {"lowres", OPT_INT | HAS_ARG | OPT_EXPERT, {&lowres}, "", ""},
        {"sync", HAS_ARG | OPT_EXPERT, {.func_arg = opt_sync}, "set audio-video sync. type (type=audio/video/ext)",
         "type"},
        {"autoexit", OPT_BOOL | OPT_EXPERT, {&autoexit}, "exit at the end", ""},
        {"exitonkeydown", OPT_BOOL | OPT_EXPERT, {&exit_on_keydown}, "exit on key down", ""},
        {"exitonmousedown", OPT_BOOL | OPT_EXPERT, {&exit_on_mousedown}, "exit on mouse down", ""},
        {"loop", OPT_INT | HAS_ARG | OPT_EXPERT, {&loop}, "set number of times the playback shall be looped",
         "loop count"},
        {"framedrop", OPT_BOOL | OPT_EXPERT, {&framedrop}, "drop frames when cpu is too slow", ""},
        {"infbuf", OPT_BOOL | OPT_EXPERT, {&infinite_buffer},
         "don't limit the input buffer size (useful with realtime streams)", ""},
        {"window_title", OPT_STRING | HAS_ARG, {&window_title}, "set window title", "window title"},
        {"left", OPT_INT | HAS_ARG | OPT_EXPERT, {&screen_left}, "set the x position for the left of the window",
         "x pos"},
        {"top", OPT_INT | HAS_ARG | OPT_EXPERT, {&screen_top}, "set the y position for the top of the window", "y pos"},
#if CONFIG_AVFILTER
        {"vf", OPT_EXPERT | HAS_ARG, {.func_arg = opt_add_vfilter}, "set video filters", "filter_graph"},
        {"af", OPT_STRING | HAS_ARG, {&afilters}, "set audio filters", "filter_graph"},
#endif
        {"rdftspeed", OPT_INT | HAS_ARG | OPT_AUDIO | OPT_EXPERT, {&rdftspeed}, "rdft speed", "msecs"},
        {"showmode", HAS_ARG, {.func_arg = opt_show_mode}, "select show mode (0 = video, 1 = waves, 2 = RDFT)", "mode"},
        {"default", HAS_ARG | OPT_AUDIO | OPT_VIDEO | OPT_EXPERT, {.func_arg = opt_default}, "generic catch all option",
         ""},
        {"i", OPT_BOOL, {&dummy}, "read specified file", "input_file"},
        {"codec", HAS_ARG, {.func_arg = opt_codec}, "force decoder", "decoder_name"},
        {"acodec", HAS_ARG | OPT_STRING | OPT_EXPERT, {&audio_codec_name}, "force audio decoder", "decoder_name"},
        {"scodec", HAS_ARG | OPT_STRING | OPT_EXPERT, {&subtitle_codec_name}, "force subtitle decoder", "decoder_name"},
        {"vcodec", HAS_ARG | OPT_STRING | OPT_EXPERT, {&video_codec_name}, "force video decoder", "decoder_name"},
        {"autorotate", OPT_BOOL, {&autorotate}, "automatically rotate video", ""},
        {"find_stream_info", OPT_BOOL | OPT_INPUT | OPT_EXPERT, {&find_stream_info},
         "read and decode the streams to fill missing information with heuristics"},
        {"filter_threads", HAS_ARG | OPT_INT | OPT_EXPERT, {&filter_nbthreads}, "number of filter threads per graph"},
        {nullptr,},
};

static void show_usage(void) {
    av_log(nullptr, AV_LOG_INFO, "Simple media player\n");
    av_log(nullptr, AV_LOG_INFO, "usage: %s [options] input_file\n", program_name);
    av_log(nullptr, AV_LOG_INFO, "\n");
}

void show_help_default(const char *opt, const char *arg) {
    av_log_set_callback(log_callback_help);
    show_usage();
    show_help_options(options, "Main options:", 0, OPT_EXPERT, 0);
    show_help_options(options, "Advanced options:", OPT_EXPERT, 0, 0);
    printf("\n");
    show_help_children(avcodec_get_class(), AV_OPT_FLAG_DECODING_PARAM);
    show_help_children(avformat_get_class(), AV_OPT_FLAG_DECODING_PARAM);
#if !CONFIG_AVFILTER
    show_help_children(sws_get_class(), AV_OPT_FLAG_ENCODING_PARAM);
#else
    show_help_children(avfilter_get_class(), AV_OPT_FLAG_FILTERING_PARAM);
#endif
    printf("\nWhile playing:\n"
           "q, ESC              quit\n"
           "f                   toggle full screen\n"
           "p, SPC              pause\n"
           "m                   toggle mute\n"
           "9, 0                decrease and increase volume respectively\n"
           "/, *                decrease and increase volume respectively\n"
           "a                   cycle audio channel in the current program\n"
           "v                   cycle video channel\n"
           "t                   cycle subtitle channel in the current program\n"
           "c                   cycle program\n"
           "w                   cycle video filters or show modes\n"
           "s                   activate frame-step mode\n"
           "left/right          seek backward/forward 10 seconds or to custom interval if -seek_interval is set\n"
           "down/up             seek backward/forward 1 minute\n"
           "page down/page up   seek backward/forward 10 minutes\n"
           "right mouse click   seek to percentage in file corresponding to fraction of width\n"
           "left double-click   toggle full screen\n"
    );
}

void test();

/* Called from the main */
int main(int argc, char **argv) {
    //test();
    printf("main() av_version_info = %s\n", av_version_info());
    printf("main() argc = %d\n", argc);
    for (int j = 0; j < argc; j++) {
        printf("main() argv[%d]: %s\n", j, argv[j]);
    }
    printf("------------------------------------------\n");

    printf("main()  display_disable = %d\n", display_disable);
    printf("main()    audio_disable = %d\n", audio_disable);
    printf("main()    video_disable = %d\n", video_disable);
    printf("main() subtitle_disable = %d\n", subtitle_disable);
    // --------------------------------------------------------SDL初始化
    int flags;
    if (display_disable) {
        video_disable = 1;
    }
    flags = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER;
    if (audio_disable)
        flags &= ~SDL_INIT_AUDIO;
    else {
        /* Try to work around an occasional ALSA buffer underflow issue when the
         * period size is NPOT due to ALSA resampling by forcing the buffer size. */
        if (!SDL_getenv("SDL_AUDIO_ALSA_SET_BUFFER_SIZE"))
            SDL_setenv("SDL_AUDIO_ALSA_SET_BUFFER_SIZE", "1", 1);
    }
    if (display_disable)
        flags &= ~SDL_INIT_VIDEO;
    if (SDL_Init(flags)) {
        av_log(nullptr, AV_LOG_FATAL, "Could not initialize SDL - %s\n", SDL_GetError());
        av_log(nullptr, AV_LOG_FATAL, "(Did you set the DISPLAY variable?)\n");
        exit(1);
    }

    SDL_EventState(SDL_SYSWMEVENT, SDL_IGNORE);
    SDL_EventState(SDL_USEREVENT, SDL_IGNORE);

    if (!display_disable) {
        int flags = SDL_WINDOW_HIDDEN;
        if (alwaysontop)
#if SDL_VERSION_ATLEAST(2, 0, 5)
            flags |= SDL_WINDOW_ALWAYS_ON_TOP;
#else
            av_log(nullptr, AV_LOG_WARNING,
                   "Your SDL version doesn't support SDL_WINDOW_ALWAYS_ON_TOP. Feature will be inactive.\n");
#endif
        if (borderless)
            flags |= SDL_WINDOW_BORDERLESS;
        else
            flags |= SDL_WINDOW_RESIZABLE;
        window = SDL_CreateWindow(program_name, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, default_width,
                                  default_height, flags);
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
        if (window) {
            renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
            if (!renderer) {
                av_log(nullptr, AV_LOG_WARNING, "Failed to initialize a hardware accelerated renderer: %s\n",
                       SDL_GetError());
                renderer = SDL_CreateRenderer(window, -1, 0);
            }
            if (renderer) {
                if (!SDL_GetRendererInfo(renderer, &renderer_info))
                    av_log(nullptr, AV_LOG_VERBOSE, "Initialized %s renderer.\n", renderer_info.name);
            }
        }
        if (!window || !renderer || !renderer_info.num_texture_formats) {
            av_log(nullptr, AV_LOG_FATAL, "Failed to create window or renderer: %s", SDL_GetError());
            do_exit(nullptr);
        }
    }
    // --------------------------------------------------------

    init_dynload();
    av_log_set_flags(AV_LOG_SKIP_REPEATED);
    parse_loglevel(argc, argv, options);
    /* register all codecs, demux and protocols */
#if CONFIG_AVDEVICE
    avdevice_register_all();
#endif
    avformat_network_init();
    init_opts();
    show_banner(argc, argv, options);
    parse_options(nullptr, argc, argv, options, opt_input_file);

    signal(SIGINT, sigterm_handler); /* Interrupt (ANSI).    */
    signal(SIGTERM, sigterm_handler); /* Termination (ANSI).  */

    input_filename = "https://zb3.qhqsnedu.com/live/chingyinglam/playlist.m3u8";
    input_filename = "https://meiju10.qhqsnedu.com/20200215/K9dFB7dW/3000kb/hls/index.m3u8";
    input_filename = "https://fangao.qhqsnedu.com/video/20190901/89cc34d4345d4a989ebebccc0ba8c1e8/cloudv-transfer/5555555526nso9o25556p16530pp8o3r_9774f5a8e6d5485f86c8f722492933b2_0_3.m3u8";
    input_filename = "https://meiju4.qhqsnedu.com/20190210/0OJRDGal/2000kb/hls/index.m3u8";
    input_filename = "/Users/alexander/Music/music/谁在意我留下的泪.mp3";
    input_filename = "/Users/alexander/Downloads/video.mp4";
    input_filename = "/Users/alexander/Downloads/千千阙歌.mp4";
    input_filename = "https://meiju9.qhqsnedu.com/20190823/1RSrZA26/2000kb/hls/index.m3u8";
    input_filename = "https://meiju.qhqsnedu.com/20181202/zbUvAw69/2000kb/hls/index.m3u8";
    input_filename = "https://cdn1.ibizastream.biz:441/free/1/playlist_dvr.m3u8";// *
    input_filename = "/Users/alexander/Movies/Movies/广告-20200511135626.h264";
    input_filename = "/Users/alexander/Downloads/小品-吃面.mp4";
    input_filename = "https://fangao.qhqsnedu.com/video/20190901/88c29da8beab47778c7329ec9444a9a4/cloudv-transfer/55555555ps61060q5556p165341q8o3r_f533a63031c74bbdb159da0479f79482_0_3.m3u8";
    input_filename = "http://ivi.bupt.edu.cn/hls/cctv6hd.m3u8";// CCTV-6综合高清
    input_filename = "https://zb3.qhqsnedu.com/live/chingyinglam/playlist.m3u8";// 林正英
    input_filename = "https://meiju5.qhqsnedu.com/20190612/Zg1IVNGE/2000kb/hls/index.m3u8";
    input_filename = "http://ivi.bupt.edu.cn/hls/cctv1hd.m3u8";// CCTV-1综合高清
    input_filename = "http://101.72.196.41/r/baiducdnct.inter.iqiyi.com/tslive/c16_lb_huaijiujuchang_1080p_t10/c16_lb_huaijiujuchang_1080p_t10.m3u8";
    input_filename = "/Users/alexander/Movies/AQUAMAN_Trailer_2_4K_ULTRA_HD_NEW2018.webm";
    input_filename = "/root/视频/心愿.mp4";
    input_filename = "/root/视频/tomcat_video/test.mp4";
    input_filename = "/Users/v_wangliwei/Movies/动态修改UI演示.mov";
    input_filename = "http://183.207.248.71:80/cntv/live1/CCTV-1/cctv-6";
    if (!input_filename) {
        show_usage();
        av_log(nullptr, AV_LOG_FATAL, "An input file must be specified\n");
        av_log(nullptr, AV_LOG_FATAL, "Use -h to get full help or, even better, run 'man %s'\n", program_name);
        do_exit(nullptr);
    }

    av_init_packet(&flush_pkt);
    flush_pkt.data = (uint8_t *) &flush_pkt;

    // 开始干活
    VideoState *is;
    is = stream_open(input_filename, file_iformat);
    if (!is) {
        av_log(nullptr, AV_LOG_FATAL, "Failed to initialize VideoState!\n");
        do_exit(nullptr);
    }

    event_loop(is);

    /* never returns */

    printf("main() game over\n");
    return 0;
}

typedef struct Wrapper {
    int age;
    char *name;
};

void test() {
    Wrapper *wrapper1 = nullptr;
    Wrapper *wrapper2 = nullptr;
    wrapper1 = static_cast<Wrapper *>(av_mallocz(sizeof(Wrapper)));
    wrapper2 = static_cast<Wrapper *>(av_mallocz(sizeof(Wrapper)));
    memset(wrapper1, 0, sizeof(Wrapper));
    memset(wrapper2, 0, sizeof(Wrapper));
    wrapper1->age = 30;
    wrapper1->name = "Mama";
    wrapper2->age = 35;
    wrapper2->name = "Baba";

    printf("test() before wrapper1: %p\n", wrapper1);
    printf("test() before wrapper2: %p\n", wrapper2);
    printf("test() before wrapper1->age: %d, wrapper1->name: %s\n", wrapper1->age, wrapper1->name);
    printf("test() before wrapper2->age: %d, wrapper2->name: %s\n", wrapper2->age, wrapper2->name);

    Wrapper *tempWrapper = nullptr;
    tempWrapper = wrapper1;
    wrapper1 = wrapper2;
    wrapper2 = tempWrapper;

    printf("test() after  wrapper1: %p\n", wrapper1);
    printf("test() after  wrapper2: %p\n", wrapper2);
    printf("test() after  wrapper1->age: %d, wrapper1->name: %s\n", wrapper1->age, wrapper1->name);
    printf("test() after  wrapper2->age: %d, wrapper2->name: %s\n", wrapper2->age, wrapper2->name);

    av_free(wrapper1);
    av_free(wrapper2);
}