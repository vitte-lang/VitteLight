// SPDX-License-Identifier: GPL-3.0-or-later
//
// ffmpeg.c — Thin FFmpeg adapter for Vitte Light (C17)
// Namespace: "ffmpeg"
//
// Build options:
//   // Probe via libav* when dev libs are present:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -DVL_HAVE_FFMPEGLIB \
//      -lavformat -lavcodec -lavutil -lswresample -lswscale -c ffmpeg.c
//
//   // Otherwise falls back to spawning the `ffmpeg` CLI binary:
//   cc -std=c17 -O2 -Wall -Wextra -pedantic -c ffmpeg.c
//
// Provides:
//   typedef struct {
//     int has_video, has_audio;
//     int width, height;
//     int audio_sr, audio_ch;
//     double duration_sec;
//     char vcodec[64], acodec[64];
//   } ff_info;
//
//   // Probe media (prefers libav*, else runs `ffprobe` if available, else zeroes):
//   int  ff_probe(const char* in_path, ff_info* out);
//
//   // Common actions via CLI (works without libav*):
//   int  ff_extract_wav(const char* in_path, const char* out_wav, int sr, int ch);
//   int  ff_screenshot_png(const char* in_path, const char* out_png, double t_sec, int w, int h);
//   int  ff_transcode_h264_aac_mp4(const char* in_path, const char* out_mp4,
//                                  int v_bitrate_k, int a_bitrate_k);
//
// Notes:
//   - CLI functions require `ffmpeg` in PATH. Return -1 if not found or command fails.
//   - Paths are quoted. Basic escaping only. Avoid untrusted inputs for absolute safety.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #define SLASH '\\'
#else
  #include <unistd.h>
  #define SLASH '/'
#endif

// ---------------- Data ----------------
typedef struct {
    int has_video, has_audio;
    int width, height;
    int audio_sr, audio_ch;
    double duration_sec;
    char vcodec[64], acodec[64];
} ff_info;

// ---------------- Utilities ----------------

static int file_exists(const char* p) {
    if (!p || !*p) return 0;
    FILE* f = fopen(p, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static int which_in_path(const char* exe) {
#if defined(_WIN32)
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "where %s >NUL 2>NUL", exe);
    return system(cmd) == 0;
#else
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "command -v %s >/dev/null 2>&1", exe);
    return system(cmd) == 0;
#endif
}

static void quote_path(char* out, size_t n, const char* in) {
#if defined(_WIN32)
    // "C:\path with spaces\file.ext"
    snprintf(out, n, "\"%s\"", in ? in : "");
#else
    // 'path with spaces' (escape single quotes)
    if (!in) { snprintf(out, n, "''"); return; }
    size_t used = 0;
    out[0] = '\0';
    if (used + 1 < n) { out[used++] = '\''; out[used] = '\0'; }
    for (const char* p = in; *p && used + 4 < n; ++p) {
        if (*p == '\'') { // close, escape, reopen
            used += snprintf(out + used, n - used, "'\\''");
        } else {
            out[used++] = *p; out[used] = '\0';
        }
    }
    if (used + 2 < n) { out[used++] = '\''; out[used] = '\0'; }
#endif
}

static int run_cmd(const char* cmdline) {
#if defined(_WIN32)
    // Use system() for simplicity. Non-zero ⇒ failure.
    int rc = system(cmdline);
    return rc == 0 ? 0 : -1;
#else
    int rc = system(cmdline);
    if (rc == -1) return -1;
    if (WIFEXITED(rc) && WEXITSTATUS(rc) == 0) return 0;
    return -1;
#endif
}

// ---------------- Probe ----------------

#if defined(VL_HAVE_FFMPEGLIB)
#  include <libavformat/avformat.h>
#  include <libavcodec/avcodec.h>
#  include <libavutil/avutil.h>

int ff_probe(const char* in_path, ff_info* out) {
    if (!in_path || !out) { errno = EINVAL; return -1; }
    memset(out, 0, sizeof(*out));
    AVFormatContext* fmt = NULL;
#if LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(58,9,100)
    if (avformat_open_input(&fmt, in_path, NULL, NULL) < 0) return -1;
#else
    if (av_open_input_file(&fmt, in_path, NULL, 0, NULL) < 0) return -1;
#endif
    if (avformat_find_stream_info(fmt, NULL) < 0) { avformat_close_input(&fmt); return -1; }

    out->duration_sec = (fmt->duration > 0) ? (fmt->duration / (double)AV_TIME_BASE) : 0.0;

    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        AVStream* st = fmt->streams[i];
        AVCodecParameters* p = st->codecpar;
        if (p->codec_type == AVMEDIA_TYPE_VIDEO && !out->has_video) {
            out->has_video = 1;
            out->width = p->width;
            out->height = p->height;
            const AVCodecDescriptor* d = avcodec_descriptor_get(p->codec_id);
            snprintf(out->vcodec, sizeof(out->vcodec), "%s", d && d->name ? d->name : "unknown");
        } else if (p->codec_type == AVMEDIA_TYPE_AUDIO && !out->has_audio) {
            out->has_audio = 1;
            out->audio_sr = p->sample_rate;
            out->audio_ch = p->channels;
            const AVCodecDescriptor* d = avcodec_descriptor_get(p->codec_id);
            snprintf(out->acodec, sizeof(out->acodec), "%s", d && d->name ? d->name : "unknown");
        }
    }
    avformat_close_input(&fmt);
    return 0;
}

#else

// Fallback: try `ffprobe` CLI; if missing, return zeros with success=0 but not error.
int ff_probe(const char* in_path, ff_info* out) {
    if (!in_path || !out) { errno = EINVAL; return -1; }
    memset(out, 0, sizeof(*out));

    if (!which_in_path("ffprobe")) return 0; // not an error, just no data
    char qi[1024]; quote_path(qi, sizeof(qi), in_path);

    // duration (seconds)
#if defined(_WIN32)
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
        "ffprobe -v error -show_entries format=duration -of default=nw=1:nk=1 %s > ffprobe_dur.txt 2>NUL",
        qi);
    if (run_cmd(cmd) == 0 && file_exists("ffprobe_dur.txt")) {
        FILE* f = fopen("ffprobe_dur.txt", "rb");
        if (f) { double d=0; if (fscanf(f, "%lf", &d)==1) out->duration_sec = d; fclose(f); remove("ffprobe_dur.txt"); }
    }
#else
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
        "ffprobe -v error -show_entries format=duration -of default=nw=1:nk=1 %s > /tmp/ffprobe_dur.$$ 2>/dev/null",
        qi);
    if (run_cmd(cmd) == 0) {
        char tmp[64]; snprintf(tmp, sizeof(tmp), "/tmp/ffprobe_dur.%d", getpid());
        FILE* f = fopen(tmp, "rb");
        if (f) { double d=0; if (fscanf(f, "%lf", &d)==1) out->duration_sec = d; fclose(f); remove(tmp); }
    }
#endif

    // codecs and dimensions: best-effort using -show_streams JSON simplified
#if defined(_WIN32)
    snprintf(cmd, sizeof(cmd),
        "ffprobe -v error -select_streams v:0 -show_entries stream=codec_name,width,height "
        "-of default=nw=1:nk=1 %s > ffprobe_v.txt 2>NUL", qi);
    if (run_cmd(cmd) == 0 && file_exists("ffprobe_v.txt")) {
        FILE* f = fopen("ffprobe_v.txt", "rb");
        if (f) {
            char vc[64]={0}; int w=0,h=0;
            if (fscanf(f, "%63s %d %d", vc, &w, &h) >= 1) {
                out->has_video = 1; out->width = w; out->height = h;
                snprintf(out->vcodec, sizeof(out->vcodec), "%s", vc);
            }
            fclose(f); remove("ffprobe_v.txt");
        }
    }
    snprintf(cmd, sizeof(cmd),
        "ffprobe -v error -select_streams a:0 -show_entries stream=codec_name,sample_rate,channels "
        "-of default=nw=1:nk=1 %s > ffprobe_a.txt 2>NUL", qi);
    if (run_cmd(cmd) == 0 && file_exists("ffprobe_a.txt")) {
        FILE* f = fopen("ffprobe_a.txt", "rb");
        if (f) {
            char ac[64]={0}; int sr=0,ch=0;
            if (fscanf(f, "%63s %d %d", ac, &sr, &ch) >= 1) {
                out->has_audio = 1; out->audio_sr = sr; out->audio_ch = ch;
                snprintf(out->acodec, sizeof(out->acodec), "%s", ac);
            }
            fclose(f); remove("ffprobe_a.txt");
        }
    }
#else
    snprintf(cmd, sizeof(cmd),
        "ffprobe -v error -select_streams v:0 -show_entries stream=codec_name,width,height "
        "-of default=nw=1:nk=1 %s > /tmp/ffprobe_v.$$ 2>/dev/null", qi);
    if (run_cmd(cmd) == 0) {
        char tmpv[64]; snprintf(tmpv, sizeof(tmpv), "/tmp/ffprobe_v.%d", getpid());
        FILE* f = fopen(tmpv, "rb");
        if (f) {
            char vc[64]={0}; int w=0,h=0;
            if (fscanf(f, "%63s %d %d", vc, &w, &h) >= 1) {
                out->has_video = 1; out->width = w; out->height = h;
                snprintf(out->vcodec, sizeof(out->vcodec), "%s", vc);
            }
            fclose(f); remove(tmpv);
        }
    }
    snprintf(cmd, sizeof(cmd),
        "ffprobe -v error -select_streams a:0 -show_entries stream=codec_name,sample_rate,channels "
        "-of default=nw=1:nk=1 %s > /tmp/ffprobe_a.$$ 2>/dev/null", qi);
    if (run_cmd(cmd) == 0) {
        char tmpa[64]; snprintf(tmpa, sizeof(tmpa), "/tmp/ffprobe_a.%d", getpid());
        FILE* f = fopen(tmpa, "rb");
        if (f) {
            char ac[64]={0}; int sr=0,ch=0;
            if (fscanf(f, "%63s %d %d", ac, &sr, &ch) >= 1) {
                out->has_audio = 1; out->audio_sr = sr; out->audio_ch = ch;
                snprintf(out->acodec, sizeof(out->acodec), "%s", ac);
            }
            fclose(f); remove(tmpa);
        }
    }
#endif
    return 0;
}
#endif

// ---------------- CLI Actions ----------------

int ff_extract_wav(const char* in_path, const char* out_wav, int sr, int ch) {
    if (!in_path || !out_wav) { errno = EINVAL; return -1; }
    if (!which_in_path("ffmpeg")) { errno = ENOENT; return -1; }
    if (sr <= 0) sr = 48000;
    if (ch <= 0) ch = 2;

    char qi[1024], qo[1024];
    quote_path(qi, sizeof(qi), in_path);
    quote_path(qo, sizeof(qo), out_wav);

    char cmd[4096];
#if defined(_WIN32)
    snprintf(cmd, sizeof(cmd), "ffmpeg -y -i %s -vn -ac %d -ar %d -f wav %s 1>NUL 2>NUL",
             qi, ch, sr, qo);
#else
    snprintf(cmd, sizeof(cmd), "ffmpeg -y -i %s -vn -ac %d -ar %d -f wav %s >/dev/null 2>&1",
             qi, ch, sr, qo);
#endif
    return run_cmd(cmd);
}

int ff_screenshot_png(const char* in_path, const char* out_png, double t_sec, int w, int h) {
    if (!in_path || !out_png) { errno = EINVAL; return -1; }
    if (!which_in_path("ffmpeg")) { errno = ENOENT; return -1; }
    if (t_sec < 0) t_sec = 0;
    if (w <= 0 && h <= 0) { w = 1280; h = -1; } // keep aspect

    char qi[1024], qo[1024];
    quote_path(qi, sizeof(qi), in_path);
    quote_path(qo, sizeof(qo), out_png);

    char scale[64];
    if (w > 0 && h > 0) snprintf(scale, sizeof(scale), "scale=%d:%d", w, h);
    else if (w > 0)     snprintf(scale, sizeof(scale), "scale=%d:-1", w);
    else                snprintf(scale, sizeof(scale), "scale=-1:%d", h);

    char cmd[4096];
#if defined(_WIN32)
    snprintf(cmd, sizeof(cmd),
        "ffmpeg -y -ss %.3f -i %s -frames:v 1 -vf %s -f image2 %s 1>NUL 2>NUL",
        t_sec, qi, scale, qo);
#else
    snprintf(cmd, sizeof(cmd),
        "ffmpeg -y -ss %.3f -i %s -frames:v 1 -vf %s -f image2 %s >/dev/null 2>&1",
        t_sec, qi, scale, qo);
#endif
    return run_cmd(cmd);
}

int ff_transcode_h264_aac_mp4(const char* in_path, const char* out_mp4,
                              int v_bitrate_k, int a_bitrate_k)
{
    if (!in_path || !out_mp4) { errno = EINVAL; return -1; }
    if (!which_in_path("ffmpeg")) { errno = ENOENT; return -1; }
    if (v_bitrate_k <= 0) v_bitrate_k = 3000;
    if (a_bitrate_k <= 0) a_bitrate_k = 160;

    char qi[1024], qo[1024];
    quote_path(qi, sizeof(qi), in_path);
    quote_path(qo, sizeof(qo), out_mp4);

    char cmd[4096];
#if defined(_WIN32)
    snprintf(cmd, sizeof(cmd),
        "ffmpeg -y -i %s -c:v libx264 -preset veryfast -b:v %dk "
        "-movflags +faststart -c:a aac -b:a %dk %s 1>NUL 2>NUL",
        qi, v_bitrate_k, a_bitrate_k, qo);
#else
    snprintf(cmd, sizeof(cmd),
        "ffmpeg -y -i %s -c:v libx264 -preset veryfast -b:v %dk "
        "-movflags +faststart -c:a aac -b:a %dk %s >/dev/null 2>&1",
        qi, v_bitrate_k, a_bitrate_k, qo);
#endif
    return run_cmd(cmd);
}

// ---------------- Demo ----------------
#ifdef FFMPEG_DEMO
#include <stdio.h>
int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s INPUT\n", argv[0]); return 2; }
    const char* in = argv[1];
    ff_info info;
    if (ff_probe(in, &info) == 0) {
        printf("has_video=%d %dx%d vcodec=%s | has_audio=%d %dHz %dch acodec=%s | duration=%.3f\n",
               info.has_video, info.width, info.height, info.vcodec,
               info.has_audio, info.audio_sr, info.audio_ch, info.acodec,
               info.duration_sec);
    } else {
        perror("ff_probe");
    }
    ff_screenshot_png(in, "frame.png", 1.23, 1280, -1);
    ff_extract_wav(in, "audio.wav", 48000, 2);
    ff_transcode_h264_aac_mp4(in, "out.mp4", 2500, 128);
    return 0;
}
#endif