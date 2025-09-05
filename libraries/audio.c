// SPDX-License-Identifier: GPL-3.0-or-later
//
// audio.c — PortAudio bindings pour Vitte Light VM (C17, complet)
// Namespace: "audio"
//
// Build:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -DVL_HAVE_PORTAUDIO -c audio.c
//   cc ... audio.o -lportaudio
//
// Portabilité:
//   - Implémentation réelle si VL_HAVE_PORTAUDIO et <portaudio.h> présents.
//   - Sinon: stubs -> (nil,"ENOSYS").
//
// Modèle:
//   - Un id correspond à un PaStream bloquant interleavé (I/O
//   half/full-duplex).
//   - Formats: "f32", "i16", "i24", "i32", "u8" (interleaved).
//
// API:
//
//   Init / fin
//     audio.init()                        -> true | (nil,errmsg)
//     audio.terminate()                   -> true | (nil,errmsg)
//
//   Périphériques
//     audio.device_count()                -> int
//     audio.default_input_device()        -> int  // index PortAudio, -1 si
//     aucun audio.default_output_device()       -> int
//     audio.device_info(index:int)
//         -> name, hostapi, max_in, max_out, default_sr:float,
//            in_latency_ms:int, out_latency_ms:int | (nil,errmsg)
//
//   Flux
//     audio.open(input_dev:int|-1, output_dev:int|-1,
//                sample_rate:int, frames_per_buffer:int,
//                in_channels:int, out_channels:int,
//                fmt:string("f32","i16","i24","i32","u8")
//                [, latency_ms:int])
//         -> id | (nil,errmsg)
//     audio.start(id)                     -> true | (nil,errmsg)
//     audio.stop(id)                      -> true | (nil,errmsg)
//     audio.close(id)                     -> true
//
//   I/O bloquant
//     audio.read(id, frames:int)          -> bytes | (nil,errmsg)
//     audio.write(id, data:string)        -> frames_written:int | (nil,errmsg)
//
//   Infos runtime
//     audio.time(id)                      -> stream_time_seconds:float |
//     (nil,errmsg) audio.cpu_load(id)                  -> percent:float |
//     (nil,errmsg) audio.available_read(id)            -> frames:int |
//     (nil,errmsg) audio.available_write(id)           -> frames:int |
//     (nil,errmsg)
//
// Notes:
//   - `open` accepte half-duplex (input_dev=-1 ou output_dev=-1).
//   - `write` exige data multiple de (bytes_per_sample * out_channels).
//   - Les buffers peuvent contenir des octets NUL; la VM doit permettre des
//     strings binaires (vl_push_lstring utilisé en sortie).
//
// Dépendances: auxlib.h, state.h, object.h, vm.h

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "auxlib.h"
#include "object.h"
#include "state.h"
#include "vm.h"

#ifdef VL_HAVE_PORTAUDIO
#include <portaudio.h>
#endif

// ---------------------------------------------------------------------
// VM arg helpers
// ---------------------------------------------------------------------
static const char *au_check_str(VL_State *S, int idx) {
  if (vl_get(S, idx) && vl_isstring(S, idx))
    return vl_tocstring(S, vl_get(S, idx));
  vl_errorf(S, "argument #%d: string expected", idx);
  vl_error(S);
  return NULL;
}
static int64_t au_check_int(VL_State *S, int idx) {
  if (vl_get(S, idx) && (vl_isint(S, idx) || vl_isfloat(S, idx)))
    return vl_isint(S, idx) ? vl_toint(S, vl_get(S, idx))
                            : (int64_t)vl_tonumber(S, vl_get(S, idx));
  vl_errorf(S, "argument #%d: int expected", idx);
  vl_error(S);
  return 0;
}
static int au_opt_int(VL_State *S, int idx, int defv) {
  if (!vl_get(S, idx)) return defv;
  if (vl_isint(S, idx) || vl_isfloat(S, idx)) return (int)au_check_int(S, idx);
  return defv;
}

// ---------------------------------------------------------------------
// Stubs si PortAudio absent
// ---------------------------------------------------------------------
#ifndef VL_HAVE_PORTAUDIO

#define NOSYS_PAIR(S)            \
  do {                           \
    vl_push_nil(S);              \
    vl_push_string(S, "ENOSYS"); \
    return 2;                    \
  } while (0)
static int vla_init(VL_State *S) {
  vl_push_nil(S);
  vl_push_string(S, "ENOSYS");
  return 2;
}
static int vla_term(VL_State *S) {
  vl_push_nil(S);
  vl_push_string(S, "ENOSYS");
  return 2;
}
static int vla_dev_count(VL_State *S) {
  vl_push_int(S, 0);
  return 1;
}
static int vla_def_in(VL_State *S) {
  vl_push_int(S, -1);
  return 1;
}
static int vla_def_out(VL_State *S) {
  vl_push_int(S, -1);
  return 1;
}
static int vla_dev_info(VL_State *S) {
  (void)au_check_int(S, 1);
  NOSYS_PAIR(S);
}
static int vla_open(VL_State *S) {
  (void)au_check_int(S, 1);
  NOSYS_PAIR(S);
}
static int vla_start(VL_State *S) { NOSYS_PAIR(S); }
static int vla_stop(VL_State *S) { NOSYS_PAIR(S); }
static int vla_close(VL_State *S) {
  vl_push_bool(S, 1);
  return 1;
}
static int vla_read(VL_State *S) { NOSYS_PAIR(S); }
static int vla_write(VL_State *S) { NOSYS_PAIR(S); }
static int vla_time(VL_State *S) { NOSYS_PAIR(S); }
static int vla_cpu(VL_State *S) { NOSYS_PAIR(S); }
static int vla_avr(VL_State *S) { NOSYS_PAIR(S); }
static int vla_avw(VL_State *S) { NOSYS_PAIR(S); }

#else
// ---------------------------------------------------------------------
// Implémentation réelle (PortAudio)
// ---------------------------------------------------------------------

typedef struct AStream {
  int used;
  PaStream *st;
  int in_ch;
  int out_ch;
  PaSampleFormat fmt;
  int bps;  // bytes per sample (1,2,3,4)
  int fpb;  // frames per buffer
  double sr;
} AStream;

static AStream *g_as = NULL;
static int g_as_cap = 0;
static int g_pa_inited = 0;

static int ensure_as_cap(int need) {
  if (need <= g_as_cap) return 1;
  int ncap = g_as_cap ? g_as_cap : 8;
  while (ncap < need) ncap <<= 1;
  AStream *na = (AStream *)realloc(g_as, (size_t)ncap * sizeof *na);
  if (!na) return 0;
  for (int i = g_as_cap; i < ncap; i++) {
    na[i].used = 0;
    na[i].st = NULL;
    na[i].in_ch = 0;
    na[i].out_ch = 0;
    na[i].fmt = 0;
    na[i].bps = 0;
    na[i].fpb = 0;
    na[i].sr = 0.0;
  }
  g_as = na;
  g_as_cap = ncap;
  return 1;
}
static int alloc_as_slot(void) {
  for (int i = 1; i < g_as_cap; i++)
    if (!g_as[i].used) return i;
  if (!ensure_as_cap(g_as_cap ? g_as_cap * 2 : 8)) return 0;
  for (int i = 1; i < g_as_cap; i++)
    if (!g_as[i].used) return i;
  return 0;
}
static int check_id(int id) {
  return id > 0 && id < g_as_cap && g_as[id].used && g_as[id].st;
}

static int push_pa_err(VL_State *S, PaError e) {
  vl_push_nil(S);
  vl_push_string(S, Pa_GetErrorText(e));
  return 2;
}

static PaSampleFormat fmt_from_str(const char *s, int *bps) {
  if (!s) {
    if (bps) *bps = 4;
    return paFloat32;
  }
  if (strcmp(s, "f32") == 0) {
    if (bps) *bps = 4;
    return paFloat32;
  }
  if (strcmp(s, "i16") == 0) {
    if (bps) *bps = 2;
    return paInt16;
  }
  if (strcmp(s, "i24") == 0) {
    if (bps) *bps = 3;
    return paInt24;
  }
  if (strcmp(s, "i32") == 0) {
    if (bps) *bps = 4;
    return paInt32;
  }
  if (strcmp(s, "u8") == 0) {
    if (bps) *bps = 1;
    return paUInt8;
  }
  if (bps) *bps = 4;
  return paFloat32;
}

// --- Init/Terminate ---

static int vla_init(VL_State *S) {
  if (!g_pa_inited) {
    PaError e = Pa_Initialize();
    if (e != paNoError) return push_pa_err(S, e);
    g_pa_inited = 1;
  }
  vl_push_bool(S, 1);
  return 1;
}

static int vla_term(VL_State *S) {
  if (g_pa_inited) {
    Pa_Terminate();
    g_pa_inited = 0;
  }
  vl_push_bool(S, 1);
  return 1;
}

// --- Devices ---

static int vla_dev_count(VL_State *S) {
  if (!g_pa_inited) {
    PaError e = Pa_Initialize();
    if (e != paNoError) return push_pa_err(S, e);
    g_pa_inited = 1;
  }
  vl_push_int(S, (int64_t)Pa_GetDeviceCount());
  return 1;
}

static int vla_def_in(VL_State *S) {
  if (!g_pa_inited) {
    PaError e = Pa_Initialize();
    if (e != paNoError) return push_pa_err(S, e);
    g_pa_inited = 1;
  }
  vl_push_int(S, (int64_t)Pa_GetDefaultInputDevice());
  return 1;
}

static int vla_def_out(VL_State *S) {
  if (!g_pa_inited) {
    PaError e = Pa_Initialize();
    if (e != paNoError) return push_pa_err(S, e);
    g_pa_inited = 1;
  }
  vl_push_int(S, (int64_t)Pa_GetDefaultOutputDevice());
  return 1;
}

static int vla_dev_info(VL_State *S) {
  int idx = (int)au_check_int(S, 1);
  if (!g_pa_inited) {
    PaError e = Pa_Initialize();
    if (e != paNoError) return push_pa_err(S, e);
    g_pa_inited = 1;
  }
  if (idx < 0 || idx >= Pa_GetDeviceCount()) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  const PaDeviceInfo *di = Pa_GetDeviceInfo(idx);
  if (!di) {
    vl_push_nil(S);
    vl_push_string(S, "EIO");
    return 2;
  }
  const PaHostApiInfo *hai = Pa_GetHostApiInfo(di->hostApi);
  const char *host = hai ? hai->name : "";

  vl_push_string(S, di->name ? di->name : "");
  vl_push_string(S, host);
  vl_push_int(S, (int64_t)di->maxInputChannels);
  vl_push_int(S, (int64_t)di->maxOutputChannels);
  vl_push_float(S, di->defaultSampleRate);
  vl_push_int(S, (int64_t)(di->defaultLowInputLatency * 1000.0 + 0.5));
  vl_push_int(S, (int64_t)(di->defaultLowOutputLatency * 1000.0 + 0.5));
  return 7;
}

// --- Open/Start/Stop/Close ---

static int vla_open(VL_State *S) {
  int indev = (int)au_check_int(S, 1);
  int outdev = (int)au_check_int(S, 2);
  int sr = (int)au_check_int(S, 3);
  int fpb = (int)au_check_int(S, 4);
  int in_ch = (int)au_check_int(S, 5);
  int out_ch = (int)au_check_int(S, 6);
  const char *fmtstr = au_check_str(S, 7);
  int latency_ms = au_opt_int(S, 8, 0);

  if (!g_pa_inited) {
    PaError e = Pa_Initialize();
    if (e != paNoError) return push_pa_err(S, e);
    g_pa_inited = 1;
  }

  if (in_ch < 0 || out_ch < 0 || sr <= 0 || fpb <= 0) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  if (indev < 0 && outdev < 0) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }

  PaSampleFormat sf;
  int bps = 0;
  sf = fmt_from_str(fmtstr, &bps);

  PaStreamParameters inP, outP;
  PaStreamParameters *pin = NULL, *pout = NULL;

  if (indev >= 0) {
    const PaDeviceInfo *di = Pa_GetDeviceInfo(indev);
    if (!di || in_ch > di->maxInputChannels) {
      vl_push_nil(S);
      vl_push_string(S, "EINVAL");
      return 2;
    }
    inP.device = indev;
    inP.channelCount = in_ch;
    inP.sampleFormat = sf;
    inP.suggestedLatency =
        latency_ms > 0 ? (latency_ms / 1000.0) : di->defaultLowInputLatency;
    inP.hostApiSpecificStreamInfo = NULL;
    pin = &inP;
  }
  if (outdev >= 0) {
    const PaDeviceInfo *do_ = Pa_GetDeviceInfo(outdev);
    if (!do_ || out_ch > do_->maxOutputChannels) {
      vl_push_nil(S);
      vl_push_string(S, "EINVAL");
      return 2;
    }
    outP.device = outdev;
    outP.channelCount = out_ch;
    outP.sampleFormat = sf;
    outP.suggestedLatency =
        latency_ms > 0 ? (latency_ms / 1000.0) : do_->defaultLowOutputLatency;
    outP.hostApiSpecificStreamInfo = NULL;
    pout = &outP;
  }

  PaStream *st = NULL;
  PaError e = Pa_OpenStream(&st, pin, pout, (double)sr, (unsigned long)fpb,
                            paNoFlag, NULL, NULL);
  if (e != paNoError) return push_pa_err(S, e);

  int id = alloc_as_slot();
  if (!id) {
    Pa_CloseStream(st);
    vl_push_nil(S);
    vl_push_string(S, "ENOMEM");
    return 2;
  }
  g_as[id].used = 1;
  g_as[id].st = st;
  g_as[id].in_ch = in_ch;
  g_as[id].out_ch = out_ch;
  g_as[id].fmt = sf;
  g_as[id].bps = bps;
  g_as[id].fpb = fpb;
  g_as[id].sr = (double)sr;

  vl_push_int(S, (int64_t)id);
  return 1;
}

static int vla_start(VL_State *S) {
  int id = (int)au_check_int(S, 1);
  if (!check_id(id)) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  PaError e = Pa_StartStream(g_as[id].st);
  if (e != paNoError) return push_pa_err(S, e);
  vl_push_bool(S, 1);
  return 1;
}

static int vla_stop(VL_State *S) {
  int id = (int)au_check_int(S, 1);
  if (!check_id(id)) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  PaError e = Pa_StopStream(g_as[id].st);
  if (e != paNoError) return push_pa_err(S, e);
  vl_push_bool(S, 1);
  return 1;
}

static int vla_close(VL_State *S) {
  int id = (int)au_check_int(S, 1);
  if (id > 0 && id < g_as_cap && g_as[id].used) {
    Pa_CloseStream(g_as[id].st);
    g_as[id].st = NULL;
    g_as[id].used = 0;
  }
  vl_push_bool(S, 1);
  return 1;
}

// --- I/O ---

static int vla_read(VL_State *S) {
  int id = (int)au_check_int(S, 1);
  int frames = (int)au_check_int(S, 2);
  if (!check_id(id) || frames <= 0 || g_as[id].in_ch <= 0) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }

  size_t bytes = (size_t)frames * (size_t)g_as[id].in_ch * (size_t)g_as[id].bps;
  void *buf = malloc(bytes);
  if (!buf) {
    vl_push_nil(S);
    vl_push_string(S, "ENOMEM");
    return 2;
  }

  PaError e = Pa_ReadStream(g_as[id].st, buf, (unsigned long)frames);
  if (e != paNoError) {
    free(buf);
    return push_pa_err(S, e);
  }

  vl_push_lstring(S, (const char *)buf, (int)bytes);
  free(buf);
  return 1;
}

static int vla_write(VL_State *S) {
  int id = (int)au_check_int(S, 1);
  const char *data = au_check_str(S, 2);
  if (!check_id(id) || g_as[id].out_ch <= 0) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  size_t n = strlen(data);  // ATTENTION: requiert strings binaires supportées
                            // par VM si nuls présents
  size_t fsize = (size_t)g_as[id].out_ch * (size_t)g_as[id].bps;
  if (fsize == 0) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  size_t frames = n / fsize;
  if (frames == 0) {
    vl_push_int(S, 0);
    return 1;
  }

  PaError e = Pa_WriteStream(g_as[id].st, data, (unsigned long)frames);
  if (e != paNoError) return push_pa_err(S, e);

  vl_push_int(S, (int64_t)frames);
  return 1;
}

// --- Infos runtime ---

static int vla_time(VL_State *S) {
  int id = (int)au_check_int(S, 1);
  if (!check_id(id)) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  const PaStreamInfo *si = Pa_GetStreamInfo(g_as[id].st);
  if (!si) {
    vl_push_nil(S);
    vl_push_string(S, "EIO");
    return 2;
  }
  vl_push_float(S, Pa_GetStreamTime(g_as[id].st));
  return 1;
}

static int vla_cpu(VL_State *S) {
  int id = (int)au_check_int(S, 1);
  if (!check_id(id)) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  vl_push_float(S, Pa_GetStreamCpuLoad(g_as[id].st) * 100.0);
  return 1;
}

static int vla_avr(VL_State *S) {
  int id = (int)au_check_int(S, 1);
  if (!check_id(id)) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  long v = Pa_GetStreamReadAvailable(g_as[id].st);
  if (v < 0) return push_pa_err(S, (PaError)v);
  vl_push_int(S, (int64_t)v);
  return 1;
}
static int vla_avw(VL_State *S) {
  int id = (int)au_check_int(S, 1);
  if (!check_id(id)) {
    vl_push_nil(S);
    vl_push_string(S, "EINVAL");
    return 2;
  }
  long v = Pa_GetStreamWriteAvailable(g_as[id].st);
  if (v < 0) return push_pa_err(S, (PaError)v);
  vl_push_int(S, (int64_t)v);
  return 1;
}

#endif  // VL_HAVE_PORTAUDIO

// ---------------------------------------------------------------------
// Registration VM
// ---------------------------------------------------------------------
static const VL_Reg audiolib[] = {{"init", vla_init},
                                  {"terminate", vla_term},

                                  {"device_count", vla_dev_count},
                                  {"default_input_device", vla_def_in},
                                  {"default_output_device", vla_def_out},
                                  {"device_info", vla_dev_info},

                                  {"open", vla_open},
                                  {"start", vla_start},
                                  {"stop", vla_stop},
                                  {"close", vla_close},

                                  {"read", vla_read},
                                  {"write", vla_write},

                                  {"time", vla_time},
                                  {"cpu_load", vla_cpu},
                                  {"available_read", vla_avr},
                                  {"available_write", vla_avw},

                                  {NULL, NULL}};

void vl_open_audiolib(VL_State *S) { vl_register_lib(S, "audio", audiolib); }
