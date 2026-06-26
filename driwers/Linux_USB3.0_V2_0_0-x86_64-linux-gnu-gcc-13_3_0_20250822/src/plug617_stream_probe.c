#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "guideusbcamera.h"

#define DEFAULT_DEVICE "/dev/video0"
#define DEFAULT_WIDTH 640
#define DEFAULT_HEIGHT 512
#define DEFAULT_MODE 0
#define DEFAULT_VERSION 1
#define DEFAULT_FRAMES 100
#define DEFAULT_TIMEOUT_SEC 10
#define DEFAULT_SAVE_FRAMES 5
#define DEFAULT_LOG_LEVEL 15
#define DEFAULT_PREVIEW 0
#define DEFAULT_PREVIEW_FPS 25
#define DEFAULT_PREVIEW_FORMAT "auto"
#define DEFAULT_PREVIEW_LENGTH_UNITS "auto"
#define DEFAULT_PREVIEW_NORMALIZE 1
#define DEFAULT_PREVIEW_PALETTE "gray"
#define CLOSE_WATCHDOG_SEC 5

typedef struct {
    const char *device;
    const char *preview_format;
    const char *preview_length_units;
    const char *preview_palette;
    char out_dir[PATH_MAX];
    int width;
    int height;
    int mode;
    int version;
    int frames;
    int timeout_sec;
    int save_frames;
    int log_level;
    int preview;
    int preview_fps;
    int preview_normalize;
} probe_config_t;

static probe_config_t g_cfg;
static pthread_mutex_t g_state_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_preview_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_frame_count = 0;
static FILE *g_preview_pipe = NULL;
static int g_preview_disabled = 0;
static int g_preview_width = 0;
static int g_preview_height = 0;
static char g_preview_pixel_format[32] = {0};
static int g_preview_length_warning_printed = 0;
static int g_preview_no_yuv_warning_printed = 0;
static int g_preview_zero_warning_printed = 0;
static volatile sig_atomic_t g_stop_requested = 0;
static volatile sig_atomic_t g_signal_number = 0;
static volatile sig_atomic_t g_signal_count = 0;

static void usage(const char *argv0)
{
    printf("Usage: %s [options]\n", argv0);
    printf("\nOptions:\n");
    printf("  --device /dev/videoX      V4L2 device path (default: %s)\n", DEFAULT_DEVICE);
    printf("  --width N                 Frame width (default: %d)\n", DEFAULT_WIDTH);
    printf("  --height N                Frame height (default: %d)\n", DEFAULT_HEIGHT);
    printf("  --mode N                  SDK video mode index (default: %d)\n", DEFAULT_MODE);
    printf("  --version N               SDK device version: 1, 2, or 3 (default: %d)\n", DEFAULT_VERSION);
    printf("  --frames N                Stop after N frames (default: %d)\n", DEFAULT_FRAMES);
    printf("  --timeout-sec N           Stop after N seconds, 0 disables timeout (default: %d)\n", DEFAULT_TIMEOUT_SEC);
    printf("  --out DIR                 Output directory (default: outputs/run_YYYYmmdd_HHMMSS)\n");
    printf("  --save-frames N           Save raw data for first N frames (default: %d)\n", DEFAULT_SAVE_FRAMES);
    printf("  --log-level N             SDK log level (default: %d)\n", DEFAULT_LOG_LEVEL);
    printf("  --preview                 Open live preview using ffplay (default: off)\n");
    printf("  --no-preview              Disable live preview\n");
    printf("  --preview-fps N           Preview frame rate passed to ffplay (default: %d)\n", DEFAULT_PREVIEW_FPS);
    printf("  --preview-format FMT      auto, gray8, uyvy-luma, yuyv-luma, yuyv422, uyvy422, gray16le, or gray16be (default: %s)\n", DEFAULT_PREVIEW_FORMAT);
    printf("  --preview-length-units U  auto, bytes, or shorts (default: %s)\n", DEFAULT_PREVIEW_LENGTH_UNITS);
    printf("  --preview-palette P       gray or blue-red (default: %s)\n", DEFAULT_PREVIEW_PALETTE);
    printf("  --preview-normalize       Stretch gray16 preview contrast in the runner (default: on)\n");
    printf("  --no-preview-normalize    Disable preview contrast normalization\n");
    printf("  --help                    Show this help\n");
}

static void signal_handler(int signum)
{
    g_signal_number = signum;
    g_stop_requested = 1;
    g_signal_count++;
    if (g_signal_count >= 2) {
        const char msg[] = "\nSecond signal received, exiting immediately.\n";
        write(STDERR_FILENO, msg, sizeof(msg) - 1);
        _exit(128 + signum);
    }
}

static void alarm_handler(int signum)
{
    (void)signum;
    {
        const char msg[] = "\nTimed out while closing SDK stream, exiting immediately.\n";
        write(STDERR_FILENO, msg, sizeof(msg) - 1);
    }
    _exit(124);
}

static int parse_int_arg(const char *name, const char *value, int *out)
{
    char *end = NULL;
    long parsed;

    errno = 0;
    parsed = strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed < INT_MIN || parsed > INT_MAX) {
        fprintf(stderr, "Invalid integer for %s: %s\n", name, value);
        return -1;
    }

    *out = (int)parsed;
    return 0;
}

static int build_default_out_dir(char *buf, size_t size)
{
    time_t now = time(NULL);
    struct tm tm_now;

    if (now == (time_t)-1) {
        perror("time");
        return -1;
    }

    if (localtime_r(&now, &tm_now) == NULL) {
        perror("localtime_r");
        return -1;
    }

    if (strftime(buf, size, "outputs/run_%Y%m%d_%H%M%S", &tm_now) == 0) {
        fprintf(stderr, "Failed to build default output directory path\n");
        return -1;
    }

    return 0;
}

static int mkdir_p(const char *path)
{
    char tmp[PATH_MAX];
    size_t len;
    char *p;

    if (path == NULL || path[0] == '\0') {
        fprintf(stderr, "Output directory path is empty\n");
        return -1;
    }

    if (snprintf(tmp, sizeof(tmp), "%s", path) >= (int)sizeof(tmp)) {
        fprintf(stderr, "Output directory path is too long: %s\n", path);
        return -1;
    }

    len = strlen(tmp);
    if (len > 1 && tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }

    for (p = tmp + 1; *p != '\0'; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0775) != 0 && errno != EEXIST) {
                fprintf(stderr, "mkdir failed for %s: %s\n", tmp, strerror(errno));
                return -1;
            }
            *p = '/';
        }
    }

    if (mkdir(tmp, 0775) != 0 && errno != EEXIST) {
        fprintf(stderr, "mkdir failed for %s: %s\n", tmp, strerror(errno));
        return -1;
    }

    return 0;
}

static int make_output_path(char *buf, size_t size, const char *filename)
{
    int written = snprintf(buf, size, "%s/%s", g_cfg.out_dir, filename);
    if (written < 0 || written >= (int)size) {
        fprintf(stderr, "Output path is too long for file: %s\n", filename);
        return -1;
    }
    return 0;
}

static int save_bytes_file(const char *path, const short *data, int length_value, const char *label)
{
    FILE *fp;
    size_t bytes;
    size_t written;

    if (length_value <= 0) {
        return 0;
    }

    if (data == NULL) {
        fprintf(stderr, "WARNING: %s length is %d but pointer is NULL, skipping save\n", label, length_value);
        return -1;
    }

    bytes = (size_t)length_value;
    fp = fopen(path, "wb");
    if (fp == NULL) {
        fprintf(stderr, "Failed to open %s for writing: %s\n", path, strerror(errno));
        return -1;
    }

    written = fwrite((const void *)data, 1, bytes, fp);
    if (written != bytes) {
        fprintf(stderr, "Failed to write %s completely: wrote %zu of %zu bytes\n", path, written, bytes);
        fclose(fp);
        return -1;
    }

    if (fclose(fp) != 0) {
        fprintf(stderr, "Failed to close %s: %s\n", path, strerror(errno));
        return -1;
    }

    printf("saved %s: %zu bytes -> %s\n", label, bytes, path);
    return 0;
}

static uint16_t param_word(const short *data, size_t index)
{
    return (uint16_t)data[index];
}

static void write_param_value(FILE *fp, const short *data, size_t word_count, size_t index, const char *name)
{
    if (index < word_count) {
        fprintf(fp, "%s[%zu]=0x%04X (%u)\n", name, index, param_word(data, index), param_word(data, index));
    } else {
        fprintf(fp, "%s[%zu]=unavailable\n", name, index);
    }
}

static void write_param_scaled_u16(FILE *fp, const short *data, size_t word_count, size_t index, const char *name)
{
    if (index < word_count) {
        uint16_t raw = param_word(data, index);
        fprintf(fp, "%s[%zu]=0x%04X (%u), scaled=/10 => %.1f\n", name, index, raw, raw, raw / 10.0);
    } else {
        fprintf(fp, "%s[%zu]=unavailable\n", name, index);
    }
}

static void write_param_temp(FILE *fp, const short *data, size_t word_count, size_t index, const char *name)
{
    if (index < word_count) {
        uint16_t raw_u16 = param_word(data, index);
        int16_t raw_i16 = (int16_t)data[index];
        fprintf(fp, "%s[%zu]=0x%04X (u16=%u, i16=%d), signed_scaled=/10 => %.1f\n",
                name, index, raw_u16, raw_u16, raw_i16, raw_i16 / 10.0);
    } else {
        fprintf(fp, "%s[%zu]=unavailable\n", name, index);
    }
}

static int save_param_txt(const char *path, const short *data, int length_value, int frame_no)
{
    FILE *fp;
    size_t word_count;
    size_t i;
    int found_end = 0;

    if (length_value <= 0 || data == NULL) {
        return 0;
    }

    word_count = (size_t)length_value / sizeof(short);

    fp = fopen(path, "w");
    if (fp == NULL) {
        fprintf(stderr, "Failed to open %s for writing: %s\n", path, strerror(errno));
        return -1;
    }

    fprintf(fp, "frame=%d\n", frame_no);
    fprintf(fp, "paramLine_length_raw=%d\n", length_value);
    fprintf(fp, "byte_safe_word_count=%zu\n", word_count);
    fprintf(fp, "\n");
    fprintf(fp, "NOTE: SDK Demo.c does not show how *_length is measured.\n");
    fprintf(fp, "This tool interprets length values as bytes for raw saving and parsing to avoid over-reading SDK buffers.\n");
    fprintf(fp, "If this SDK reports paramLine_length in 16-bit words, the text parse can be intentionally conservative.\n");
    fprintf(fp, "\n");

    write_param_value(fp, data, word_count, 0, "head1");
    write_param_value(fp, data, word_count, 1, "head2");
    write_param_scaled_u16(fp, data, word_count, 3, "distance");
    write_param_value(fp, data, word_count, 4, "emissivity");
    write_param_value(fp, data, word_count, 5, "reflectivity");
    write_param_value(fp, data, word_count, 28, "shutter_status");
    write_param_value(fp, data, word_count, 44, "hot_spot_x");
    write_param_value(fp, data, word_count, 45, "hot_spot_y");
    write_param_temp(fp, data, word_count, 46, "hot_spot_temperature");
    write_param_value(fp, data, word_count, 47, "cold_spot_x");
    write_param_value(fp, data, word_count, 48, "cold_spot_y");
    write_param_temp(fp, data, word_count, 49, "cold_spot_temperature");
    write_param_value(fp, data, word_count, 50, "cursor_x");
    write_param_value(fp, data, word_count, 51, "cursor_y");
    write_param_temp(fp, data, word_count, 52, "cursor_temperature");
    write_param_temp(fp, data, word_count, 53, "regional_mean_temperature");

    fprintf(fp, "\n");
    fprintf(fp, "frame_end_0x6666_positions=");
    for (i = 0; i < word_count; ++i) {
        if (param_word(data, i) == 0x6666) {
            fprintf(fp, "%s%zu", found_end ? "," : "", i);
            found_end = 1;
        }
    }
    if (!found_end) {
        fprintf(fp, "not_found");
    }
    fprintf(fp, "\n");

    if (fclose(fp) != 0) {
        fprintf(stderr, "Failed to close %s: %s\n", path, strerror(errno));
        return -1;
    }

    printf("saved param txt -> %s\n", path);
    return 0;
}

static int command_available(const char *command)
{
    char check_cmd[128];

    if (snprintf(check_cmd, sizeof(check_cmd), "command -v %s >/dev/null 2>&1", command) >= (int)sizeof(check_cmd)) {
        return 0;
    }

    return system(check_cmd) == 0;
}

static const char *normalize_preview_format(const char *format)
{
    if (strcmp(format, "gray8") == 0) {
        return "gray";
    }
    return format;
}

static size_t preview_expected_bytes(const char *pixel_format, int width, int height)
{
    size_t pixels = (size_t)width * (size_t)height;

    if (strcmp(pixel_format, "gray") == 0) {
        return pixels;
    }
    if (strcmp(pixel_format, "gray16le") == 0 || strcmp(pixel_format, "gray16be") == 0) {
        return pixels * 2U;
    }
    if (strcmp(pixel_format, "uyvy-luma") == 0 || strcmp(pixel_format, "yuyv-luma") == 0) {
        return pixels;
    }
    if (strcmp(pixel_format, "yuyv422") == 0 || strcmp(pixel_format, "uyvy422") == 0) {
        return pixels * 2U;
    }

    return 0;
}

static int looks_like_gray16be(const unsigned char *data, size_t available_bytes)
{
    size_t pairs = available_bytes / 2U;
    size_t i;
    int high_min = 255;
    int high_max = 0;
    int low_min = 255;
    int low_max = 0;
    size_t plausible_high = 0;

    if (pairs > 512U) {
        pairs = 512U;
    }
    if (pairs < 32U) {
        return 0;
    }

    for (i = 0; i < pairs; ++i) {
        int high = data[i * 2U];
        int low = data[i * 2U + 1U];

        if (high < high_min) high_min = high;
        if (high > high_max) high_max = high;
        if (low < low_min) low_min = low;
        if (low > low_max) low_max = low;
        if (high >= 1 && high <= 63) plausible_high++;
    }

    return plausible_high * 100U / pairs >= 85U &&
           high_max - high_min <= 24 &&
           low_max - low_min >= 16;
}

static int looks_like_uyvy_luma(const unsigned char *data, size_t available_bytes)
{
    size_t samples = available_bytes;
    size_t i;
    int even_min = 255;
    int even_max = 0;
    int odd_min = 255;
    int odd_max = 0;
    size_t even_n = 0;
    size_t odd_n = 0;

    if (samples > 4096U) {
        samples = 4096U;
    }
    if (samples < 64U) {
        return 0;
    }

    for (i = 0; i < samples; ++i) {
        int v = data[i];
        if ((i & 1U) == 0U) {
            if (v < even_min) even_min = v;
            if (v > even_max) even_max = v;
            even_n++;
        } else {
            if (v < odd_min) odd_min = v;
            if (v > odd_max) odd_max = v;
            odd_n++;
        }
    }

    return even_n > 0 &&
           odd_n > 0 &&
           even_max - even_min <= 8 &&
           odd_max - odd_min >= 32;
}

static int select_preview_format_and_size(guide_usb_frame_data_t *frame,
                                          int width,
                                          int height,
                                          const char **pixel_format,
                                          size_t *bytes_to_write)
{
    const char *requested_format = normalize_preview_format(g_cfg.preview_format);
    size_t reported_length;
    size_t byte_capacity;
    size_t short_capacity;
    size_t pixels;
    size_t expected;

    reported_length = (size_t)frame->frame_yuv_data_length;
    byte_capacity = reported_length;
    short_capacity = reported_length <= SIZE_MAX / sizeof(short) ? reported_length * sizeof(short) : 0;
    pixels = (size_t)width * (size_t)height;

    if (strcmp(requested_format, "auto") == 0) {
        if (byte_capacity == pixels) {
            if (short_capacity >= pixels * 2U &&
                looks_like_uyvy_luma((const unsigned char *)frame->frame_yuv_data, byte_capacity)) {
                if (!g_preview_length_warning_printed) {
                    fprintf(stderr,
                            "Detected UYVY-like data with length reported as %zu shorts; using uyvy-luma preview\n",
                            reported_length);
                    g_preview_length_warning_printed = 1;
                }
                *pixel_format = "uyvy-luma";
                *bytes_to_write = byte_capacity;
                return 0;
            }
            if (short_capacity >= pixels * 2U &&
                looks_like_gray16be((const unsigned char *)frame->frame_yuv_data, byte_capacity)) {
                if (!g_preview_length_warning_printed) {
                    fprintf(stderr,
                            "Detected 16-bit big-endian grayscale data with length reported as %zu shorts; using gray16be preview\n",
                            reported_length);
                    g_preview_length_warning_printed = 1;
                }
                *pixel_format = "gray16be";
                *bytes_to_write = pixels * 2U;
                return 0;
            }
            *pixel_format = "gray";
            *bytes_to_write = pixels;
            return 0;
        }
        if (byte_capacity >= pixels * 2U) {
            *pixel_format = "yuyv422";
            *bytes_to_write = pixels * 2U;
            return 0;
        }

        fprintf(stderr,
                "WARNING: cannot auto-select preview format: length=%d, expected gray8=%zu bytes or YUV422=%zu bytes\n",
                frame->frame_yuv_data_length, pixels, pixels * 2U);
        return -1;
    }

    if (strcmp(requested_format, "gray") != 0 &&
        strcmp(requested_format, "uyvy-luma") != 0 &&
        strcmp(requested_format, "yuyv-luma") != 0 &&
        strcmp(requested_format, "gray16le") != 0 &&
        strcmp(requested_format, "gray16be") != 0 &&
        strcmp(requested_format, "yuyv422") != 0 &&
        strcmp(requested_format, "uyvy422") != 0) {
        fprintf(stderr, "WARNING: unsupported preview format '%s'\n", g_cfg.preview_format);
        return -1;
    }

    expected = preview_expected_bytes(requested_format, width, height);
    if (expected == 0) {
        return -1;
    }

    if (strcmp(requested_format, "uyvy-luma") == 0 || strcmp(requested_format, "yuyv-luma") == 0) {
        if (byte_capacity >= pixels * 2U) {
            *pixel_format = requested_format;
            *bytes_to_write = pixels * 2U;
            return 0;
        }
        if (byte_capacity >= (size_t)width * 2U &&
            byte_capacity % ((size_t)width * 2U) == 0) {
            *pixel_format = requested_format;
            *bytes_to_write = byte_capacity;
            return 0;
        }

        fprintf(stderr,
                "WARNING: not enough packed YUV data for luma preview: length=%zu bytes, need at least one %d-pixel row (%zu bytes)\n",
                byte_capacity, width, (size_t)width * 2U);
        return -1;
    }

    if (strcmp(g_cfg.preview_length_units, "bytes") == 0) {
        if (byte_capacity < expected) {
            fprintf(stderr,
                    "WARNING: not enough preview data for %s: length=%zu bytes, expected=%zu bytes\n",
                    requested_format, byte_capacity, expected);
            return -1;
        }
        *pixel_format = requested_format;
        *bytes_to_write = expected;
        return 0;
    }

    if (strcmp(g_cfg.preview_length_units, "shorts") == 0) {
        if (short_capacity < expected) {
            fprintf(stderr,
                    "WARNING: not enough preview data for %s: length=%zu shorts, expected=%zu bytes\n",
                    requested_format, reported_length, expected);
            return -1;
        }
        *pixel_format = requested_format;
        *bytes_to_write = expected;
        return 0;
    }

    if (strcmp(g_cfg.preview_length_units, "auto") != 0) {
        fprintf(stderr, "WARNING: unsupported preview length units '%s'\n", g_cfg.preview_length_units);
        return -1;
    }

    if (byte_capacity >= expected) {
        *pixel_format = requested_format;
        *bytes_to_write = expected;
        return 0;
    }

    if (short_capacity >= expected) {
        if (!g_preview_length_warning_printed) {
            fprintf(stderr,
                    "WARNING: length=%d is smaller than %zu bytes; treating preview length as 16-bit words because format was explicitly set to %s\n",
                    frame->frame_yuv_data_length, expected, requested_format);
            g_preview_length_warning_printed = 1;
        }
        *pixel_format = requested_format;
        *bytes_to_write = expected;
        return 0;
    }

    fprintf(stderr,
            "WARNING: not enough preview data for %s: length=%d, expected=%zu bytes\n",
            requested_format, frame->frame_yuv_data_length, expected);
    return -1;
}

static int preview_open_locked(int width, int height, const char *pixel_format)
{
    char command[512];
    FILE *old_pipe;

    if (!g_cfg.preview || g_preview_disabled) {
        return -1;
    }

    if (g_preview_pipe != NULL &&
        g_preview_width == width &&
        g_preview_height == height &&
        strcmp(g_preview_pixel_format, pixel_format) == 0) {
        return 0;
    }

    old_pipe = g_preview_pipe;
    g_preview_pipe = NULL;
    if (old_pipe != NULL) {
        pclose(old_pipe);
    }

    if (!command_available("ffplay")) {
        fprintf(stderr, "WARNING: ffplay is not installed, live preview is disabled. Try: sudo apt install ffmpeg\n");
        g_preview_disabled = 1;
        return -1;
    }

    if (width <= 0 || height <= 0) {
        width = g_cfg.width;
        height = g_cfg.height;
    }

    g_preview_width = width;
    g_preview_height = height;
    snprintf(g_preview_pixel_format, sizeof(g_preview_pixel_format), "%s", pixel_format);

    if (snprintf(command, sizeof(command),
                 "ffplay -loglevel warning -fflags nobuffer -flags low_delay "
                 "-f rawvideo -pixel_format %s -video_size %dx%d -framerate %d -i -",
                 g_preview_pixel_format,
                 g_preview_width,
                 g_preview_height,
                 g_cfg.preview_fps) >= (int)sizeof(command)) {
        fprintf(stderr, "WARNING: ffplay command is too long, live preview is disabled\n");
        g_preview_disabled = 1;
        return -1;
    }

    printf("Opening live preview: %s\n", command);
    fflush(stdout);

    g_preview_pipe = popen(command, "w");
    if (g_preview_pipe == NULL) {
        fprintf(stderr, "WARNING: failed to start ffplay preview: %s\n", strerror(errno));
        g_preview_disabled = 1;
        return -1;
    }

    setvbuf(g_preview_pipe, NULL, _IONBF, 0);
    return 0;
}

static void preview_close(void)
{
    FILE *pipe_to_close = NULL;

    pthread_mutex_lock(&g_preview_lock);
    g_preview_disabled = 1;
    pipe_to_close = g_preview_pipe;
    g_preview_pipe = NULL;
    pthread_mutex_unlock(&g_preview_lock);

    if (pipe_to_close != NULL) {
        int ret = pclose(pipe_to_close);
        printf("ffplay preview closed, pclose returned %d\n", ret);
        fflush(stdout);
    }
}

static int convert_gray16_to_gray8(const unsigned char *src,
                                   size_t src_bytes,
                                   int width,
                                   int height,
                                   int big_endian,
                                   unsigned char **out_buf,
                                   size_t *out_bytes)
{
    size_t pixels = (size_t)width * (size_t)height;
    size_t i;
    uint16_t min_v = UINT16_MAX;
    uint16_t max_v = 0;
    unsigned char *dst;
    static int printed = 0;

    if (src_bytes < pixels * 2U) {
        fprintf(stderr, "WARNING: not enough gray16 data: have %zu bytes, need %zu bytes\n", src_bytes, pixels * 2U);
        return -1;
    }

    dst = (unsigned char *)malloc(pixels);
    if (dst == NULL) {
        fprintf(stderr, "WARNING: failed to allocate preview conversion buffer: %s\n", strerror(errno));
        return -1;
    }

    for (i = 0; i < pixels; ++i) {
        uint16_t v;
        if (big_endian) {
            v = (uint16_t)(((uint16_t)src[i * 2U] << 8) | src[i * 2U + 1U]);
        } else {
            v = (uint16_t)(((uint16_t)src[i * 2U + 1U] << 8) | src[i * 2U]);
        }

        if (v < min_v) min_v = v;
        if (v > max_v) max_v = v;
    }

    if (max_v == min_v) {
        memset(dst, 0, pixels);
    } else {
        uint32_t range = (uint32_t)max_v - (uint32_t)min_v;

        for (i = 0; i < pixels; ++i) {
            uint16_t v;
            if (big_endian) {
                v = (uint16_t)(((uint16_t)src[i * 2U] << 8) | src[i * 2U + 1U]);
            } else {
                v = (uint16_t)(((uint16_t)src[i * 2U + 1U] << 8) | src[i * 2U]);
            }

            if (g_cfg.preview_normalize) {
                dst[i] = (unsigned char)((((uint32_t)v - (uint32_t)min_v) * 255U) / range);
            } else {
                dst[i] = (unsigned char)(v >> 8);
            }
        }
    }

    if (!printed) {
        printf("gray16 preview conversion: endian=%s min=%u max=%u normalize=%s -> gray8\n",
               big_endian ? "big" : "little",
               (unsigned int)min_v,
               (unsigned int)max_v,
               g_cfg.preview_normalize ? "on" : "off");
        printed = 1;
    }

    *out_buf = dst;
    *out_bytes = pixels;
    return 0;
}

static int convert_packed_yuv_luma_to_gray8(const unsigned char *src,
                                            size_t src_bytes,
                                            int width,
                                            int height,
                                            int uyvy_order,
                                            unsigned char **out_buf,
                                            size_t *out_bytes)
{
    size_t pixels = (size_t)width * (size_t)height;
    size_t src_row_bytes = (size_t)width * 2U;
    size_t src_rows;
    size_t y;
    unsigned char *dst;
    static int printed = 0;

    if ((pixels & 1U) != 0U) {
        fprintf(stderr, "WARNING: cannot extract luma from odd pixel count: %zu\n", pixels);
        return -1;
    }

    if (src_row_bytes == 0 || src_bytes < src_row_bytes) {
        fprintf(stderr, "WARNING: not enough packed YUV data: have %zu bytes, need at least %zu bytes\n", src_bytes, src_row_bytes);
        return -1;
    }

    src_rows = src_bytes / src_row_bytes;
    if (src_rows == 0) {
        fprintf(stderr, "WARNING: packed YUV source has zero rows\n");
        return -1;
    }

    dst = (unsigned char *)malloc(pixels);
    if (dst == NULL) {
        fprintf(stderr, "WARNING: failed to allocate luma preview buffer: %s\n", strerror(errno));
        return -1;
    }

    for (y = 0; y < (size_t)height; ++y) {
        size_t src_y = y * src_rows / (size_t)height;
        const unsigned char *src_row = src + src_y * src_row_bytes;
        unsigned char *dst_row = dst + y * (size_t)width;
        size_t x;

        for (x = 0; x < (size_t)width; x += 2U) {
            const unsigned char *p = src_row + x * 2U;
            if (uyvy_order) {
                dst_row[x] = p[1];
                dst_row[x + 1U] = p[3];
            } else {
                dst_row[x] = p[0];
                dst_row[x + 1U] = p[2];
            }
        }
    }

    if (!printed) {
        printf("packed YUV preview conversion: order=%s src_rows=%zu display_rows=%d -> gray8 luma\n",
               uyvy_order ? "UYVY" : "YUYV", src_rows, height);
        printed = 1;
    }

    *out_buf = dst;
    *out_bytes = pixels;
    return 0;
}

static int buffer_is_all_zero(const unsigned char *data, size_t bytes)
{
    size_t i;

    for (i = 0; i < bytes; ++i) {
        if (data[i] != 0) {
            return 0;
        }
    }

    return 1;
}

static int convert_gray8_to_blue_red(const unsigned char *src,
                                     size_t src_bytes,
                                     int width,
                                     int height,
                                     unsigned char **out_buf,
                                     size_t *out_bytes)
{
    size_t pixels = (size_t)width * (size_t)height;
    size_t i;
    unsigned char *dst;
    static int printed = 0;

    if (src_bytes < pixels) {
        fprintf(stderr, "WARNING: not enough gray data for palette conversion: have %zu bytes, need %zu bytes\n",
                src_bytes, pixels);
        return -1;
    }

    if (pixels > SIZE_MAX / 3U) {
        fprintf(stderr, "WARNING: preview frame is too large for RGB palette conversion\n");
        return -1;
    }

    dst = (unsigned char *)malloc(pixels * 3U);
    if (dst == NULL) {
        fprintf(stderr, "WARNING: failed to allocate palette preview buffer: %s\n", strerror(errno));
        return -1;
    }

    for (i = 0; i < pixels; ++i) {
        unsigned int v = src[i];
        unsigned int green = v <= 127U ? v * 2U : (255U - v) * 2U;
        if (green > 255U) {
            green = 255U;
        }

        dst[i * 3U] = (unsigned char)v;
        dst[i * 3U + 1U] = (unsigned char)green;
        dst[i * 3U + 2U] = (unsigned char)(255U - v);
    }

    if (!printed) {
        printf("preview palette conversion: gray8 -> rgb24 blue-red\n");
        printed = 1;
    }

    *out_buf = dst;
    *out_bytes = pixels * 3U;
    return 0;
}

static int apply_preview_palette(const unsigned char *src,
                                 size_t src_bytes,
                                 int width,
                                 int height,
                                 const char **pixel_format,
                                 unsigned char **out_buf,
                                 size_t *out_bytes)
{
    if (strcmp(g_cfg.preview_palette, "gray") == 0) {
        return 0;
    }

    if (strcmp(*pixel_format, "gray") != 0) {
        fprintf(stderr,
                "WARNING: preview palette '%s' can only be applied to gray preview data; leaving %s unchanged\n",
                g_cfg.preview_palette, *pixel_format);
        return 0;
    }

    if (strcmp(g_cfg.preview_palette, "blue-red") == 0) {
        if (convert_gray8_to_blue_red(src, src_bytes, width, height, out_buf, out_bytes) != 0) {
            return -1;
        }
        *pixel_format = "rgb24";
        return 1;
    }

    fprintf(stderr, "WARNING: unsupported preview palette '%s'\n", g_cfg.preview_palette);
    return -1;
}

static void preview_frame(guide_usb_frame_data_t *frame)
{
    int width;
    int height;
    const char *pixel_format = NULL;
    const char *ffplay_pixel_format = NULL;
    size_t bytes_to_write = 0;
    const unsigned char *preview_data = NULL;
    unsigned char *converted_data = NULL;
    size_t ffplay_bytes = 0;
    size_t written;

    if (!g_cfg.preview || frame == NULL || g_preview_disabled) {
        return;
    }

    if (frame->frame_yuv_data == NULL || frame->frame_yuv_data_length <= 0) {
        if (!g_preview_no_yuv_warning_printed) {
            fprintf(stderr, "WARNING: live preview needs frame_yuv_data; current mode/frame has no YUV data\n");
            g_preview_no_yuv_warning_printed = 1;
        }
        return;
    }

    width = frame->frame_width > 0 ? frame->frame_width : g_cfg.width;
    height = frame->frame_height > 0 ? frame->frame_height : g_cfg.height;
    if (width <= 0 || height <= 0 || (size_t)width > SIZE_MAX / (size_t)height / 2U) {
        fprintf(stderr, "WARNING: invalid preview frame size %dx%d\n", width, height);
        return;
    }

    if (select_preview_format_and_size(frame, width, height, &pixel_format, &bytes_to_write) != 0) {
        return;
    }

    preview_data = (const unsigned char *)frame->frame_yuv_data;
    ffplay_pixel_format = pixel_format;
    ffplay_bytes = bytes_to_write;

    if (strcmp(pixel_format, "gray16be") == 0 || strcmp(pixel_format, "gray16le") == 0) {
        if (convert_gray16_to_gray8(preview_data,
                                    bytes_to_write,
                                    width,
                                    height,
                                    strcmp(pixel_format, "gray16be") == 0,
                                    &converted_data,
                                    &ffplay_bytes) != 0) {
            return;
        }
        preview_data = converted_data;
        ffplay_pixel_format = "gray";
    }

    if (strcmp(pixel_format, "uyvy-luma") == 0 || strcmp(pixel_format, "yuyv-luma") == 0) {
        if (convert_packed_yuv_luma_to_gray8(preview_data,
                                             bytes_to_write,
                                             width,
                                             height,
                                             strcmp(pixel_format, "uyvy-luma") == 0,
                                             &converted_data,
                                             &ffplay_bytes) != 0) {
            return;
        }
        preview_data = converted_data;
        ffplay_pixel_format = "gray";
    }

    if (buffer_is_all_zero(preview_data, ffplay_bytes)) {
        if (!g_preview_zero_warning_printed) {
            fprintf(stderr, "WARNING: skipping all-zero preview frame to avoid blacking out the window\n");
            g_preview_zero_warning_printed = 1;
        }
        free(converted_data);
        return;
    }

    {
        unsigned char *paletted_data = NULL;
        size_t paletted_bytes = 0;
        int palette_ret = apply_preview_palette(preview_data,
                                                ffplay_bytes,
                                                width,
                                                height,
                                                &ffplay_pixel_format,
                                                &paletted_data,
                                                &paletted_bytes);
        if (palette_ret < 0) {
            free(converted_data);
            return;
        }
        if (palette_ret > 0) {
            free(converted_data);
            converted_data = paletted_data;
            preview_data = converted_data;
            ffplay_bytes = paletted_bytes;
        }
    }

    pthread_mutex_lock(&g_preview_lock);
    if (preview_open_locked(width, height, ffplay_pixel_format) != 0) {
        pthread_mutex_unlock(&g_preview_lock);
        free(converted_data);
        return;
    }

    written = fwrite((const void *)preview_data, 1, ffplay_bytes, g_preview_pipe);
    if (written != ffplay_bytes || fflush(g_preview_pipe) != 0 || ferror(g_preview_pipe)) {
        fprintf(stderr, "WARNING: failed to write frame to ffplay preview, disabling preview\n");
        pclose(g_preview_pipe);
        g_preview_pipe = NULL;
        g_preview_disabled = 1;
    }
    pthread_mutex_unlock(&g_preview_lock);
    free(converted_data);
}

static void save_frame_files(int frame_no, guide_usb_frame_data_t *frame)
{
    char filename[128];
    char path[PATH_MAX];

    if (g_cfg.save_frames <= 0 || frame_no > g_cfg.save_frames || frame == NULL) {
        return;
    }

    if (frame->frame_src_data_length > 0) {
        snprintf(filename, sizeof(filename), "frame_%06d_src.raw", frame_no);
        if (make_output_path(path, sizeof(path), filename) == 0) {
            save_bytes_file(path, frame->frame_src_data, frame->frame_src_data_length, "src");
        }
    }

    if (frame->frame_yuv_data_length > 0) {
        snprintf(filename, sizeof(filename), "frame_%06d_yuv.raw", frame_no);
        if (make_output_path(path, sizeof(path), filename) == 0) {
            save_bytes_file(path, frame->frame_yuv_data, frame->frame_yuv_data_length, "yuv");
        }
    }

    if (frame->paramLine_length > 0) {
        snprintf(filename, sizeof(filename), "frame_%06d_param.raw", frame_no);
        if (make_output_path(path, sizeof(path), filename) == 0) {
            save_bytes_file(path, frame->paramLine, frame->paramLine_length, "param");
        }

        snprintf(filename, sizeof(filename), "frame_%06d_param.txt", frame_no);
        if (make_output_path(path, sizeof(path), filename) == 0) {
            save_param_txt(path, frame->paramLine, frame->paramLine_length, frame_no);
        }
    }
}

static int connect_status_callback(guide_usb_device_status_e device_status)
{
    if (device_status == DEVICE_CONNECT_OK) {
        printf("connectStatusCB: DEVICE_CONNECT_OK (%d)\n", device_status);
    } else if (device_status == DEVICE_DISCONNECT_OK) {
        printf("connectStatusCB: DEVICE_DISCONNECT_OK (%d)\n", device_status);
    } else {
        printf("connectStatusCB: unknown status (%d)\n", device_status);
    }
    fflush(stdout);
    return 0;
}

static int frame_callback(guide_usb_frame_data_t *frame)
{
    int frame_no;

    if (frame == NULL) {
        fprintf(stderr, "frameCallBack: received NULL frame pointer\n");
        return 0;
    }

    pthread_mutex_lock(&g_state_lock);
    g_frame_count++;
    frame_no = g_frame_count;
    if (g_cfg.frames > 0 && g_frame_count >= g_cfg.frames) {
        g_stop_requested = 1;
    }
    pthread_mutex_unlock(&g_state_lock);

    printf("frame %d: frame_width=%d frame_height=%d frame_src_data_length=%d frame_yuv_data_length=%d paramLine_length=%d\n",
           frame_no,
           frame->frame_width,
           frame->frame_height,
           frame->frame_src_data_length,
           frame->frame_yuv_data_length,
           frame->paramLine_length);
    fflush(stdout);

    save_frame_files(frame_no, frame);
    preview_frame(frame);
    return 0;
}

static int current_frame_count(void)
{
    int count;

    pthread_mutex_lock(&g_state_lock);
    count = g_frame_count;
    pthread_mutex_unlock(&g_state_lock);

    return count;
}

static int parse_args(int argc, char **argv)
{
    int opt;
    int option_index = 0;
    static const struct option long_options[] = {
        {"device", required_argument, 0, 1},
        {"width", required_argument, 0, 2},
        {"height", required_argument, 0, 3},
        {"mode", required_argument, 0, 4},
        {"version", required_argument, 0, 5},
        {"frames", required_argument, 0, 6},
        {"timeout-sec", required_argument, 0, 7},
        {"out", required_argument, 0, 8},
        {"save-frames", required_argument, 0, 9},
        {"log-level", required_argument, 0, 10},
        {"preview", no_argument, 0, 11},
        {"no-preview", no_argument, 0, 12},
        {"preview-fps", required_argument, 0, 13},
        {"preview-format", required_argument, 0, 14},
        {"preview-length-units", required_argument, 0, 15},
        {"preview-normalize", no_argument, 0, 16},
        {"no-preview-normalize", no_argument, 0, 17},
        {"preview-palette", required_argument, 0, 18},
        {"help", no_argument, 0, 19},
        {0, 0, 0, 0}
    };

    g_cfg.device = DEFAULT_DEVICE;
    g_cfg.preview_format = DEFAULT_PREVIEW_FORMAT;
    g_cfg.preview_length_units = DEFAULT_PREVIEW_LENGTH_UNITS;
    g_cfg.preview_palette = DEFAULT_PREVIEW_PALETTE;
    g_cfg.width = DEFAULT_WIDTH;
    g_cfg.height = DEFAULT_HEIGHT;
    g_cfg.mode = DEFAULT_MODE;
    g_cfg.version = DEFAULT_VERSION;
    g_cfg.frames = DEFAULT_FRAMES;
    g_cfg.timeout_sec = DEFAULT_TIMEOUT_SEC;
    g_cfg.save_frames = DEFAULT_SAVE_FRAMES;
    g_cfg.log_level = DEFAULT_LOG_LEVEL;
    g_cfg.preview = DEFAULT_PREVIEW;
    g_cfg.preview_fps = DEFAULT_PREVIEW_FPS;
    g_cfg.preview_normalize = DEFAULT_PREVIEW_NORMALIZE;

    if (build_default_out_dir(g_cfg.out_dir, sizeof(g_cfg.out_dir)) != 0) {
        return -1;
    }

    while ((opt = getopt_long(argc, argv, "", long_options, &option_index)) != -1) {
        switch (opt) {
        case 1:
            g_cfg.device = optarg;
            break;
        case 2:
            if (parse_int_arg("--width", optarg, &g_cfg.width) != 0) return -1;
            break;
        case 3:
            if (parse_int_arg("--height", optarg, &g_cfg.height) != 0) return -1;
            break;
        case 4:
            if (parse_int_arg("--mode", optarg, &g_cfg.mode) != 0) return -1;
            break;
        case 5:
            if (parse_int_arg("--version", optarg, &g_cfg.version) != 0) return -1;
            break;
        case 6:
            if (parse_int_arg("--frames", optarg, &g_cfg.frames) != 0) return -1;
            break;
        case 7:
            if (parse_int_arg("--timeout-sec", optarg, &g_cfg.timeout_sec) != 0) return -1;
            break;
        case 8:
            if (snprintf(g_cfg.out_dir, sizeof(g_cfg.out_dir), "%s", optarg) >= (int)sizeof(g_cfg.out_dir)) {
                fprintf(stderr, "--out path is too long: %s\n", optarg);
                return -1;
            }
            break;
        case 9:
            if (parse_int_arg("--save-frames", optarg, &g_cfg.save_frames) != 0) return -1;
            break;
        case 10:
            if (parse_int_arg("--log-level", optarg, &g_cfg.log_level) != 0) return -1;
            break;
        case 11:
            g_cfg.preview = 1;
            break;
        case 12:
            g_cfg.preview = 0;
            break;
        case 13:
            if (parse_int_arg("--preview-fps", optarg, &g_cfg.preview_fps) != 0) return -1;
            break;
        case 14:
            g_cfg.preview_format = optarg;
            break;
        case 15:
            g_cfg.preview_length_units = optarg;
            break;
        case 16:
            g_cfg.preview_normalize = 1;
            break;
        case 17:
            g_cfg.preview_normalize = 0;
            break;
        case 18:
            g_cfg.preview_palette = optarg;
            break;
        case 19:
            usage(argv[0]);
            exit(0);
        default:
            usage(argv[0]);
            return -1;
        }
    }

    if (g_cfg.width <= 0 || g_cfg.height <= 0) {
        fprintf(stderr, "--width and --height must be positive\n");
        return -1;
    }
    if (g_cfg.frames <= 0) {
        fprintf(stderr, "--frames must be positive\n");
        return -1;
    }
    if (g_cfg.timeout_sec < 0) {
        fprintf(stderr, "--timeout-sec must be >= 0\n");
        return -1;
    }
    if (g_cfg.save_frames < 0) {
        fprintf(stderr, "--save-frames must be >= 0\n");
        return -1;
    }
    if (g_cfg.preview_fps <= 0) {
        fprintf(stderr, "--preview-fps must be positive\n");
        return -1;
    }
    if (strcmp(g_cfg.preview_format, "auto") != 0 &&
        strcmp(g_cfg.preview_format, "gray8") != 0 &&
        strcmp(g_cfg.preview_format, "gray") != 0 &&
        strcmp(g_cfg.preview_format, "uyvy-luma") != 0 &&
        strcmp(g_cfg.preview_format, "yuyv-luma") != 0 &&
        strcmp(g_cfg.preview_format, "gray16le") != 0 &&
        strcmp(g_cfg.preview_format, "gray16be") != 0 &&
        strcmp(g_cfg.preview_format, "yuyv422") != 0 &&
        strcmp(g_cfg.preview_format, "uyvy422") != 0) {
        fprintf(stderr, "--preview-format must be auto, gray8, uyvy-luma, yuyv-luma, yuyv422, uyvy422, gray16le, or gray16be\n");
        return -1;
    }
    if (strcmp(g_cfg.preview_length_units, "auto") != 0 &&
        strcmp(g_cfg.preview_length_units, "bytes") != 0 &&
        strcmp(g_cfg.preview_length_units, "shorts") != 0) {
        fprintf(stderr, "--preview-length-units must be auto, bytes, or shorts\n");
        return -1;
    }
    if (strcmp(g_cfg.preview_palette, "gray") != 0 &&
        strcmp(g_cfg.preview_palette, "blue-red") != 0) {
        fprintf(stderr, "--preview-palette must be gray or blue-red\n");
        return -1;
    }

    return 0;
}

static void print_config(void)
{
    printf("plug617_stream_probe parameters:\n");
    printf("  device=%s\n", g_cfg.device);
    printf("  width=%d\n", g_cfg.width);
    printf("  height=%d\n", g_cfg.height);
    printf("  mode=%d\n", g_cfg.mode);
    printf("  version=%d\n", g_cfg.version);
    printf("  frames=%d\n", g_cfg.frames);
    printf("  timeout_sec=%d\n", g_cfg.timeout_sec);
    printf("  out=%s\n", g_cfg.out_dir);
    printf("  save_frames=%d\n", g_cfg.save_frames);
    printf("  log_level=%d\n", g_cfg.log_level);
    printf("  preview=%s\n", g_cfg.preview ? "on" : "off");
    printf("  preview_fps=%d\n", g_cfg.preview_fps);
    printf("  preview_format=%s\n", g_cfg.preview_format);
    printf("  preview_length_units=%s\n", g_cfg.preview_length_units);
    printf("  preview_palette=%s\n", g_cfg.preview_palette);
    printf("  preview_normalize=%s\n", g_cfg.preview_normalize ? "on" : "off");
    printf("\n");
    if (g_cfg.save_frames > 0) {
        printf("WARNING: SDK Demo.c does not clarify whether *_length values are bytes or 16-bit words.\n");
        printf("WARNING: Raw dumps interpret *_length as bytes to avoid reading beyond SDK buffers.\n");
        printf("\n");
    }
    fflush(stdout);
}

static int check_device_path(const char *device)
{
    struct stat st;

    if (stat(device, &st) != 0) {
        fprintf(stderr, "Device path does not exist or is not accessible: %s (%s)\n", device, strerror(errno));
        return -1;
    }

    if (!S_ISCHR(st.st_mode)) {
        fprintf(stderr, "WARNING: %s exists but is not a character device\n", device);
    }

    if (access(device, R_OK | W_OK) != 0) {
        fprintf(stderr, "WARNING: current user may not have read/write access to %s (%s)\n", device, strerror(errno));
    }

    return 0;
}

int main(int argc, char **argv)
{
    guide_usb_device_info_t device_info;
    int ret;
    int close_ret = 0;
    int exit_ret = 0;
    int sdk_initialized = 0;
    int stream_opened = 0;
    int timed_out = 0;
    int last_wait_log_sec = -1;
    time_t start_time;
    struct sigaction sa;
    struct sigaction alarm_sa;
    struct sigaction pipe_sa;

    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);

    if (parse_args(argc, argv) != 0) {
        usage(argv[0]);
        return 2;
    }

    print_config();

    if (mkdir_p(g_cfg.out_dir) != 0) {
        return 2;
    }

    if (check_device_path(g_cfg.device) != 0) {
        return 3;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    memset(&alarm_sa, 0, sizeof(alarm_sa));
    alarm_sa.sa_handler = alarm_handler;
    sigemptyset(&alarm_sa.sa_mask);
    sigaction(SIGALRM, &alarm_sa, NULL);

    memset(&pipe_sa, 0, sizeof(pipe_sa));
    pipe_sa.sa_handler = SIG_IGN;
    sigemptyset(&pipe_sa.sa_mask);
    sigaction(SIGPIPE, &pipe_sa, NULL);

    ret = guide_usb_setLogLevel(g_cfg.log_level);
    printf("guide_usb_setLogLevel(%d) returned %d\n", g_cfg.log_level, ret);
    if (ret < 0) {
        fprintf(stderr, "WARNING: guide_usb_setLogLevel failed: %d\n", ret);
    }

    ret = guide_usb_initialize(g_cfg.device);
    printf("guide_usb_initialize(%s) returned %d\n", g_cfg.device, ret);
    if (ret < 0) {
        fprintf(stderr, "guide_usb_initialize failed: %d\n", ret);
        return 4;
    }
    sdk_initialized = 1;

    memset(&device_info, 0, sizeof(device_info));
    device_info.width = g_cfg.width;
    device_info.height = g_cfg.height;
    device_info.video_mode = (guide_usb_video_mode_e)g_cfg.mode;
    device_info.device_version = g_cfg.version;

    ret = guide_usb_openStream(&device_info,
                               (OnFrameDataReceivedCB)frame_callback,
                               (OnDeviceConnectStatusCB)connect_status_callback);
    printf("guide_usb_openStream(...) returned %d\n", ret);
    if (ret < 0) {
        fprintf(stderr, "guide_usb_openStream failed: %d\n", ret);
        exit_ret = guide_usb_exit();
        printf("guide_usb_exit() returned %d\n", exit_ret);
        return 5;
    }
    stream_opened = 1;

    start_time = time(NULL);
    while (!g_stop_requested) {
        int frames_seen = current_frame_count();
        time_t now = time(NULL);
        int elapsed_sec = 0;

        if (frames_seen >= g_cfg.frames) {
            break;
        }

        if (start_time != (time_t)-1 && now != (time_t)-1) {
            elapsed_sec = (int)difftime(now, start_time);
            if (elapsed_sec != last_wait_log_sec && elapsed_sec > 0) {
                printf("waiting for frames... elapsed=%d sec frames_received=%d target_frames=%d\n",
                       elapsed_sec, frames_seen, g_cfg.frames);
                last_wait_log_sec = elapsed_sec;
            }
        }

        if (g_cfg.timeout_sec > 0 && start_time != (time_t)-1 && now != (time_t)-1 &&
            difftime(now, start_time) >= g_cfg.timeout_sec) {
            timed_out = 1;
            break;
        }

        usleep(10000);
    }

    if (g_signal_number != 0) {
        printf("Stop requested by signal %d\n", g_signal_number);
    } else if (timed_out) {
        printf("Stop requested by timeout after %d seconds\n", g_cfg.timeout_sec);
    } else {
        printf("Stop requested after receiving target frames or callback stop condition\n");
    }

    preview_close();

    if (stream_opened) {
        printf("Calling guide_usb_closeStream() with %d second watchdog. Press Ctrl+C again to force immediate exit.\n",
               CLOSE_WATCHDOG_SEC);
        alarm(CLOSE_WATCHDOG_SEC);
        close_ret = guide_usb_closeStream();
        alarm(0);
        printf("guide_usb_closeStream() returned %d\n", close_ret);
        if (close_ret < 0) {
            fprintf(stderr, "guide_usb_closeStream failed: %d\n", close_ret);
        }
    }

    if (sdk_initialized) {
        printf("Calling guide_usb_exit() with %d second watchdog\n", CLOSE_WATCHDOG_SEC);
        alarm(CLOSE_WATCHDOG_SEC);
        exit_ret = guide_usb_exit();
        alarm(0);
        printf("guide_usb_exit() returned %d\n", exit_ret);
        if (exit_ret < 0) {
            fprintf(stderr, "guide_usb_exit failed: %d\n", exit_ret);
        }
    }

    printf("SUMMARY frames_received=%d target_frames=%d status=%s close_ret=%d exit_ret=%d\n",
           current_frame_count(),
           g_cfg.frames,
           g_signal_number != 0 ? "interrupted" : (timed_out ? "timeout" : "completed"),
           close_ret,
           exit_ret);

    if (g_signal_number != 0) {
        return 130;
    }
    if (timed_out) {
        return 124;
    }
    if (close_ret < 0 || exit_ret < 0) {
        return 6;
    }

    return 0;
}
