// SPDX-License-Identifier: GPL-3.0-or-later
//
// codec.c — Audio codec bindings (Opus + FLAC) for Vitte Light VM (C17,
// complet) Namespace: "codec"
//
// Build examples:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -DVL_HAVE_OPUS -DVL_HAVE_FLAC -c
//   codec.c cc ... codec.o -lopus -lFLAC
//
// Portabilité:
//   - Implémentation réelle si macros et headers présents.
//   - Sinon: stubs -> (nil,"ENOSYS").
//
// Modèle:
//   - Opus: encode/decode paquets bruts (pas de conteneur Ogg).
//   - FLAC: encodage/décodage mémoire via callbacks (StreamEncoder/Decoder).
//   - PCM interleavé. Formats supportés:
//       * Opus: "f32" (float32) ou "i16".
//       * FLAC: entrée 16/24/32 bits; sortie 16 ou 32 bits selon source.
//
// API:
//
//   Capabilities
//     codec.version() -> string  // "opus,flac", "opus", "flac", or "stubs"
//
//   --- Opus ---
//     // Encoder
//     codec.opus_encoder_create(sample_rate:int, channels:int[, app:int=2049[,
//     pcm_fmt="f32"]])
//         -> id | (nil,errmsg)
//         // app: 2048=VOIP, 2049=AUDIO, 2051=LOWDELAY
//     codec.opus_encode(id, pcm_bytes:string, frame_size:int)
//         -> packet:string | (nil,errmsg)
//     codec.opus_encoder_free(id) -> true
//
//     // Decoder
//     codec.opus_decoder_create(sample_rate:int, channels:int[, pcm_fmt="f32"])
//         -> id | (nil,errmsg)
//     codec.opus_decode(id, packet:string[, fec=false])
//         -> pcm_bytes:string | (nil,errmsg)
//     codec.opus_decoder_free(id) -> true
//
//   --- FLAC ---
//     // One-shot encode
//     codec.flac_encode(pcm_bytes:string, sample_rate:int, channels:int,
//     bits_per_sample:int[, level:int=5])
//         -> flac_bytes:string | (nil,errmsg)
//     // One-shot decode
//     codec.flac_decode(flac_bytes:string)
//         -> pcm_bytes:string, sample_rate:int, channels:int,
//         bits_per_sample:int | (nil,errmsg)
//
// Dépendances: auxlib.h, state.h, object.h, vm.h

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "auxlib.h"
#include "object.h"
#include "state.h"
#include "vm.h"

#ifdef VL_HAVE_OPUS
#include <opus/opus.h>
#endif
#ifdef VL_HAVE_FLAC
#include <FLAC/stream_decoder.h>
#include <FLAC/stream_encoder.h>
#endif

// ---------------------------------------------------------------------
// VM arg helpers
// ---------------------------------------------------------------------
static const char *cc_check_str(VL_State *S, int idx) {
  if (vl_get(S, idx) && vl_isstring(S, idx))
    return vl_tocstring(S, vl_get(S, idx));
  vl_errorf(S, "argument #%d: string expected", idx);
  vl_error(S);
  return NULL;
}
static int64_t cc_check_int(VL_State *S, int idx) {
  if (vl_get(S, idx) && (vl_isint(S, idx) || vl_isfloat(S, idx)))
    return vl_isint(S, idx) ? vl_toint(S, vl_get(S, idx))
                            : (int64_t)vl_tonumber(S, vl_get(S, idx));
  vl_errorf(S, "argument #%d: int expected", idx);
  vl_error(S);
  return 0;
}
static int cc_opt_bool(VL_State *S, int idx, int defv) {
  if (!vl_get(S, idx)) return defv;
  return vl_tobool(vl_get(S, idx)) ? 1 : 0;
}
static int cc_opt_int(VL_State *S, int idx, int defv) {
  if (!vl_get(S, idx)) return defv;
  if (vl_isint(S, idx) || vl_isfloat(S, idx)) return (int)cc_check_int(S, idx);
  return defv;
}
static const char *cc_opt_str(VL_State *S, int idx, const char *defv) {
  if (!vl_get(S, idx) || !vl_isstring(S, idx)) return defv;
  return cc_check_str(S, idx);
}

// ---------------------------------------------------------------------
// Stubs if missing libs
// ---------------------------------------------------------------------
#if !defined(VL_HAVE_OPUS)
#define CC_OPUS_STUB 1
#else
#define CC_OPUS_STUB 0
#endif
#if !defined(VL_HAVE_FLAC)
#define CC_FLAC_STUB 1
#else
#define CC_FLAC_STUB 0
#endif

// Common
static int vlcc_version(VL_State *S) {
#if CC_OPUS_STUB && CC_FLAC_STUB
  vl_push_string(S, "stubs");
#elif !CC_OPUS_STUB && CC_FLAC_STUB
  vl_push_string(S, "opus");
#elif CC_OPUS_STUB && !CC_FLAC_STUB
  vl_push_string(S, "flac");
#else
  vl_push_string(S, "opus,flac");
#endif
  return 1;
}

#define NOSYS_PAIR(S)            \
  do {                           \
    vl_push_nil(S);              \
    vl_push_string(S, "ENOSYS"); \
    return 2;                    \
  } while (0)

// ---------------------------------------------------------------------
// ------------------------------ OPUS ---------------------------------
// ---------------------------------------------------------------------
#if CC_OPUS_STUB

static int vloc_opus_enc_create(VL_State *S) { NOSYS_PAIR(S); }
static int vloc_opus_encode(VL_State *S) { NOSYS_PAIR(S); }
static int vloc_opus_enc_free(VL_State *S) {
  vl_push_bool(S, 1);
  return 1;
}
static int vloc_opus_dec_create(VL_State *S) { NOSYS_PAIR(S); }
static int vloc_opus_decode(VL_State *S) { NOSYS_PAIR(S); }
static int vloc_opus_dec_free(VL_State *S) {
  vl_push_bool(S, 1);
  return 1;
}

#else

typedef struct OpusEncH {
  int used;
  OpusEncoder *enc;
  int sr, ch, app;
  int f32;  // 1=f32, 0=i16
} OpusEncH;

typedef struct OpusDecH {
  int used;
  OpusDecoder *dec;
  int sr, ch;
  int f32;
} OpusDecH;

static OpusEncH *g_oenc = NULL;
static int g_oenc_cap = 0;
static OpusDecH *g_odec = NULL;
static int g_odec_cap = 0;

static int ensure_oenc_cap(int need) {
  if (need <= g_oenc_cap) return 1;
  int n = g_oenc_cap ? g_oenc_cap : 8;
  while (n < need) n <<= 1;
  OpusEncH *nn = (OpusEncH *)realloc(g_oenc, (size_t)n * sizeof *nn);
  if (!nn) return 0;
  for (int i = g_oenc_cap; i < n; i++) {
    nn[i].used = 0;
    nn[i].enc = NULL;
    nn[i].sr = 0;
    nn[i].ch = 0;
    nn[i].app = 0;
    nn[i].f32 = 1;
  }
  g_oenc = nn;
  g_oenc_cap = n;
  return 1;
}
static int ensure_odec_cap(int need) {
  if (need <= g_odec_cap) return 1;
  int n = g_odec_cap ? g_odec_cap : 8;
  while (n < need) n <<= 1;
  OpusDecH *nn = (OpusDecH *)realloc(g_odec, (size_t)n * sizeof *nn);
  if (!nn) return 0;
  for (int i = g_odec_cap; i < n; i++) {
    nn[i].used = 0;
    nn[i].dec = NULL;
    nn[i].sr = 0;
    nn[i].ch = 0;
    nn[i].f32 = 1;
  }
  g_odec = nn;
  g_odec_cap = n;
  return 1;
}
static int alloc_oenc(void) {
  for (int i = 1; i < g_oenc_cap; i++)
    if (!g_oenc[i].used) return i;
  if (!ensure_oenc_cap(g_oenc_cap ? g_oenc_cap * 2 : 8)) return 0;
  for (int i = 1; i < g_oenc_cap; i++)
    if (!g_oenc[i].used) return i;
  return 0;
}
static int alloc_odec(void) {
  for (int i = 1; i < g_odec_cap; i++)
    if (!g_odec[i].used) return i;
  if (!ensure_odec_cap(g_odec_cap ? g_odec_cap * 2 : 8)) return 0;
  for (int i = 1; i < g_odec_cap; i++)
    if (!g_odec[i].used) return i;
  return 0;
}
static int chk_oenc(int id) {
  return id > 0 && id < g_oenc_cap && g_oenc[id].used && g_oenc[id].enc;
}
static int chk_odec(int id) {
  return id > 0 && id < g_odec_cap && g_odec[id].used && g_odec[id].dec;
}

static int push_opus_err(VL_State *S, int e) {
  vl_push_nil(S);
  vl_push_string(S, opus_strerror(e));
  return 2;
}

// codec.opus_encoder_create(sr,ch[,app=2049[,fmt="f32"]])
static int vloc_opus_enc_create(VL_State *S) {
  int sr = (int)cc_check_int(S, 1);
  int ch = (int)cc_check_int(S, 2);
  int app = cc_opt_int(S, 3, 2049);  // OPUS_APPLICATION_AUDIO
  const char *fmt = cc_opt_str(S, 4, "f32");
  if (sr <= 0 || ch <= 0 || (strcmp(fmt, "f32") && strcmp(fmt, "i16"))) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }

  int err = 0;
  OpusEncoder *enc = opus_encoder_create(sr, ch, app, &err);
  if (!enc || err != OPUS_OK) return push_opus_err(S, err);

  int id = alloc_oenc();
  if (!id) {
    opus_encoder_destroy(enc);
    vl_push_nil(S);
    vl_push_string(S, "ENOMEM");
    return 2;
  }
  g_oenc[id].used = 1;
  g_oenc[id].enc = enc;
  g_oenc[id].sr = sr;
  g_oenc[id].ch = ch;
  g_oenc[id].app = app;
  g_oenc[id].f32 = strcmp(fmt, "f32") == 0 ? 1 : 0;
  vl_push_int(S, (int64_t)id);
  return 1;
}

// codec.opus_encode(id, pcm, frame_size) -> packet
static int vloc_opus_encode(VL_State *S) {
  int id = (int)cc_check_int(S, 1);
  const char *pcm = cc_check_str(S, 2);
  int frame = (int)cc_check_int(S, 3);
  if (!chk_oenc(id) || frame <= 0) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }

  // opus max packet ~1275 bytes; safety buf 4096
  unsigned char out[4096];
  int ret;
  if (g_oenc[id].f32) {
    // Expect interleaved float32
    // VM strings are 0-terminées; binaire non garanti -> limitation connue
    int samples = frame * g_oenc[id].ch;
    ret = opus_encode_float(g_oenc[id].enc, (const float *)pcm, frame, out,
                            (opus_int32)sizeof out);
  } else {
    ret = opus_encode(g_oenc[id].enc, (const opus_int16 *)pcm, frame, out,
                      (opus_int32)sizeof out);
  }
  if (ret < 0) return push_opus_err(S, ret);
  vl_push_lstring(S, (const char *)out, ret);
  return 1;
}

static int vloc_opus_enc_free(VL_State *S) {
  int id = (int)cc_check_int(S, 1);
  if (chk_oenc(id)) {
    opus_encoder_destroy(g_oenc[id].enc);
    g_oenc[id].enc = NULL;
    g_oenc[id].used = 0;
  }
  vl_push_bool(S, 1);
  return 1;
}

// codec.opus_decoder_create(sr,ch[,fmt="f32"])
static int vloc_opus_dec_create(VL_State *S) {
  int sr = (int)cc_check_int(S, 1);
  int ch = (int)cc_check_int(S, 2);
  const char *fmt = cc_opt_str(S, 3, "f32");
  if (sr <= 0 || ch <= 0 || (strcmp(fmt, "f32") && strcmp(fmt, "i16"))) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  int err = 0;
  OpusDecoder *dec = opus_decoder_create(sr, ch, &err);
  if (!dec || err != OPUS_OK) return push_opus_err(S, err);
  int id = alloc_odec();
  if (!id) {
    opus_decoder_destroy(dec);
    vl_push_nil(S);
    vl_push_string(S, "ENOMEM");
    return 2;
  }
  g_odec[id].used = 1;
  g_odec[id].dec = dec;
  g_odec[id].sr = sr;
  g_odec[id].ch = ch;
  g_odec[id].f32 = strcmp(fmt, "f32") == 0 ? 1 : 0;
  vl_push_int(S, (int64_t)id);
  return 1;
}

// codec.opus_decode(id, packet[, fec=false]) -> pcm
static int vloc_opus_decode(VL_State *S) {
  int id = (int)cc_check_int(S, 1);
  const unsigned char *pkt = (const unsigned char *)cc_check_str(S, 2);
  int fec = cc_opt_bool(S, 3, 0);
  if (!chk_odec(id)) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }

  // Opus max samples per channel per packet ~ 120ms @ 48k => 5760
  const int max_per_ch = 5760;
  int ch = g_odec[id].ch;
  if (g_odec[id].f32) {
    float *buf =
        (float *)malloc((size_t)max_per_ch * (size_t)ch * sizeof(float));
    if (!buf) {
      vl_push_nil(S);
      vl_push_string(S, "ENOMEM");
      return 2;
    }
    int n = opus_decode_float(g_odec[id].dec, pkt,
                              (opus_int32)strlen((const char *)pkt), buf,
                              max_per_ch, fec ? 1 : 0);
    if (n < 0) {
      free(buf);
      return push_opus_err(S, n);
    }
    vl_push_lstring(S, (const char *)buf, n * ch * (int)sizeof(float));
    free(buf);
  } else {
    opus_int16 *buf = (opus_int16 *)malloc((size_t)max_per_ch * (size_t)ch *
                                           sizeof(opus_int16));
    if (!buf) {
      vl_push_nil(S);
      vl_push_string(S, "ENOMEM");
      return 2;
    }
    int n =
        opus_decode(g_odec[id].dec, pkt, (opus_int32)strlen((const char *)pkt),
                    buf, max_per_ch, fec ? 1 : 0);
    if (n < 0) {
      free(buf);
      return push_opus_err(S, n);
    }
    vl_push_lstring(S, (const char *)buf, n * ch * (int)sizeof(opus_int16));
    free(buf);
  }
  return 1;
}

static int vloc_opus_dec_free(VL_State *S) {
  int id = (int)cc_check_int(S, 1);
  if (chk_odec(id)) {
    opus_decoder_destroy(g_odec[id].dec);
    g_odec[id].dec = NULL;
    g_odec[id].used = 0;
  }
  vl_push_bool(S, 1);
  return 1;
}

#endif  // OPUS

// ---------------------------------------------------------------------
// ------------------------------ FLAC ---------------------------------
// ---------------------------------------------------------------------
#if CC_FLAC_STUB

static int vloc_flac_encode(VL_State *S) { NOSYS_PAIR(S); }
static int vloc_flac_decode(VL_State *S) { NOSYS_PAIR(S); }

#else

typedef struct MemIO {
  const uint8_t *in;
  size_t in_len;
  size_t in_off;
  AuxBuffer out;
} MemIO;

// -------- ENCODE --------
static FLAC__StreamEncoderWriteStatus enc_write_cb(
    const FLAC__StreamEncoder *encoder, const FLAC__byte buffer[], size_t bytes,
    unsigned samples, unsigned current_frame, void *client_data) {
  (void)encoder;
  (void)samples;
  (void)current_frame;
  MemIO *io = (MemIO *)client_data;
  aux_buffer_append(&io->out, (const uint8_t *)buffer, bytes);
  return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
}
static FLAC__StreamEncoderSeekStatus enc_seek_cb(
    const FLAC__StreamEncoder *encoder, FLAC__uint64 absolute_byte_offset,
    void *client_data) {
  (void)encoder;
  (void)absolute_byte_offset;
  (void)client_data;
  return FLAC__STREAM_ENCODER_SEEK_STATUS_UNSUPPORTED;
}
static FLAC__StreamEncoderTellStatus enc_tell_cb(
    const FLAC__StreamEncoder *encoder, FLAC__uint64 *absolute_byte_offset,
    void *client_data) {
  (void)encoder;
  (void)client_data;
  if (absolute_byte_offset) *absolute_byte_offset = 0;
  return FLAC__STREAM_ENCODER_TELL_STATUS_UNSUPPORTED;
}

// codec.flac_encode(pcm, sr, ch, bps[, level=5]) -> flac_bytes
static int vloc_flac_encode(VL_State *S) {
  const char *pcm = cc_check_str(S, 1);
  int sr = (int)cc_check_int(S, 2);
  int ch = (int)cc_check_int(S, 3);
  int bps = (int)cc_check_int(S, 4);  // 16/24/32
  int level = cc_opt_int(S, 5, 5);
  if (sr <= 0 || ch <= 0 || (bps != 16 && bps != 24 && bps != 32)) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }

  size_t nbytes = strlen(pcm);
  size_t fsize = (size_t)ch * (size_t)(bps / 8);
  if (fsize == 0 || nbytes % fsize != 0) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  size_t frames = nbytes / fsize;

  // Convert to int32 interleaved for FLAC
  FLAC__int32 *tmp =
      (FLAC__int32 *)malloc(frames * (size_t)ch * sizeof(FLAC__int32));
  if (!tmp) {
    vl_push_nil(S);
    vl_push_string(S, "ENOMEM");
    return 2;
  }
  const uint8_t *p = (const uint8_t *)pcm;
  for (size_t i = 0; i < frames * (size_t)ch; i++) {
    if (bps == 16) {
      int16_t v = (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
      tmp[i] = (int32_t)v;
      p += 2;
    } else if (bps == 24) {
      int32_t v =
          ((int32_t)p[0] | ((int32_t)p[1] << 8) | ((int32_t)p[2] << 16));
      // sign-extend 24->32
      if (v & 0x800000) v |= 0xFF000000;
      tmp[i] = v;
      p += 3;
    } else {
      int32_t v = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                            ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
      tmp[i] = v;
      p += 4;
    }
  }

  MemIO io;
  memset(&io, 0, sizeof io);
  FLAC__StreamEncoder *enc = FLAC__stream_encoder_new();
  if (!enc) {
    free(tmp);
    vl_push_nil(S);
    vl_push_string(S, "EIO");
    return 2;
  }

  FLAC__bool ok = 1;
  ok &= FLAC__stream_encoder_set_channels(enc, (unsigned)ch);
  ok &= FLAC__stream_encoder_set_bits_per_sample(enc, (unsigned)bps);
  ok &= FLAC__stream_encoder_set_sample_rate(enc, (unsigned)sr);
  ok &= FLAC__stream_encoder_set_compression_level(enc, (unsigned)level);
  if (!ok) {
    FLAC__stream_encoder_delete(enc);
    free(tmp);
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }

  if (FLAC__stream_encoder_init_stream(enc, enc_write_cb, enc_seek_cb,
                                       enc_tell_cb, NULL, &io) !=
      FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
    FLAC__stream_encoder_delete(enc);
    free(tmp);
    vl_push_nil(S);
    vl_push_string(S, "EIO");
    return 2;
  }

  ok = FLAC__stream_encoder_process_interleaved(enc, tmp, (unsigned)frames);
  ok &= FLAC__stream_encoder_finish(enc);
  FLAC__stream_encoder_delete(enc);
  free(tmp);
  if (!ok) {
    aux_buffer_free(&io.out);
    vl_push_nil(S);
    vl_push_string(S, "EIO");
    return 2;
  }

  vl_push_lstring(S, (const char *)io.out.data, (int)io.out.len);
  aux_buffer_free(&io.out);
  return 1;
}

// -------- DECODE --------
typedef struct DecCtx {
  MemIO io;
  int got_info;
  unsigned sr, ch, bps;
  AuxBuffer pcm;  // packed out: if bps<=16 -> int16, else int32
} DecCtx;

static FLAC__StreamDecoderReadStatus dec_read_cb(
    const FLAC__StreamDecoder *decoder, FLAC__byte buffer[], size_t *bytes,
    void *client_data) {
  (void)decoder;
  DecCtx *d = (DecCtx *)client_data;
  if (*bytes == 0) return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
  size_t remain = d->io.in_len - d->io.in_off;
  size_t n = *bytes < remain ? *bytes : remain;
  if (n == 0) {
    *bytes = 0;
    return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
  }
  memcpy(buffer, d->io.in + d->io.in_off, n);
  d->io.in_off += n;
  *bytes = n;
  return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}
static FLAC__StreamDecoderSeekStatus dec_seek_cb(
    const FLAC__StreamDecoder *decoder, FLAC__uint64 absolute_byte_offset,
    void *client_data) {
  (void)decoder;
  DecCtx *d = (DecCtx *)client_data;
  if (absolute_byte_offset > d->io.in_len)
    return FLAC__STREAM_DECODER_SEEK_STATUS_ERROR;
  d->io.in_off = (size_t)absolute_byte_offset;
  return FLAC__STREAM_DECODER_SEEK_STATUS_OK;
}
static FLAC__StreamDecoderTellStatus dec_tell_cb(
    const FLAC__StreamDecoder *decoder, FLAC__uint64 *absolute_byte_offset,
    void *client_data) {
  (void)decoder;
  DecCtx *d = (DecCtx *)client_data;
  if (absolute_byte_offset) *absolute_byte_offset = d->io.in_off;
  return FLAC__STREAM_DECODER_TELL_STATUS_OK;
}
static FLAC__StreamDecoderLengthStatus dec_len_cb(
    const FLAC__StreamDecoder *decoder, FLAC__uint64 *stream_length,
    void *client_data) {
  (void)decoder;
  DecCtx *d = (DecCtx *)client_data;
  if (stream_length) *stream_length = d->io.in_len;
  return FLAC__STREAM_DECODER_LENGTH_STATUS_OK;
}
static FLAC__bool dec_eof_cb(const FLAC__StreamDecoder *decoder,
                             void *client_data) {
  (void)decoder;
  DecCtx *d = (DecCtx *)client_data;
  return d->io.in_off >= d->io.in_len;
}
static FLAC__StreamDecoderWriteStatus dec_write_cb(
    const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame,
    const FLAC__int32 *const buffer[], void *client_data) {
  (void)decoder;
  DecCtx *d = (DecCtx *)client_data;
  unsigned ch = frame->header.channels;
  unsigned n = frame->header.blocksize;
  if (!d->got_info) {
    d->sr = frame->header.sample_rate;
    d->ch = ch;
    d->bps = frame->header.bits_per_sample;
    d->got_info = 1;
  }
  if (d->bps <= 16) {
    for (unsigned i = 0; i < n; i++) {
      for (unsigned c = 0; c < ch; c++) {
        int32_t v = buffer[c][i];
        int16_t s = (v < -32768) ? -32768 : (v > 32767 ? 32767 : (int16_t)v);
        aux_buffer_append(&d->pcm, (const uint8_t *)&s, 2);
      }
    }
  } else {
    for (unsigned i = 0; i < n; i++) {
      for (unsigned c = 0; c < ch; c++) {
        int32_t v = buffer[c][i];
        aux_buffer_append(&d->pcm, (const uint8_t *)&v, 4);
      }
    }
  }
  return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}
static void dec_meta_cb(const FLAC__StreamDecoder *decoder,
                        const FLAC__StreamMetaData *metadata,
                        void *client_data) {
  (void)decoder;
  DecCtx *d = (DecCtx *)client_data;
  if (metadata->type == FLAC__METADATA_TYPE_STREAMINFO && !d->got_info) {
    d->sr = metadata->data.stream_info.sample_rate;
    d->ch = metadata->data.stream_info.channels;
    d->bps = metadata->data.stream_info.bits_per_sample;
    d->got_info = 1;
  }
}
static void dec_err_cb(const FLAC__StreamDecoder *decoder,
                       FLAC__StreamDecoderErrorStatus status,
                       void *client_data) {
  (void)decoder;
  (void)status;
  (void)client_data;
}

// codec.flac_decode(flac_bytes) -> pcm, sr, ch, bps
static int vloc_flac_decode(VL_State *S) {
  const char *bytes = cc_check_str(S, 1);
  size_t n = strlen(bytes);

  DecCtx ctx;
  memset(&ctx, 0, sizeof ctx);
  ctx.io.in = (const uint8_t *)bytes;
  ctx.io.in_len = n;
  ctx.io.in_off = 0;

  FLAC__StreamDecoder *dec = FLAC__stream_decoder_new();
  if (!dec) {
    vl_push_nil(S);
    vl_push_string(S, "EIO");
    return 2;
  }

  FLAC__StreamDecoderInitStatus ist = FLAC__stream_decoder_init_stream(
      dec, dec_read_cb, dec_seek_cb, dec_tell_cb, dec_len_cb, dec_eof_cb,
      dec_write_cb, dec_meta_cb, dec_err_cb, &ctx);
  if (ist != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
    FLAC__stream_decoder_delete(dec);
    vl_push_nil(S);
    vl_push_string(S, "EIO");
    return 2;
  }

  FLAC__bool ok = FLAC__stream_decoder_process_until_end_of_stream(dec);
  ok &= FLAC__stream_decoder_finish(dec);
  FLAC__stream_decoder_delete(dec);
  if (!ok) {
    aux_buffer_free(&ctx.pcm);
    vl_push_nil(S);
    vl_push_string(S, "EIO");
    return 2;
  }

  vl_push_lstring(S, (const char *)ctx.pcm.data, (int)ctx.pcm.len);
  vl_push_int(S, (int64_t)ctx.sr);
  vl_push_int(S, (int64_t)ctx.ch);
  vl_push_int(S, (int64_t)(ctx.bps <= 16 ? 16 : 32));
  aux_buffer_free(&ctx.pcm);
  return 4;
}

#endif  // FLAC

// ---------------------------------------------------------------------
// Registration VM
// ---------------------------------------------------------------------
static const VL_Reg codeclib[] = {{"version", vlcc_version},

                                  // Opus
                                  {"opus_encoder_create", vloc_opus_enc_create},
                                  {"opus_encode", vloc_opus_encode},
                                  {"opus_encoder_free", vloc_opus_enc_free},
                                  {"opus_decoder_create", vloc_opus_dec_create},
                                  {"opus_decode", vloc_opus_decode},
                                  {"opus_decoder_free", vloc_opus_dec_free},

                                  // FLAC
                                  {"flac_encode", vloc_flac_encode},
                                  {"flac_decode", vloc_flac_decode},

                                  {NULL, NULL}};

void vl_open_codeclib(VL_State *S) { vl_register_lib(S, "codec", codeclib); }
