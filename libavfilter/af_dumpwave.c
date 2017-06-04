/*
 * Copyright (c) 2017 Dmytro Humeniuk
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
 * audio filter
 */

#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/channel_layout.h"
#include "libavutil/opt.h"
#include "libavutil/parseutils.h"
#include "avfilter.h"
#include "formats.h"
#include "audio.h"
#include "internal.h"

enum DumpWavesScale {
    SCALE_LIN,
    SCALE_LOG,
    SCALE_SQRT,
    SCALE_CBRT,
    SCALE_NB,
};

struct frame_node {
    AVFrame *frame;
    struct frame_node *next;
};

typedef struct DumpWaveContext {
    const AVClass *class;
    int w, h, c;
    AVRational rate;
    int scale, col;                  ///< DumpWaveScale
    char *json;
    char *str;
    double *values;
    int64_t n;
    double max; /* peak of the samples per channel */
    double sum; /* abs sum of the samples per channel */
} DumpWaveContext;

#define OFFSET(x) offsetof(DumpWaveContext, x)
#define FLAGS AV_OPT_FLAG_AUDIO_PARAM|AV_OPT_FLAG_FILTERING_PARAM

static const AVOption dumpwave_options[] = {
    { "s",    "set dump size", OFFSET(w), AV_OPT_TYPE_IMAGE_SIZE, {.str = "600x240"}, 0, 0, FLAGS },
    { "c", "set number of samples per item",  OFFSET(c), AV_OPT_TYPE_INT64,  {.i64 = 0}, 0, INT64_MAX, FLAGS },
    { "json", "set dump file", OFFSET(json), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(dumpwave);

static av_cold int init(AVFilterContext *ctx)
{
    DumpWaveContext *dumpwave = ctx->priv;
    const int ch_width = dumpwave->w;
    dumpwave->values = av_malloc(ch_width * sizeof(double));
    dumpwave->str = av_malloc(ch_width*4);
    
    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    DumpWaveContext *dumpwave = ctx->priv;
    const int ch_height = dumpwave->h;
    const int ch_width = dumpwave->w;
    char *result = dumpwave->str;
    FILE *dump_fp = NULL;
    
    if (dumpwave->json && !(dump_fp = av_fopen_utf8(dumpwave->json, "w")))
        av_log(ctx, AV_LOG_WARNING, "dumping failed.\n");
    
    if (dump_fp) {
        fprintf(dump_fp, "{\"width\":%d,\"height\":%d,\"samples\":[%s]}", ch_width, ch_height, result);
        fclose(dump_fp);
    }

    av_freep(dumpwave->values);
//    av_freep(dumpwave->str);
}

static int query_formats(AVFilterContext *ctx)
{
    AVFilterFormats *formats = NULL;
    AVFilterChannelLayouts *layouts = NULL;
    AVFilterLink *inlink = ctx->inputs[0];
    AVFilterLink *outlink = ctx->outputs[0];
    static const enum AVSampleFormat sample_fmts[] = { AV_SAMPLE_FMT_S16 };
    int ret;

    /* set input audio formats */
    formats = ff_make_format_list(sample_fmts);
    if ((ret = ff_formats_ref(formats, &inlink->out_formats)) < 0)
        return ret;

    layouts = ff_all_channel_layouts();
    if ((ret = ff_channel_layouts_ref(layouts, &inlink->out_channel_layouts)) < 0)
        return ret;

    formats = ff_all_samplerates();
    if ((ret = ff_formats_ref(formats, &inlink->out_samplerates)) < 0)
        return ret;

    /* set output audio format */
    formats = ff_make_format_list(sample_fmts);
    if ((ret = ff_formats_ref(formats, &outlink->in_formats)) < 0)
        return ret;
    
    layouts = ff_all_channel_layouts();
    if ((ret = ff_channel_layouts_ref(layouts, &outlink->in_channel_layouts)) < 0)
        return ret;
    
    formats = ff_all_samplerates();
    if ((ret = ff_formats_ref(formats, &outlink->in_samplerates)) < 0)
        return ret;

    return 0;
}


static int dumpwave_request_frame(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    DumpWaveContext *dumpwave = ctx->priv;
    const int ch_height = dumpwave->h;
    const int ch_width = dumpwave->w;
    const double max = dumpwave->max;
    const double *values = dumpwave->values;
    char *result = dumpwave->str;
    
    double rmsSize;
    int res = 0;
    
    AVFilterLink *inlink = ctx->inputs[0];
    int ret;

    ret = ff_request_frame(inlink);
    if (ret == AVERROR_EOF) {

        char *pos = result;

        for(int i = 0; i < ch_width; i++) {
            rmsSize = exp(values[i] / (double) max * M_E - M_E);
            res = ch_height * rmsSize;
            pos += sprintf(pos, "%d,", res);
        }
        pos[-1] = '\0';
    }

    return ret;
}

static int dumpwave_filter_frame(AVFilterLink *inlink, AVFrame *frame)
{
    AVFilterContext *ctx = inlink->dst;
    DumpWaveContext *dumpwave = ctx->priv;
    const int ch_height = dumpwave->h;
    const int ch_width = dumpwave->w;
    const int nb_channels = inlink->channels;
    const int16_t nb_samples = frame->nb_samples;
    const int64_t max_samples = dumpwave->c;
    double *sum = &dumpwave->sum;
    double *max = &dumpwave->max;
    int *col = &dumpwave->col;
    int64_t *n = &dumpwave->n;
    double *values = dumpwave->values;
    int ret = 0;
    int16_t s16 = 0;
    float smpl = 0;

    int i;

    const int16_t *p = (const int16_t *)frame->data[0];
    
    for (i = 0; i < nb_samples; i++) {
        s16 = p[i*nb_channels];
        smpl = s16/(float)INT16_MAX;
        if (smpl != 0.)
        {
            float db = 20*log10(fabs(smpl));
            smpl = (db + 60) / 60;
            if (smpl < 0.0)
            {
                smpl = 0.0;
            }
        }
        *sum += smpl*smpl;
        
        if ((*n)++ == max_samples) {
            double sample = sqrt(*sum / max_samples);
            
            av_assert0(*col < ch_width);
            values[*col] = sample;
            
            *sum = 0;
            *max = FFMAX(*max, sample);
            (*col)++;
            *n = 0;
        }
    }
    ret = ff_filter_frame(ctx->outputs[0], frame);

    return 0;

end:
    av_frame_free(&frame);
    return ret;
}

static const AVFilterPad dumpwave_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_AUDIO,
        .filter_frame = dumpwave_filter_frame,
    },
    { NULL }
};

static const AVFilterPad dumpwave_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_AUDIO,
        .request_frame = dumpwave_request_frame,
    },
    { NULL }
};

AVFilter ff_af_dumpwave = {
    .name          = "dumpwave",
    .description   = NULL_IF_CONFIG_SMALL("Convert input audio to a video output single picture."),
    .init          = init,
    .uninit        = uninit,
    .query_formats = query_formats,
    .priv_size     = sizeof(DumpWaveContext),
    .inputs        = dumpwave_inputs,
    .outputs       = dumpwave_outputs,
    .priv_class    = &dumpwave_class,
};
