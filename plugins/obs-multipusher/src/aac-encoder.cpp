#include "aac-encoder.hpp"

#include <obs-module.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
}

#include <cstring>
#include <vector>

namespace {

struct MpAacEncoder {
    obs_encoder_t *encoder = nullptr;
    AVCodecContext *ctx = nullptr;
    AVFrame *frame = nullptr;
    AVPacket *pkt = nullptr;
    int64_t totalSamples = 0;
    int frameSize = 1024;
    std::vector<uint8_t> buffer;
};

const char *mp_aac_get_name(void *)
{
    return "AAC (multipusher)";
}

void mp_aac_destroy(void *data)
{
    auto *enc = static_cast<MpAacEncoder *>(data);
    if (!enc)
        return;
    if (enc->frame)
        av_frame_free(&enc->frame);
    if (enc->pkt)
        av_packet_free(&enc->pkt);
    if (enc->ctx)
        avcodec_free_context(&enc->ctx);
    delete enc;
}

void *mp_aac_create(obs_data_t *settings, obs_encoder_t *encoder)
{
    const AVCodec *codec = avcodec_find_encoder_by_name("aac");
    if (!codec)
        codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec) {
        blog(LOG_WARNING, "[obs-multipusher] mp_aac: bundled FFmpeg has no AAC encoder");
        return nullptr;
    }

    auto *enc = new MpAacEncoder();
    enc->encoder = encoder;
    enc->ctx = avcodec_alloc_context3(codec);
    if (!enc->ctx) {
        delete enc;
        return nullptr;
    }

    audio_t *audio = obs_encoder_audio(encoder);
    int bitrate = settings ? static_cast<int>(obs_data_get_int(settings, "bitrate")) : 128;
    if (bitrate <= 0)
        bitrate = 128;

    enc->ctx->bit_rate = bitrate * 1000;
    enc->ctx->sample_rate = audio ? static_cast<int>(audio_output_get_sample_rate(audio)) : 48000;

    int channels = audio ? static_cast<int>(audio_output_get_channels(audio)) : 2;
    if (channels <= 0)
        channels = 2;
    av_channel_layout_default(&enc->ctx->ch_layout, channels);

    // The native FFmpeg AAC encoder only supports planar float.
    enc->ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
    enc->ctx->time_base = AVRational{1, enc->ctx->sample_rate};
    enc->ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if (avcodec_open2(enc->ctx, codec, nullptr) < 0) {
        blog(LOG_WARNING, "[obs-multipusher] mp_aac: avcodec_open2 failed");
        avcodec_free_context(&enc->ctx);
        delete enc;
        return nullptr;
    }

    enc->frame = av_frame_alloc();
    enc->pkt = av_packet_alloc();
    if (!enc->frame || !enc->pkt) {
        mp_aac_destroy(enc);
        return nullptr;
    }

    enc->frameSize = enc->ctx->frame_size > 0 ? enc->ctx->frame_size : 1024;
    enc->frame->format = enc->ctx->sample_fmt;
    enc->frame->ch_layout = enc->ctx->ch_layout;
    enc->frame->sample_rate = enc->ctx->sample_rate;
    enc->frame->nb_samples = enc->frameSize;
    if (av_frame_get_buffer(enc->frame, 0) < 0) {
        blog(LOG_WARNING, "[obs-multipusher] mp_aac: av_frame_get_buffer failed");
        mp_aac_destroy(enc);
        return nullptr;
    }

    blog(LOG_INFO, "[obs-multipusher] mp_aac created: %d Hz, %d ch, %d kbps",
         enc->ctx->sample_rate, channels, bitrate);
    return enc;
}

bool mp_aac_encode(void *data, struct encoder_frame *frame, struct encoder_packet *packet,
                   bool *received_packet)
{
    auto *enc = static_cast<MpAacEncoder *>(data);
    if (received_packet)
        *received_packet = false;
    if (!enc || !enc->frame || !enc->ctx || !frame)
        return false;

    const int planes = av_sample_fmt_is_planar(enc->ctx->sample_fmt)
                           ? enc->ctx->ch_layout.nb_channels
                           : 1;
    for (int i = 0; i < planes && i < MAX_AV_PLANES; i++) {
        if (!frame->data[i])
            continue;
        const size_t bytes = frame->linesize[i];
        if (static_cast<int>(bytes) > enc->frame->linesize[i]) {
            blog(LOG_WARNING, "[obs-multipusher] mp_aac: frame too large (%zu > %d)",
                 bytes, enc->frame->linesize[i]);
            return false;
        }
        memcpy(enc->frame->extended_data[i], frame->data[i], bytes);
    }
    enc->frame->nb_samples = static_cast<int>(frame->frames);
    enc->frame->pts = av_rescale_q(enc->totalSamples, AVRational{1, enc->ctx->sample_rate},
                                   enc->ctx->time_base);

    int ret = avcodec_send_frame(enc->ctx, enc->frame);
    if (ret < 0) {
        blog(LOG_WARNING, "[obs-multipusher] mp_aac: send_frame failed: %d", ret);
        return false;
    }
    enc->totalSamples += frame->frames;

    ret = avcodec_receive_packet(enc->ctx, enc->pkt);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        return true;
    if (ret < 0) {
        blog(LOG_WARNING, "[obs-multipusher] mp_aac: receive_packet failed: %d", ret);
        return false;
    }

    enc->buffer.assign(enc->pkt->data, enc->pkt->data + enc->pkt->size);
    packet->data = enc->buffer.data();
    packet->size = enc->buffer.size();
    packet->pts = av_rescale_q(enc->pkt->pts, enc->ctx->time_base,
                               AVRational{1, enc->ctx->sample_rate});
    packet->dts = packet->pts;
    packet->timebase_num = 1;
    packet->timebase_den = static_cast<int32_t>(enc->ctx->sample_rate);
    packet->type = OBS_ENCODER_AUDIO;
    packet->keyframe = true;
    av_packet_unref(enc->pkt);
    if (received_packet)
        *received_packet = true;
    return true;
}

void mp_aac_defaults(obs_data_t *settings)
{
    obs_data_set_default_int(settings, "bitrate", 128);
}

bool mp_aac_extra_data(void *data, uint8_t **extra_data, size_t *size)
{
    auto *enc = static_cast<MpAacEncoder *>(data);
    if (!enc || !enc->ctx)
        return false;
    *extra_data = enc->ctx->extradata;
    *size = static_cast<size_t>(enc->ctx->extradata_size);
    return true;
}

void mp_aac_audio_info(void *data, struct audio_convert_info *info)
{
    auto *enc = static_cast<MpAacEncoder *>(data);
    if (!enc || !enc->ctx)
        return;
    info->format = AUDIO_FORMAT_FLOAT_PLANAR;
    info->samples_per_sec = static_cast<uint32_t>(enc->ctx->sample_rate);
    const int channels = enc->ctx->ch_layout.nb_channels;
    if (channels > 0 && channels <= 8 && channels != 7)
        info->speakers = static_cast<enum speaker_layout>(channels);
    else
        info->speakers = SPEAKERS_UNKNOWN;
}

size_t mp_aac_frame_size(void *data)
{
    auto *enc = static_cast<MpAacEncoder *>(data);
    return enc ? static_cast<size_t>(enc->frameSize) : 1024;
}

uint32_t mp_aac_priming(void *data)
{
    auto *enc = static_cast<MpAacEncoder *>(data);
    return (enc && enc->ctx) ? static_cast<uint32_t>(enc->ctx->initial_padding) : 0;
}

} // namespace

void RegisterMultipusherAacEncoder()
{
    obs_encoder_info info = {};
    info.id = "mp_aac";
    info.type = OBS_ENCODER_AUDIO;
    info.codec = "aac";
    info.get_name = mp_aac_get_name;
    info.create = mp_aac_create;
    info.destroy = mp_aac_destroy;
    info.encode = mp_aac_encode;
    info.get_frame_size = mp_aac_frame_size;
    info.get_defaults = mp_aac_defaults;
    info.get_extra_data = mp_aac_extra_data;
    info.get_audio_info = mp_aac_audio_info;
    info.get_priming_samples = mp_aac_priming;
    info.caps = OBS_ENCODER_CAP_INTERNAL;
    obs_register_encoder(&info);
    blog(LOG_INFO, "[obs-multipusher] registered internal AAC encoder: mp_aac");
}
