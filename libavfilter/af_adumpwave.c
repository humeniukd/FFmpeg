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

#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/channel_layout.h"
#include "libavutil/opt.h"
#include "libavutil/parseutils.h"
#include "avfilter.h"
#include "formats.h"
#include "audio.h"
#include "filters.h"
#include "libavutil/mem.h"

typedef struct ADumpwaveContext {
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
} ADumpwaveContext;

#define OFFSET(x) offsetof(ADumpwaveContext, x)
#define FLAGS AV_OPT_FLAG_AUDIO_PARAM|AV_OPT_FLAG_FILTERING_PARAM

static const AVOption adumpwave_options[] = {
    { "w", "set number of data values",  OFFSET(width), AV_OPT_TYPE_INT,  {.i64 = 1800}, 1, INT_MAX, FLAGS },
    { "width", "set number of data values",  OFFSET(width), AV_OPT_TYPE_INT,  {.i64 = 1800}, 1, INT_MAX, FLAGS },
    { "n", "set number of samples per value per channel",  OFFSET(nb_samples), AV_OPT_TYPE_INT64,  {.i64 = 1}, 1, INT64_MAX, FLAGS },
    { "nb_samples", "set number of samples per value per channel",  OFFSET(nb_samples), AV_OPT_TYPE_INT64,  {.i64 = 1}, 1, INT64_MAX, FLAGS },
    { "f", "set dump file", OFFSET(filename), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, FLAGS },
    { "file", "set dump file", OFFSET(filename), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(adumpwave);

static av_cold int init(AVFilterContext *ctx)
{
    ADumpwaveContext *adumpwave = ctx->priv;

    adumpwave->sum = adumpwave->idx = adumpwave->count = 0;

    avfilter_graph_set_auto_convert(ctx->graph, AVFILTER_AUTO_CONVERT_ALL);

    if (!adumpwave->filename) {
        av_log(ctx, AV_LOG_ERROR, "No output file provided\n");
        return AVERROR(EINVAL);
    } else if (!strcmp("-", adumpwave->filename)) {
        adumpwave->dump_fp = stdout;
    } else {
        adumpwave->dump_fp = fopen(adumpwave->filename, "w");
        if (!adumpwave->dump_fp) {
            int err = AVERROR(errno);
            char buf[128];
            av_strerror(err, buf, sizeof(buf));
            av_log(ctx, AV_LOG_ERROR, "Could not open file %s: %s\n",
                   adumpwave->filename, buf);
            return err;
        }
    }
    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    ADumpwaveContext *adumpwave = ctx->priv;
    fclose(adumpwave->dump_fp);
    av_freep(&adumpwave->values);
}

static int config_input(AVFilterLink *inlink)
{
    ADumpwaveContext *adumpwave = inlink->dst->priv;
    adumpwave->frame_size = adumpwave->nb_samples * inlink->ch_layout.nb_channels;

    return 0;
}

static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    ADumpwaveContext *adumpwave = ctx->priv;
    adumpwave->values = av_malloc(adumpwave->width * sizeof(float));
    if (!adumpwave->values)
        return AVERROR(ENOMEM);
    memset(adumpwave->values, 0, adumpwave->width * sizeof(float));

    return 0;
}

static int request_frame(AVFilterLink *outlink) {
    AVFilterContext *ctx = outlink->src;
    ADumpwaveContext *adumpwave = ctx->priv;

    AVFilterLink *inlink = ctx->inputs[0];

    int ret = ff_request_frame(inlink);

    if (ret == AVERROR_EOF)
        for (int i = 0; i < adumpwave->width; i++)
            fprintf(adumpwave->dump_fp, (i == adumpwave->width - 1) ? "%f\n" : "%f,",
                    av_clipf(adumpwave->values[i] / adumpwave->max_value, 0, 1.0));
    return ret;
}

/**
 * Convert sample to dB and calculate root mean squared value
 */
static void RMS(ADumpwaveContext *adumpwave, const float sample) {
    double value = 0.;
    if (sample != 0)
        value = (20. * log10(fabs(sample)) + 60.) / 60.;

    adumpwave->sum += value * value;

    if (adumpwave->count++ == adumpwave->frame_size) {
        value = av_clipd(sqrt(adumpwave->sum / adumpwave->frame_size), 0, 1.0);
        adumpwave->max_value = FFMAX(value, adumpwave->max_value);
        adumpwave->values[adumpwave->idx++] = value;
        adumpwave->sum = adumpwave->count = 0;
    }
}

static int filter_frame(AVFilterLink *inlink, AVFrame *frame)
{
    AVFilterContext *ctx = inlink->dst;
    ADumpwaveContext *adumpwave = ctx->priv;
    int channels = frame->ch_layout.nb_channels;
    int c, i;

    if (adumpwave->idx < adumpwave->width)
        switch (inlink->format) {
            case AV_SAMPLE_FMT_FLTP:
                for (c = 0; c < channels; c++) {
                    const float *src = (const float *)frame->extended_data[c];

                    for (i = 0; i < frame->nb_samples; i++, src++)
                        RMS(adumpwave, *src);
                }
                break;
            case AV_SAMPLE_FMT_FLT: {
                const float *src = (const float *)frame->extended_data[0];

                for (i = 0; i < frame->nb_samples; i++) {
                    for (c = 0; c < channels; c++, src++)
                        RMS(adumpwave, *src);
                }

            } break;
        }
    return ff_filter_frame(inlink->dst->outputs[0], frame);
}

static const AVFilterPad adumpwave_inputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_AUDIO,
        .filter_frame  = filter_frame,
        .config_props  = config_input
    },
};

static const AVFilterPad adumpwave_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_AUDIO,
        .request_frame = request_frame,
        .config_props  = config_output
    },
};

const FFFilter ff_af_adumpwave = {
    .p.name        = "adumpwave",
    .p.description = NULL_IF_CONFIG_SMALL("Dump waveform data to csv file"),
    .init          = init,
    .uninit        = uninit,
    .p.flags       = AVFILTER_FLAG_SUPPORT_TIMELINE_INTERNAL,
    .priv_size     = sizeof(ADumpwaveContext),
    .p.priv_class  = &adumpwave_class,
    FILTER_INPUTS(adumpwave_inputs),
    FILTER_OUTPUTS(adumpwave_outputs),
    FILTER_SAMPLEFMTS(AV_SAMPLE_FMT_FLT, AV_SAMPLE_FMT_FLTP),
};
