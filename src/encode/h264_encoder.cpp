#include "encode/h264_encoder.hpp"

#include "NvVideoEncoder.h"

#include <linux/videodev2.h>
#include <cstring>
#include <cstdio>

namespace camctl {

static constexpr uint32_t kNumBuf = 6;

struct H264EncoderImpl {
    NvVideoEncoder* enc = nullptr;
    int W = 0, H = 0;
    H264Encoder::NalCallback on_nal;
    uint32_t out_queued = 0;   // сколько output-буферов уже поставлено в очередь
};

// Колбэк капчур-плоскости: сюда NVENC отдаёт закодированные H.264 буферы (в своём потоке).
static bool capture_dq_cb(struct v4l2_buffer* v4l2_buf, NvBuffer* buffer,
                          NvBuffer* /*shared*/, void* arg) {
    auto* p = static_cast<H264EncoderImpl*>(arg);
    if (!v4l2_buf) return false;

    const uint32_t bytes = buffer->planes[0].bytesused;
    if (bytes > 0 && p->on_nal) {
        const bool key = (v4l2_buf->flags & V4L2_BUF_FLAG_KEYFRAME) != 0;
        p->on_nal(static_cast<const uint8_t*>(buffer->planes[0].data), bytes, key);
    }
    if (bytes == 0) return false;  // EOS

    if (p->enc->capture_plane.qBuffer(*v4l2_buf, nullptr) < 0) return false;
    return true;
}

H264Encoder::H264Encoder() : p_(new H264EncoderImpl()) {}
H264Encoder::~H264Encoder() {
    if (p_->enc) {
        p_->enc->capture_plane.stopDQThread();
        p_->enc->capture_plane.waitForDQThread(1000);
        delete p_->enc;
    }
    delete p_;
}

void H264Encoder::on_nal(NalCallback cb) { p_->on_nal = std::move(cb); }

bool H264Encoder::init(int width, int height, int fps, int bitrate_bps) {
    p_->W = width;
    p_->H = height;

    p_->enc = NvVideoEncoder::createVideoEncoder("enc0");
    if (!p_->enc) { std::fprintf(stderr, "createVideoEncoder failed\n"); return false; }

    // Порядок как в образце: сперва капчур (H.264), потом output (raw).
    if (p_->enc->setCapturePlaneFormat(V4L2_PIX_FMT_H264, width, height, 2 * 1024 * 1024) < 0) {
        std::fprintf(stderr, "setCapturePlaneFormat failed\n"); return false;
    }
    if (p_->enc->setOutputPlaneFormat(V4L2_PIX_FMT_YUV420M, width, height) < 0) {
        std::fprintf(stderr, "setOutputPlaneFormat failed\n"); return false;
    }

    p_->enc->setBitrate(bitrate_bps);
    p_->enc->setProfile(V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE);  // без B-кадров -> ниже задержка
    p_->enc->setLevel(V4L2_MPEG_VIDEO_H264_LEVEL_5_1);
    p_->enc->setFrameRate(fps, 1);
    p_->enc->setInsertSpsPpsAtIdrEnabled(true);                  // SPS/PPS на каждом IDR — для стрима
    p_->enc->setIDRInterval(fps);                                // IDR ~раз в секунду
    p_->enc->setMaxPerfMode(1);

    // Плоскости — MMAP (память маплена, кадр кладём memcpy).
    if (p_->enc->output_plane.setupPlane(V4L2_MEMORY_MMAP, kNumBuf, true, false) < 0) {
        std::fprintf(stderr, "output setupPlane failed\n"); return false;
    }
    if (p_->enc->capture_plane.setupPlane(V4L2_MEMORY_MMAP, kNumBuf, true, false) < 0) {
        std::fprintf(stderr, "capture setupPlane failed\n"); return false;
    }

    if (p_->enc->output_plane.setStreamStatus(true) < 0) return false;
    if (p_->enc->capture_plane.setStreamStatus(true) < 0) return false;

    p_->enc->capture_plane.setDQThreadCallback(capture_dq_cb);
    p_->enc->capture_plane.startDQThread(p_);

    // Поставить все капчур-буферы в очередь (пустые) — чтобы NVENC было куда писать.
    for (uint32_t i = 0; i < p_->enc->capture_plane.getNumBuffers(); ++i) {
        struct v4l2_buffer v4l2_buf;
        struct v4l2_plane planes[MAX_PLANES];
        std::memset(&v4l2_buf, 0, sizeof(v4l2_buf));
        std::memset(planes, 0, sizeof(planes));
        v4l2_buf.index = i;
        v4l2_buf.m.planes = planes;
        if (p_->enc->capture_plane.qBuffer(v4l2_buf, nullptr) < 0) {
            std::fprintf(stderr, "capture qBuffer init failed\n"); return false;
        }
    }
    return true;
}

static void fill_yuv420(NvBuffer* buffer, const uint8_t* i420) {
    const uint8_t* src = i420;
    for (uint32_t pi = 0; pi < buffer->n_planes; ++pi) {
        NvBuffer::NvBufferPlane& plane = buffer->planes[pi];
        const uint32_t pw = plane.fmt.width;
        const uint32_t ph = plane.fmt.height;
        const uint32_t stride = plane.fmt.stride;
        uint8_t* dst = static_cast<uint8_t*>(plane.data);
        for (uint32_t y = 0; y < ph; ++y) {
            std::memcpy(dst + y * stride, src, pw);
            src += pw;
        }
        plane.bytesused = stride * ph;
    }
}

bool H264Encoder::encodeFrame(const uint8_t* i420, size_t /*size*/) {
    struct v4l2_buffer v4l2_buf;
    struct v4l2_plane planes[MAX_PLANES];
    std::memset(&v4l2_buf, 0, sizeof(v4l2_buf));
    std::memset(planes, 0, sizeof(planes));
    v4l2_buf.m.planes = planes;

    NvBuffer* buffer = nullptr;
    if (p_->out_queued < p_->enc->output_plane.getNumBuffers()) {
        // ещё есть неиспользованный буфер — берём по индексу
        v4l2_buf.index = p_->out_queued;
        buffer = p_->enc->output_plane.getNthBuffer(p_->out_queued);
    } else {
        // забираем освободившийся
        if (p_->enc->output_plane.dqBuffer(v4l2_buf, &buffer, nullptr, 10) < 0) {
            std::fprintf(stderr, "output dqBuffer failed\n"); return false;
        }
    }

    fill_yuv420(buffer, i420);
    for (uint32_t pi = 0; pi < buffer->n_planes; ++pi) {
        v4l2_buf.m.planes[pi].bytesused = buffer->planes[pi].bytesused;
    }

    if (p_->enc->output_plane.qBuffer(v4l2_buf, nullptr) < 0) {
        std::fprintf(stderr, "output qBuffer failed\n"); return false;
    }
    if (p_->out_queued < p_->enc->output_plane.getNumBuffers()) ++p_->out_queued;
    return true;
}

void H264Encoder::finish() {
    if (!p_->enc) return;
    // Отправить EOS: пустой output-буфер (bytesused=0).
    struct v4l2_buffer v4l2_buf;
    struct v4l2_plane planes[MAX_PLANES];
    std::memset(&v4l2_buf, 0, sizeof(v4l2_buf));
    std::memset(planes, 0, sizeof(planes));
    v4l2_buf.m.planes = planes;
    NvBuffer* buffer = nullptr;
    if (p_->out_queued < p_->enc->output_plane.getNumBuffers()) {
        v4l2_buf.index = p_->out_queued;
    } else if (p_->enc->output_plane.dqBuffer(v4l2_buf, &buffer, nullptr, 10) < 0) {
        return;
    }
    for (uint32_t pi = 0; pi < MAX_PLANES; ++pi) v4l2_buf.m.planes[pi].bytesused = 0;
    p_->enc->output_plane.qBuffer(v4l2_buf, nullptr);
    // Дождаться, пока капчур-поток получит нулевой буфер (EOS).
    p_->enc->capture_plane.waitForDQThread(3000);
}

}  // namespace camctl
