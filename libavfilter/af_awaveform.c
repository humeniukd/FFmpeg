/*
 * Copyright (c) 2019 Dmytro Humeniuk
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
 * waveform audio filter – dump waveform data to a file
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

typedef struct AWaveformContext {
    const AVClass *class;   /**< class for AVOptions */
    int width;              /**< number of data values */
    int idx;                /**< index of current value */
    char *filename;         /**< output filename */
    float *values;          /**< scaling factors */
    int64_t nb_samples;     /**< samples per value per channel */
    int64_t count;          /**< current number of samples counted */
    int64_t frame_size;     /**< samples per value */
    double sum;             /**< sum of the squared samples per value */
    double max_value;       /**< keep track of max value */
    FILE *dump_fp;
} AWaveformContext;

#define OFFSET(x) offsetof(AWaveformContext, x)
#define FLAGS AV_OPT_FLAG_AUDIO_PARAM|AV_OPT_FLAG_FILTERING_PARAM

static const AVOption awaveform_options[] = {
    { "w", "set number of data values",  OFFSET(width), AV_OPT_TYPE_INT,  {.i64 = 1800}, 1, INT_MAX, FLAGS },
    { "width", "set number of data values",  OFFSET(width), AV_OPT_TYPE_INT,  {.i64 = 1800}, 1, INT_MAX, FLAGS },
    { "n", "set number of samples per value per channel",  OFFSET(nb_samples), AV_OPT_TYPE_INT64,  {.i64 = 1}, 1, INT64_MAX, FLAGS },
    { "nb_samples", "set number of samples per value per channel",  OFFSET(nb_samples), AV_OPT_TYPE_INT64,  {.i64 = 1}, 1, INT64_MAX, FLAGS },
    { "f", "set dump file", OFFSET(filename), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, FLAGS },
    { "file", "set dump file", OFFSET(filename), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(awaveform);

static int init(AVFilterContext *ctx)
{
    AWaveformContext *awaveform = ctx->priv;

    awaveform->sum = awaveform->idx = awaveform->count = 0;

    avfilter_graph_set_auto_convert(ctx->graph, AVFILTER_AUTO_CONVERT_ALL);

    if (!awaveform->filename) {
        av_log(ctx, AV_LOG_ERROR, "No output file provided\n");
        return AVERROR(EINVAL);
    } else if (!strcmp("-", awaveform->filename)) {
        awaveform->dump_fp = stdout;
    } else {
        awaveform->dump_fp = fopen(awaveform->filename, "w");
        if (!awaveform->dump_fp) {
            int err = AVERROR(errno);
            char buf[128];
            av_strerror(err, buf, sizeof(buf));
            av_log(ctx, AV_LOG_ERROR, "Could not open file %s: %s\n",
                   awaveform->filename, buf);
            return err;
        }
    }
    return 0;
}

static void uninit(AVFilterContext *ctx)
{
    AWaveformContext *awaveform = ctx->priv;
    fclose(awaveform->dump_fp);
    av_freep(&awaveform->values);
}

static int config_input(AVFilterLink *inlink)
{
    AWaveformContext *awaveform = inlink->dst->priv;
    awaveform->frame_size = awaveform->nb_samples * inlink->ch_layout.nb_channels;

    return 0;
}

static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    AWaveformContext *awaveform = ctx->priv;
    awaveform->values = av_malloc(awaveform->width * sizeof(float));
    if (!awaveform->values)
        return AVERROR(ENOMEM);
    memset(awaveform->values, 0, awaveform->width * sizeof(float));

    return 0;
}

static int request_frame(AVFilterLink *outlink) {
    AVFilterContext *ctx = outlink->src;
    AWaveformContext *awaveform = ctx->priv;

    AVFilterLink *inlink = ctx->inputs[0];

    int ret = ff_request_frame(inlink);

    if (ret == AVERROR_EOF)
        for (int i = 0; i < awaveform->width; i++)
            fprintf(awaveform->dump_fp, (i == awaveform->width - 1) ? "%f\n" : "%f,",
                    av_clipf(awaveform->values[i] / awaveform->max_value, 0, 1.0));
    return ret;
}

/**
 * Convert sample to dB and calculate root mean squared value
 */
static inline void RMS(AWaveformContext *awaveform, const float sample)
{
    double value = 0.;
    if (sample != 0)
        value = (20. * log10(fabs(sample)) + 60.) / 60.;

    awaveform->sum += value * value;

    if (awaveform->count++ == awaveform->frame_size) {
        value = av_clipd(sqrt(awaveform->sum / awaveform->frame_size), 0, 1.0);
        awaveform->max_value = FFMAX(value, awaveform->max_value);
        awaveform->values[awaveform->idx++] = value;
        awaveform->sum = awaveform->count = 0;
    }
}

static int filter_frame(AVFilterLink *inlink, AVFrame *frame)
{
    AVFilterContext *ctx = inlink->dst;
    AWaveformContext *awaveform = ctx->priv;
    int channels = frame->ch_layout.nb_channels;
    int c, i;

    if (awaveform->idx < awaveform->width)
        switch (inlink->format) {
            case AV_SAMPLE_FMT_FLTP:
                for (c = 0; c < channels; c++) {
                    const float *src = (const float *)frame->extended_data[c];

                    for (i = 0; i < frame->nb_samples; i++, src++)
                        RMS(awaveform, *src);
                }
                break;
            case AV_SAMPLE_FMT_FLT: {
                const float *src = (const float *)frame->extended_data[0];

                for (i = 0; i < frame->nb_samples; i++) {
                    for (c = 0; c < channels; c++, src++)
                        RMS(awaveform, *src);
                }

            } break;
        }
    return ff_filter_frame(inlink->dst->outputs[0], frame);
}

static const AVFilterPad awaveform_inputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_AUDIO,
        .filter_frame  = filter_frame,
        .config_props  = config_input
    },
};

static const AVFilterPad awaveform_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_AUDIO,
        .request_frame = request_frame,
        .config_props  = config_output
    },
};

AVFilter ff_af_awaveform = {
    .name          = "awaveform",
    .description   = NULL_IF_CONFIG_SMALL("Dump waveform data to a file"),
    .init          = init,
    .uninit        = uninit,
    .priv_size     = sizeof(AWaveformContext),
    .priv_class    = &awaveform_class,
    FILTER_INPUTS(awaveform_inputs),
    FILTER_OUTPUTS(awaveform_outputs),
    FILTER_SAMPLEFMTS(AV_SAMPLE_FMT_FLT, AV_SAMPLE_FMT_FLTP, AV_SAMPLE_FMT_NONE),
};
