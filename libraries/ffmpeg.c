// SPDX-License-Identifier: GPL-3.0-or-later
//
// ffmpeg.c — FFmpeg (avformat/avcodec/swresample/swscale) bindings for Vitte
// Light VM (C17, complete) Namespace: "ffmpeg"
//
// Build:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -DVL_HAVE_FFMPEG -c ffmpeg.c
//   cc ... ffmpeg.o -lavformat -lavcodec -lavutil -lswresample -lswscale
//
// Portability:
//   - Real implementation if VL_HAVE_FFMPEG and FFmpeg headers are available.
//   - Otherwise: stubs -> (nil,"ENOSYS").
//
// Model:
//   - One handle id == one opened input (file/URL).
//   - Decoders created for audio/video streams. Output formats:
//       * Audio frames: interleaved float32 PCM (sample_rate, channels,
//       nb_samples).
//       * Video frames: RGB24 (width * height * 3 bytes).
//   - Stream list is returned as USV rows (fields 0x1F, rows 0x1E).
//
// API:
//   ffmpeg.version() -> string                     // "libavformat:X
//   libavcodec:Y libavutil:Z ..." ffmpeg.open_input(path_or_url) -> id |
//   (nil,errmsg) ffmpeg.streams(id) -> usv:string | (nil,errmsg) // rows:
//   idx,kind,codec,w,h,pixfmt,sr,ch,fmt,dur_ts,time_base,avg_fps
//   ffmpeg.read_packet(id) -> sid:int, pts_sec:float, key:int, data:string |
//   (nil,"eof") | (nil,errmsg) ffmpeg.decode_next(id[, want="av"]) ->
//        "audio", sid, pts_sec:float, sr:int, ch:int, nb_samples:int,
//        pcm_f32:string
//      | "video", sid, pts_sec:float, w:int, h:int, rgb24:string
//      | (nil,"eof") | (nil,errmsg)
//   ffmpeg.seek(id, seconds:number[, mode="any"]) -> true | (nil,errmsg)   //
//   mode: "any"|"backward"|"frame" ffmpeg.close(id) -> true
//
// Notes:
//   - Strings passed in are expected to have no NUL bytes (VM limitation).
//   Outputs use vl_push_lstring() and are binary-safe.
//   - decode_next() reads/decodes until one frame is produced from any A/V
//   stream (filtered by 'want': "a","v","av").
//
// Deps: auxlib.h, state.h, object.h, vm.h

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "auxlib.h"
#include "object.h"
#include "state.h"
#include "vm.h"

#ifdef VL_HAVE_FFMPEG
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#endif

#define US 0x1F
#define RS 0x1E

// ---------------------------------------------------------------------
// VM arg helpers
// ---------------------------------------------------------------------
static const char *ff_check_str(VL_State *S, int idx) {
  if (vl_get(S, idx) && vl_isstring(S, idx))
    return vl_tocstring(S, vl_get(S, idx));
  vl_errorf(S, "argument #%d: string expected", idx);
  vl_error(S);
  return NULL;
}
static int64_t ff_check_int(VL_State *S, int idx) {
  if (vl_get(S, idx) && (vl_isint(S, idx) || vl_isfloat(S, idx)))
    return vl_isint(S, idx) ? vl_toint(S, vl_get(S, idx))
                            : (int64_t)vl_tonumber(S, vl_get(S, idx));
  vl_errorf(S, "argument #%d: int expected", idx);
  vl_error(S);
  return 0;
}
static double ff_check_num(VL_State *S, int idx) {
  if (vl_get(S, idx) && (vl_isint(S, idx) || vl_isfloat(S, idx)))
    return vl_isint(S, idx) ? (double)vl_toint(S, vl_get(S, idx))
                            : vl_tonumber(S, vl_get(S, idx));
  vl_errorf(S, "argument #%d: number expected", idx);
  vl_error(S);
  return 0.0;
}
static const char *ff_opt_str(VL_State *S, int idx, const char *defv) {
  if (!vl_get(S, idx) || !vl_isstring(S, idx)) return defv;
  return ff_check_str(S, idx);
}

// ---------------------------------------------------------------------
// Stubs when FFmpeg is missing
// ---------------------------------------------------------------------
#ifndef VL_HAVE_FFMPEG

#define NOSYS_PAIR(S)            \
  do {                           \
    vl_push_nil(S);              \
    vl_push_string(S, "ENOSYS"); \
    return 2;                    \
  } while (0)

static int vlff_version(VL_State *S) {
  vl_push_string(S, "ffmpeg not built");
  return 1;
}
static int vlff_open(VL_State *S) {
  (void)ff_check_str(S, 1);
  NOSYS_PAIR(S);
}
static int vlff_streams(VL_State *S) { NOSYS_PAIR(S); }
static int vlff_read_packet(VL_State *S) { NOSYS_PAIR(S); }
static int vlff_decode_next(VL_State *S) { NOSYS_PAIR(S); }
static int vlff_seek(VL_State *S) { NOSYS_PAIR(S); }
static int vlff_close(VL_State *S) {
  vl_push_bool(S, 1);
  return 1;
}

#else
// ---------------------------------------------------------------------
// Real implementation
// ---------------------------------------------------------------------

typedef struct FFHandle {
  int used;
  AVFormatContext *fmt;
  AVCodecContext **dec;  // per stream or NULL
  SwrContext **swr;      // for audio streams or NULL
  SwsContext **sws;      // for video streams or NULL
  int nb;
  int inited_net;
} FFHandle;

static FFHandle *g_h = NULL;
static int g_h_cap = 0;
static int ensure_h_cap(int need) {
  if (need <= g_h_cap) return 1;
  int n = g_h_cap ? g_h_cap : 8;
  while (n < need) n <<= 1;
  FFHandle *nh = (FFHandle *)realloc(g_h, (size_t)n * sizeof *nh);
  if (!nh) return 0;
  for (int i = g_h_cap; i < n; i++) {
    nh[i].used = 0;
    nh[i].fmt = NULL;
    nh[i].dec = NULL;
    nh[i].swr = NULL;
    nh[i].sws = NULL;
    nh[i].nb = 0;
    nh[i].inited_net = 0;
  }
  g_h = nh;
  g_h_cap = n;
  return 1;
}
static int alloc_h(void) {
  for (int i = 1; i < g_h_cap; i++)
    if (!g_h[i].used) return i;
  if (!ensure_h_cap(g_h_cap ? g_h_cap * 2 : 8)) return 0;
  for (int i = 1; i < g_h_cap; i++)
    if (!g_h[i].used) return i;
  return 0;
}
static int chk_h(int id) {
  return id > 0 && id < g_h_cap && g_h[id].used && g_h[id].fmt;
}

static void free_handle(FFHandle *H) {
  if (!H || !H->used) return;
  if (H->dec) {
    for (int i = 0; i < H->nb; i++) {
      if (H->dec[i]) avcodec_free_context(&H->dec[i]);
    }
    free(H->dec);
    H->dec = NULL;
  }
  if (H->swr) {
    for (int i = 0; i < H->nb; i++) {
      if (H->swr[i]) {
        swr_free(&H->swr[i]);
      }
    }
    free(H->swr);
    H->swr = NULL;
  }
  if (H->sws) {
    for (int i = 0; i < H->nb; i++) {
      if (H->sws[i]) {
        sws_freeContext(H->sws[i]);
        H->sws[i] = NULL;
      }
    }
    free(H->sws);
    H->sws = NULL;
  }
  if (H->fmt) {
    avformat_close_input(&H->fmt);
    H->fmt = NULL;
  }
  H->nb = 0;
  H->used = 0;
}

static int push_averr(VL_State *S, int err, const char *fallback) {
  char buf[256];
  const char *msg = av_strerror(err, buf, sizeof buf) == 0 ? buf : fallback;
  vl_push_nil(S);
  vl_push_string(S, msg ? msg : "EIO");
  return 2;
}

static int vlff_version(VL_State *S) {
  char buf[256];
  snprintf(buf, sizeof buf,
           "libavformat:%u libavcodec:%u libavutil:%u swresample:%u swscale:%u",
           (unsigned)avformat_version(), (unsigned)avcodec_version(),
           (unsigned)avutil_version(), (unsigned)swresample_version(),
           (unsigned)swscale_version());
  vl_push_string(S, buf);
  return 1;
}

static int vlff_open(VL_State *S) {
  const char *url = ff_check_str(S, 1);
  int id = alloc_h();
  if (!id) {
    vl_push_nil(S);
    vl_push_string(S, "ENOMEM");
    return 2;
  }
  memset(&g_h[id], 0, sizeof g_h[id]);

  // network init once per process is fine; we keep a flag on handle to avoid
  // double finalize
  avformat_network_init();
  g_h[id].inited_net = 1;

  AVFormatContext *fmt = NULL;
  int err = avformat_open_input(&fmt, url, NULL, NULL);
  if (err < 0) {
    return push_averr(S, err, "open_input");
  }
  err = avformat_find_stream_info(fmt, NULL);
  if (err < 0) {
    avformat_close_input(&fmt);
    return push_averr(S, err, "stream_info");
  }

  int nb = (int)fmt->nb_streams;
  AVCodecContext **dec = (AVCodecContext **)calloc((size_t)nb, sizeof *dec);
  SwrContext **swr = (SwrContext **)calloc((size_t)nb, sizeof *swr);
  SwsContext **sws = (SwsContext **)calloc((size_t)nb, sizeof *sws);
  if (!dec || !swr || !sws) {
    if (dec) free(dec);
    if (swr) free(swr);
    if (sws) free(sws);
    avformat_close_input(&fmt);
    vl_push_nil(S);
    vl_push_string(S, "ENOMEM");
    return 2;
  }

  // Open decoders for audio/video streams
  for (int i = 0; i < nb; i++) {
    AVStream *st = fmt->streams[i];
    enum AVMediaType mt = st->codecpar->codec_type;
    if (mt != AVMEDIA_TYPE_AUDIO && mt != AVMEDIA_TYPE_VIDEO) continue;
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) continue;
    dec[i] = avcodec_alloc_context3(codec);
    if (!dec[i]) { /* best-effort */
      continue;
    }
    avcodec_parameters_to_context(dec[i], st->codecpar);
    dec[i]->pkt_timebase = st->time_base;
    if ((err = avcodec_open2(dec[i], codec, NULL)) < 0) {
      avcodec_free_context(&dec[i]);
      dec[i] = NULL;
      continue;
    }
    if (mt == AVMEDIA_TYPE_AUDIO) {
      // Prepare resampler to f32 interleaved
      int64_t src_layout =
          dec[i]->ch_layout.nb_channels
              ? dec[i]->ch_layout.u.mask
              : av_get_default_channel_layout(dec[i]->ch_layout.nb_channels);
      if (src_layout == 0 && dec[i]->ch_layout.nb_channels > 0)
        src_layout =
            av_get_default_channel_layout(dec[i]->ch_layout.nb_channels);
      enum AVSampleFormat src_fmt = dec[i]->sample_fmt;
      enum AVSampleFormat dst_fmt = AV_SAMPLE_FMT_FLT;  // interleaved float32
      int src_rate = dec[i]->sample_rate;
      int dst_rate = src_rate;
      uint64_t dst_layout = src_layout;
#if LIBAVUTIL_VERSION_MAJOR >= 57
      // New channel layout API already used above
#endif
      swr[i] =
          swr_alloc_set_opts(NULL, (int64_t)dst_layout, dst_fmt, dst_rate,
                             (int64_t)src_layout, src_fmt, src_rate, 0, NULL);
      if (swr[i] && swr_init(swr[i]) < 0) {
        swr_free(&swr[i]);
        swr[i] = NULL;
      }
    }
    if (mt == AVMEDIA_TYPE_VIDEO) {
      sws[i] = NULL;  // lazy-create on first frame to RGB24
    }
  }

  g_h[id].fmt = fmt;
  g_h[id].dec = dec;
  g_h[id].swr = swr;
  g_h[id].sws = sws;
  g_h[id].nb = nb;
  g_h[id].used = 1;
  vl_push_int(S, (int64_t)id);
  return 1;
}

static int vlff_streams(VL_State *S) {
  int id = (int)ff_check_int(S, 1);
  if (!chk_h(id)) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  FFHandle *H = &g_h[id];
  AuxBuffer out = {0};

  for (int i = 0; i < H->nb; i++) {
    AVStream *st = H->fmt->streams[i];
    const char *kind = av_get_media_type_string(st->codecpar->codec_type);
    if (!kind) kind = "unknown";
    const char *cname = avcodec_get_name(st->codecpar->codec_id);
    if (!cname) cname = "unknown";

    // Fields: idx,kind,codec,w,h,pixfmt,sr,ch,fmt,dur_ts,time_base,avg_fps
    char tmp[256];

    // idx
    snprintf(tmp, sizeof tmp, "%d", i);
    aux_buffer_append(&out, (const uint8_t *)tmp, strlen(tmp));
    uint8_t u = US;
    aux_buffer_append(&out, &u, 1);
    // kind
    aux_buffer_append(&out, (const uint8_t *)kind, strlen(kind));
    u = US;
    aux_buffer_append(&out, &u, 1);
    // codec
    aux_buffer_append(&out, (const uint8_t *)cname, strlen(cname));
    u = US;
    aux_buffer_append(&out, &u, 1);

    // video specifics
    if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      snprintf(tmp, sizeof tmp, "%d", st->codecpar->width);
      aux_buffer_append(&out, (const uint8_t *)tmp, strlen(tmp));
      u = US;
      aux_buffer_append(&out, &u, 1);
      snprintf(tmp, sizeof tmp, "%d", st->codecpar->height);
      aux_buffer_append(&out, (const uint8_t *)tmp, strlen(tmp));
      u = US;
      aux_buffer_append(&out, &u, 1);
      const char *pix =
          av_get_pix_fmt_name((enum AVPixelFormat)st->codecpar->format);
      if (!pix) pix = "";
      aux_buffer_append(&out, (const uint8_t *)pix, strlen(pix));
      u = US;
      aux_buffer_append(&out, &u, 1);
      // placeholders for audio fields
      aux_buffer_append(&out, (const uint8_t *)"", 0);
      u = US;
      aux_buffer_append(&out, &u, 1);  // sr
      aux_buffer_append(&out, (const uint8_t *)"", 0);
      u = US;
      aux_buffer_append(&out, &u, 1);  // ch
      aux_buffer_append(&out, (const uint8_t *)"", 0);
      u = US;
      aux_buffer_append(&out, &u, 1);  // sample_fmt
    } else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
      // fill empty video fields
      aux_buffer_append(&out, (const uint8_t *)"", 0);
      u = US;
      aux_buffer_append(&out, &u, 1);
      aux_buffer_append(&out, (const uint8_t *)"", 0);
      u = US;
      aux_buffer_append(&out, &u, 1);
      aux_buffer_append(&out, (const uint8_t *)"", 0);
      u = US;
      aux_buffer_append(&out, &u, 1);
      // audio specifics
      snprintf(tmp, sizeof tmp, "%d", st->codecpar->sample_rate);
      aux_buffer_append(&out, (const uint8_t *)tmp, strlen(tmp));
      u = US;
      aux_buffer_append(&out, &u, 1);
      snprintf(tmp, sizeof tmp, "%d",
               st->codecpar->ch_layout.nb_channels
                   ? st->codecpar->ch_layout.nb_channels
                   : st->codecpar->channels);
      aux_buffer_append(&out, (const uint8_t *)tmp, strlen(tmp));
      u = US;
      aux_buffer_append(&out, &u, 1);
      const char *sf =
          av_get_sample_fmt_name((enum AVSampleFormat)st->codecpar->format);
      if (!sf) sf = "";
      aux_buffer_append(&out, (const uint8_t *)sf, strlen(sf));
      u = US;
      aux_buffer_append(&out, &u, 1);
    } else {
      // fill 6 blanks
      for (int k = 0; k < 6; k++) {
        u = US;
        aux_buffer_append(&out, &u, 1);
      }
    }

    // duration ts
    int64_t dur = st->duration;
    snprintf(tmp, sizeof tmp, "%lld", (long long)dur);
    aux_buffer_append(&out, (const uint8_t *)tmp, strlen(tmp));
    u = US;
    aux_buffer_append(&out, &u, 1);
    // time_base
    snprintf(tmp, sizeof tmp, "%d/%d", st->time_base.num, st->time_base.den);
    aux_buffer_append(&out, (const uint8_t *)tmp, strlen(tmp));
    u = US;
    aux_buffer_append(&out, &u, 1);
    // avg_fps
    if (st->avg_frame_rate.num > 0 && st->avg_frame_rate.den > 0) {
      snprintf(tmp, sizeof tmp, "%d/%d", st->avg_frame_rate.num,
               st->avg_frame_rate.den);
    } else
      snprintf(tmp, sizeof tmp, "");
    aux_buffer_append(&out, (const uint8_t *)tmp, strlen(tmp));
    uint8_t r = RS;
    aux_buffer_append(&out, &r, 1);
  }

  vl_push_lstring(S, (const char *)out.data, (int)out.len);
  aux_buffer_free(&out);
  return 1;
}

static double ts_to_sec(int64_t ts, AVRational tb) {
  if (ts == AV_NOPTS_VALUE) return 0.0;
  return (double)ts * (double)tb.num / (double)tb.den;
}

static int vlff_read_packet(VL_State *S) {
  int id = (int)ff_check_int(S, 1);
  if (!chk_h(id)) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  FFHandle *H = &g_h[id];

  AVPacket *pkt = av_packet_alloc();
  if (!pkt) {
    vl_push_nil(S);
    vl_push_string(S, "ENOMEM");
    return 2;
  }

  int err = av_read_frame(H->fmt, pkt);
  if (err == AVERROR_EOF) {
    av_packet_free(&pkt);
    vl_push_nil(S);
    vl_push_string(S, "eof");
    return 2;
  }
  if (err < 0) {
    int ret = push_averr(S, err, "read");
    av_packet_free(&pkt);
    return ret;
  }

  int sid = pkt->stream_index;
  AVStream *st = H->fmt->streams[sid];
  double pts_sec = ts_to_sec(pkt->pts, st->time_base);

  vl_push_int(S, (int64_t)sid);
  vl_push_float(S, pts_sec);
  vl_push_int(S, (pkt->flags & AV_PKT_FLAG_KEY) ? 1 : 0);
  vl_push_lstring(S, (const char *)pkt->data, pkt->size);

  av_packet_free(&pkt);
  return 4;
}

static int want_mask_from_str(const char *w) {
  if (!w) return 3;  // both
  if (strcmp(w, "a") == 0) return 1;
  if (strcmp(w, "v") == 0) return 2;
  return 3;  // "av" or others -> both
}

static int vlff_decode_next(VL_State *S) {
  int id = (int)ff_check_int(S, 1);
  const char *want = ff_opt_str(S, 2, "av");
  if (!chk_h(id)) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  FFHandle *H = &g_h[id];
  int wantmask = want_mask_from_str(want);

  AVPacket *pkt = av_packet_alloc();
  AVFrame *frm = av_frame_alloc();
  if (!pkt || !frm) {
    if (pkt) av_packet_free(&pkt);
    if (frm) av_frame_free(&frm);
    vl_push_nil(S);
    vl_push_string(S, "ENOMEM");
    return 2;
  }

  for (;;) {
    int err = av_read_frame(H->fmt, pkt);
    if (err == AVERROR_EOF) {
      av_packet_unref(pkt);
      av_frame_free(&frm);
      av_packet_free(&pkt);
      vl_push_nil(S);
      vl_push_string(S, "eof");
      return 2;
    }
    if (err < 0) {
      int ret = push_averr(S, err, "read");
      av_frame_free(&frm);
      av_packet_free(&pkt);
      return ret;
    }

    int sid = pkt->stream_index;
    AVStream *st = H->fmt->streams[sid];
    AVCodecContext *dc = (sid >= 0 && sid < H->nb) ? H->dec[sid] : NULL;
    if (!dc) {
      av_packet_unref(pkt);
      continue;
    }

    enum AVMediaType mt = dc->codec_type;
    if ((mt == AVMEDIA_TYPE_AUDIO && !(wantmask & 1)) ||
        (mt == AVMEDIA_TYPE_VIDEO && !(wantmask & 2))) {
      av_packet_unref(pkt);
      continue;
    }

    // send/receive
    err = avcodec_send_packet(dc, pkt);
    av_packet_unref(pkt);
    if (err == 0 || err == AVERROR(EAGAIN)) {
      while (1) {
        err = avcodec_receive_frame(dc, frm);
        if (err == AVERROR(EAGAIN)) break;
        if (err == AVERROR_EOF) {
          break;
        }
        if (err < 0) {
          int ret = push_averr(S, err, "decode");
          av_frame_unref(frm);
          av_frame_free(&frm);
          av_packet_free(&pkt);
          return ret;
        }

        double pts_sec = 0.0;
        int64_t ts = frm->best_effort_timestamp != AV_NOPTS_VALUE
                         ? frm->best_effort_timestamp
                         : frm->pts;
        pts_sec = ts_to_sec(ts, st->time_base);

        if (mt == AVMEDIA_TYPE_AUDIO) {
          // Resample to f32 interleaved
          SwrContext *swr = H->swr[sid];
          int ch = dc->ch_layout.nb_channels ? dc->ch_layout.nb_channels
                                             : dc->channels;
          int sr = dc->sample_rate;
          if (!swr || ch <= 0 || sr <= 0) {
            av_frame_unref(frm);
            continue;
          }

          int in_samps = frm->nb_samples;
          int out_samps = (int)av_rescale_rnd(swr_get_delay(swr, sr) + in_samps,
                                              sr, sr, AV_ROUND_UP);
          uint8_t *outbuf = NULL;
          int out_linesize = 0;
          int ret = av_samples_alloc(&outbuf, &out_linesize, ch, out_samps,
                                     AV_SAMPLE_FMT_FLT, 0);
          if (ret < 0) {
            av_frame_unref(frm);
            continue;
          }

          int got =
              swr_convert(swr, &outbuf, out_samps,
                          (const uint8_t const *const *)frm->data, in_samps);
          if (got < 0) {
            av_freep(&outbuf);
            av_frame_unref(frm);
            continue;
          }

          int out_bytes = av_samples_get_buffer_size(&out_linesize, ch, got,
                                                     AV_SAMPLE_FMT_FLT, 0);
          if (out_bytes < 0) {
            av_freep(&outbuf);
            av_frame_unref(frm);
            continue;
          }

          vl_push_string(S, "audio");
          vl_push_int(S, (int64_t)sid);
          vl_push_float(S, pts_sec);
          vl_push_int(S, (int64_t)sr);
          vl_push_int(S, (int64_t)ch);
          vl_push_int(S, (int64_t)got);
          vl_push_lstring(S, (const char *)outbuf, out_bytes);

          av_freep(&outbuf);
          av_frame_unref(frm);
          av_frame_free(&frm);
          av_packet_free(&pkt);
          return 7;
        } else if (mt == AVMEDIA_TYPE_VIDEO) {
          // Convert to RGB24
          int w = frm->width, h = frm->height;
          if (w <= 0 || h <= 0) {
            av_frame_unref(frm);
            continue;
          }
          if (!H->sws[sid]) {
            H->sws[sid] = sws_getContext(w, h, (enum AVPixelFormat)frm->format,
                                         w, h, AV_PIX_FMT_RGB24, SWS_BILINEAR,
                                         NULL, NULL, NULL);
            if (!H->sws[sid]) {
              av_frame_unref(frm);
              continue;
            }
          }
          uint8_t *dst_data[4] = {0};
          int dst_linesize[4] = {0};
          int dst_bytes =
              av_image_alloc(dst_data, dst_linesize, w, h, AV_PIX_FMT_RGB24, 1);
          if (dst_bytes < 0) {
            av_frame_unref(frm);
            continue;
          }

          sws_scale(H->sws[sid], (const uint8_t *const *)frm->data,
                    frm->linesize, 0, h, dst_data, dst_linesize);

          vl_push_string(S, "video");
          vl_push_int(S, (int64_t)sid);
          vl_push_float(S, pts_sec);
          vl_push_int(S, (int64_t)w);
          vl_push_int(S, (int64_t)h);
          vl_push_lstring(S, (const char *)dst_data[0], dst_bytes);

          av_freep(&dst_data[0]);
          av_frame_unref(frm);
          av_frame_free(&frm);
          av_packet_free(&pkt);
          return 6;
        } else {
          av_frame_unref(frm);
          continue;
        }
      }
    } else if (err < 0) {
      int ret = push_averr(S, err, "sendpkt");
      av_frame_free(&frm);
      av_packet_free(&pkt);
      return ret;
    }
    // continue loop reading more packets
  }
}

static int vlff_seek(VL_State *S) {
  int id = (int)ff_check_int(S, 1);
  double secs = ff_check_num(S, 2);
  const char *mode = ff_opt_str(S, 3, "any");
  if (!chk_h(id)) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  FFHandle *H = &g_h[id];

  int flags = 0;
  if (strcmp(mode, "backward") == 0)
    flags |= AVSEEK_FLAG_BACKWARD;
  else if (strcmp(mode, "frame") == 0)
    flags |= AVSEEK_FLAG_FRAME;

  int64_t ts = (int64_t)(secs * (double)AV_TIME_BASE);
  int err = av_seek_frame(
      H->fmt, -1, av_rescale_q(ts, AV_TIME_BASE_Q, (AVRational){1, 1}), flags);
  if (err < 0) {
    // fallback using AV_TIME_BASE_Q
    err = avformat_seek_file(H->fmt, -1, INT64_MIN, ts, INT64_MAX, flags);
  }
  if (err < 0) return push_averr(S, err, "seek");

  // flush decoders
  for (int i = 0; i < H->nb; i++) {
    if (H->dec[i]) avcodec_flush_buffers(H->dec[i]);
  }
  vl_push_bool(S, 1);
  return 1;
}

static int vlff_close(VL_State *S) {
  int id = (int)ff_check_int(S, 1);
  if (id > 0 && id < g_h_cap && g_h[id].used) {
    free_handle(&g_h[id]);
  }
  vl_push_bool(S, 1);
  return 1;
}

#endif  // VL_HAVE_FFMPEG

// ---------------------------------------------------------------------
// Registration VM
// ---------------------------------------------------------------------
static const VL_Reg ffmpeglib[] = {{"version", vlff_version},
                                   {"open_input", vlff_open},
                                   {"streams", vlff_streams},
                                   {"read_packet", vlff_read_packet},
                                   {"decode_next", vlff_decode_next},
                                   {"seek", vlff_seek},
                                   {"close", vlff_close},
                                   {NULL, NULL}};

void vl_open_ffmpeglib(VL_State *S) { vl_register_lib(S, "ffmpeg", ffmpeglib); }
