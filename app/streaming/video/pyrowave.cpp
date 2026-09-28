/**
 * @file app/streaming/video/pyrowave.cpp
 * @brief PyroWave video decoder for the Moonlight client.
 *
 * This is the ONLY translation unit in the project that includes <pyrowave.h> or
 * calls a pyrowave_* function. See the header for why the presentation side is
 * reused rather than reimplemented, and why the CPU decode path was chosen.
 */

// standard includes
#include <cstring>

// local includes
#include "pyrowave.h"

// third party includes
// pyrowave.h hard-errors unless the Vulkan core header came first (its own #error
// at the top of the file), so this include order is a requirement, not a preference.
#include <vulkan/vulkan.h>
extern "C" {
#include <pyrowave.h>
}

// local includes
#include "ffmpeg-renderers/sdlvid.h"

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
}

namespace {

    /**
     * @brief Log a failed codec call and map it to false.
     */
    bool check(pyrowave_result result, const char *what) {
        if (result != PYROWAVE_SUCCESS) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "PyroWave: %s failed, result %d", what, static_cast<int>(result));
            return false;
        }
        return true;
    }

}  // namespace

/**
 * @brief Owning handle to the codec's Vulkan device.
 *
 * Typed destruction lives here rather than in the header so that <vulkan.h> does
 * not leak into any file that merely holds a decoder.
 */
class PyroWaveDevice {
public:
    PyroWaveDevice() = default;
    ~PyroWaveDevice() {
        if (m_Device) {
            pyrowave_device_destroy(m_Device);
        }
    }

    PyroWaveDevice(const PyroWaveDevice&) = delete;
    PyroWaveDevice& operator=(const PyroWaveDevice&) = delete;

    /**
     * @brief Create a device.
     *
     * A null LUID lets the codec pick the first suitable device. The client is not
     * tied to a capture adapter the way the host is, so there is no LUID to pin -
     * but on a hybrid-graphics laptop that may not be the GPU the window is on.
     * Recorded as a known limitation rather than guessed at: pinning correctly would
     * mean matching the adapter SDL picked, which is not exposed through
     * DECODER_PARAMETERS.
     */
    static PyroWaveDevice* create() {
        auto *device = new PyroWaveDevice();
        if (!check(pyrowave_create_device_by_compat(0, 0, 0, 0, nullptr, &device->m_Device),
                   "create_device_by_compat")) {
            delete device;
            return nullptr;
        }
        return device;
    }

    pyrowave_device get() const {
        return m_Device;
    }

private:
    pyrowave_device m_Device = nullptr;
};

PyroWaveVideoDecoder::PyroWaveVideoDecoder(bool testOnly) :
    m_Device(nullptr),
    m_Decoder(nullptr),
    m_FrontendRenderer(nullptr),
    m_Pacer(nullptr),
    m_FramesIn(0),
    m_FramesOut(0),
    m_LastFrameNumber(0),
    m_StreamFps(60),
    m_VideoFormat(0),
    m_Width(0),
    m_Height(0),
    m_ConsecutiveFailedDecodes(0),
    m_NeedsSpsFixup(false),
    m_TestOnly(testOnly) {
    memset(&m_GlobalVideoStats, 0, sizeof(m_GlobalVideoStats));
    memset(&m_LastWndVideoStats, 0, sizeof(m_LastWndVideoStats));
    memset(&m_ActiveWndVideoStats, 0, sizeof(m_ActiveWndVideoStats));

    SDL_AtomicSet(&m_DecoderThreadShouldQuit, 0);
    m_DecoderThread = nullptr;
}

PyroWaveVideoDecoder::~PyroWaveVideoDecoder() {
    reset();

    if (m_Decoder) {
        // The codec guarantees the GPU is idle before destroying its objects.
        pyrowave_decoder_destroy(reinterpret_cast<pyrowave_decoder>(m_Decoder));
        m_Decoder = nullptr;
    }

    delete m_Device;
    m_Device = nullptr;
}

void PyroWaveVideoDecoder::reset() {
    if (m_Pacer) {
        delete m_Pacer;
        m_Pacer = nullptr;
    }

    if (m_FrontendRenderer) {
        delete m_FrontendRenderer;
        m_FrontendRenderer = nullptr;
    }
}

bool PyroWaveVideoDecoder::isHardwareAccelerated() {
    // Honest answer: no. The decode runs on the CPU (see the header for why), so the
    // only GPU work is the YUV to RGB conversion inside SdlRenderer. Claiming
    // hardware acceleration here would make the stats and any capability fallback
    // logic lie, and the fallback logic is what keeps a broken path from looking fine.
    return false;
}

bool PyroWaveVideoDecoder::isAlwaysFullScreen() {
    return false;
}

bool PyroWaveVideoDecoder::isHdrSupported() {
    // Only 8-bit 4:2:0 is implemented on the host, so HDR is not something the
    // client can even be asked for.
    return false;
}

int PyroWaveVideoDecoder::getDecoderCapabilities() {
    // No capability flags apply. The codec is 8-bit 4:2:0 intra-only, so there is
    // no reference-frame restriction to report, no 10-bit or 4:4:4 variant, and no
    // buffer-count attribute to set.
    return 0;
}

int PyroWaveVideoDecoder::getDecoderColorspace() {
    return 0;  // rec601 / sRGB, matching the SDR-only profile.
}

int PyroWaveVideoDecoder::getDecoderColorRange() {
    // The encoder writes full-range YCbCr with center chroma siting, so the client
    // must not apply a limited-range expansion on the way out.
    return 1;
}

QSize PyroWaveVideoDecoder::getDecoderMaxResolution() {
    // The host's bitrate ceiling, not a hard decoder limit: past roughly 4K the
    // automatic bitrate stops fitting the link long before the codec gives up.
    // Kept as a size rather than a flag so the UI can show the real constraint.
    return QSize(3840, 2160);
}

bool PyroWaveVideoDecoder::initialize(PDECODER_PARAMETERS params) {
    if (params->videoFormat != VIDEO_FORMAT_PYROWAVE) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PyroWave decoder requires VIDEO_FORMAT_PYROWAVE (got %d)", params->videoFormat);
        return false;
    }

    // The host only implements 8-bit 4:2:0, and SdlRenderer rejects 10-bit outright
    // (sdlvid.cpp:140). Rejecting here gives a clear message instead of a confusing
    // renderer failure further down.
    if (params->videoFormat & VIDEO_FORMAT_MASK_10BIT) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PyroWave: 10-bit is not supported by the host encoder");
        return false;
    }

    m_VideoFormat = params->videoFormat;
    m_Width = params->width;
    m_Height = params->height;
    m_StreamFps = params->frameRate;

    // 4:2:0 needs even dimensions. A client that negotiates an odd width would
    // otherwise trip an assert inside the codec.
    if ((m_Width & 1) || (m_Height & 1)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PyroWave: %dx%d is not a valid 4:2:0 frame size (both must be even)",
                     m_Width, m_Height);
        return false;
    }

    m_Device = PyroWaveDevice::create();
    if (!m_Device) {
        return false;
    }

    pyrowave_decoder_create_info decoderInfo = {};
    decoderInfo.device = m_Device->get();
    decoderInfo.width = m_Width;
    decoderInfo.height = m_Height;
    decoderInfo.chroma = PYROWAVE_CHROMA_SUBSAMPLING_420;

    pyrowave_decoder decoder = nullptr;
    if (!check(pyrowave_decoder_create(&decoderInfo, &decoder), "decoder_create")) {
        return false;
    }
    m_Decoder = decoder;

    if (pyrowave_decoder_device_prefers_fragment_path(m_Device->get())) {
        // Informational only. The CPU path does not use either GPU path, but the
        // author documents the choice, so record what this GPU would have preferred
        // in case the GPU path is enabled later.
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "PyroWave: this GPU prefers the fragment path (not used: CPU decode)");
    }

    // Reuse the existing presentation stack rather than writing a renderer. This is
    // the same frontend the FFmpeg decoder uses for its non-hwaccel path, so pacing,
    // vsync, overlays and aspect scaling all keep their existing behaviour.
    m_FrontendRenderer = new SdlRenderer();
    if (!m_FrontendRenderer->initialize(params)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "PyroWave: failed to initialize the SDL renderer");
        return false;
    }

    if (!params->testOnly) {
        m_Pacer = new Pacer(m_FrontendRenderer, &m_ActiveWndVideoStats);

        // The SDL frontend reports isRenderThreadSupported() == false on any backend
        // other than direct3d11/direct3d12/metal, so the Pacer has no render thread
        // and no DxVsyncSource. It announces each ready frame with an
        // SDL_USEREVENT carrying SDL_CODE_FRAME_READY, which the session's event loop
        // turns into renderFrameOnMainThread(). That path is the one the FFmpeg
        // decoder already uses for its non-hwaccel frames, so it is proven.
        if (!m_Pacer->initialize(params->window, params->frameRate,
                                 params->enableFramePacing || (params->enableVsync &&
                                     (m_FrontendRenderer->getRendererAttributes() & RENDERER_ATTRIBUTE_FORCE_PACING)))) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "PyroWave: failed to initialize the pacer");
            return false;
        }

        // Only create the thread when instantiated for real. It uses APIs from
        // moonlight-common-c that may only be called with an established connection,
        // which is exactly what params->testOnly is telling us.
        m_DecoderThread = SDL_CreateThread(PyroWaveVideoDecoder::decoderThreadThunk,
                                           "PyroWaveDecoder", (void*)this);
        if (!m_DecoderThread) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "PyroWave: failed to create the decoder thread");
            return false;
        }
    }

    m_LastFrameNumber = 0;
    // m_BwTracker is deliberately not (re)assigned here: its assignment operators
    // are deleted, so the window settings have to be fixed at construction. The
    // default (10s window, 250ms buckets) is the same pair the FFmpeg decoder uses.

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "PyroWave video decoder initialized for %dx%d@%d (CPU decode, SDL renderer)",
                m_Width, m_Height, m_StreamFps);
    return true;
}

AVFrame* PyroWaveVideoDecoder::acquireFrameBuffer() {
    auto *frame = av_frame_alloc();
    if (!frame) {
        return nullptr;
    }

    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = m_Width;
    frame->height = m_Height;

    // One contiguous allocation for all three planes so that a single av_buffer_ref
    // keeps them alive, and av_frame_free() releases everything. The Pacer defers
    // freeing a frame by one frame, so relying on refcounting here is what keeps a
    // still-displayed frame from being pulled out from under the renderer.
    const int chromaWidth = m_Width / 2;
    const int chromaHeight = m_Height / 2;
    const size_t ySize = static_cast<size_t>(m_Width) * m_Height;
    const size_t uSize = static_cast<size_t>(chromaWidth) * chromaHeight;
    const size_t total = ySize + uSize * 2;

    auto *buf = av_buffer_alloc(total);
    if (!buf) {
        av_frame_free(&frame);
        return nullptr;
    }

    // The chroma planes are not cleared, so make sure nothing undefined reaches the
    // renderer. The codec writes all three planes, but a partial-frame decode (which
    // is allowed on packet loss) may leave gaps.
    memset(buf->data, 128, total);

    // av_buffer_ref takes a new reference rather than copying the contents, which
    // is what keeps the planes alive for as long as the frame does. frame->buf[0]
    // is the buffer the frame's plane 0 is backed by, and av_frame_free() releases
    // it. The local reference is dropped right after.
    frame->buf[0] = av_buffer_ref(buf);
    frame->data[0] = frame->buf[0]->data;
    frame->linesize[0] = m_Width;
    frame->data[1] = frame->buf[0]->data + ySize;
    frame->linesize[1] = chromaWidth;
    frame->data[2] = frame->buf[0]->data + ySize + uSize;
    frame->linesize[2] = chromaWidth;

    av_buffer_unref(&buf);
    return frame;
}

int PyroWaveVideoDecoder::writeBuffer(PDECODE_UNIT du) {
    // The LENTRY chain is flat and ordered, and for a non-H.264/HEVC codec every
    // entry is BUFFER_TYPE_PICDATA, so this is a straight concatenation. There is no
    // SPS to rewrite, which is why m_NeedsSpsFixup is unconditionally false.
    Q_ASSERT(!m_NeedsSpsFixup);

    int offset = 0;
    for (PLENTRY entry = du->bufferList; entry != nullptr; entry = entry->next) {
        if (offset + entry->length > du->fullLength) {
            // A chain that overruns fullLength is a transport bug, not a codec
            // problem. Bail rather than truncate into a corrupt frame.
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "PyroWave: packet chain overruns fullLength (%d)", du->fullLength);
            return -1;
        }

        memcpy(m_DecodeBuffer.data() + offset, entry->data, entry->length);
        offset += entry->length;
    }

    if (offset != du->fullLength) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PyroWave: reassembled %d bytes but fullLength is %d", offset, du->fullLength);
        return -1;
    }

    return 0;
}

int PyroWaveVideoDecoder::decodeAndQueueFrame(PDECODE_UNIT du) {
    auto *decoder = reinterpret_cast<pyrowave_decoder>(m_Decoder);

    // The host sends the whole frame as one contiguous blob, and the Moonlight
    // depacketizer reassembles it before handing it over, so the packetized form
    // the encoder produced is still intact here. The decoder's own packet API is
    // used anyway rather than a single push, because that is the entry point the
    // codec exposes for reordered or partial delivery.
    if (!check(pyrowave_decoder_push_packet(decoder, m_DecodeBuffer.constData(),
                                            static_cast<size_t>(du->fullLength)),
               "decoder_push_packet")) {
        m_ConsecutiveFailedDecodes++;
        return DR_NEED_IDR;
    }

    // allow_partial_frame is false: a frame with missing packets decodes to
    // something, but with visible blurring from the missing wavelet weights, and
    // there is no mechanism here to decide which blocks to trust.
    if (!pyrowave_decoder_decode_is_ready(decoder, false)) {
        // Not an error: the frame is still arriving, or the next push will complete
        // it. The decoder drops packets for an older frame sequence on its own.
        return DR_OK;
    }

    pyrowave_cpu_buffer cpuBuffer = {};
    cpuBuffer.format = PYROWAVE_CPU_BUFFER_FORMAT_YUV420P;
    cpuBuffer.width = m_Width;
    cpuBuffer.height = m_Height;

    const int chromaWidth = m_Width / 2;
    const int chromaHeight = m_Height / 2;
    cpuBuffer.row_stride_in_bytes[0] = static_cast<size_t>(m_Width);
    cpuBuffer.row_stride_in_bytes[1] = static_cast<size_t>(chromaWidth);
    cpuBuffer.row_stride_in_bytes[2] = static_cast<size_t>(chromaWidth);
    cpuBuffer.plane_size_in_bytes[0] = static_cast<size_t>(m_Width) * m_Height;
    cpuBuffer.plane_size_in_bytes[1] = static_cast<size_t>(chromaWidth) * chromaHeight;
    cpuBuffer.plane_size_in_bytes[2] = static_cast<size_t>(chromaWidth) * chromaHeight;

    auto *frame = acquireFrameBuffer();
    if (!frame) {
        // Out of memory. DR_NEED_IDR is the only failure code the protocol offers
        // besides DR_OK, and it is also the honest one: there is no usable frame, so
        // the caller must wait for a fresh one.
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PyroWave: could not allocate a frame buffer");
        m_ConsecutiveFailedDecodes++;
        return DR_NEED_IDR;
    }

    cpuBuffer.data[0] = frame->data[0];
    cpuBuffer.data[1] = frame->data[1];
    cpuBuffer.data[2] = frame->data[2];

    if (!check(pyrowave_decoder_decode_cpu_buffer_synchronous(decoder, &cpuBuffer),
               "decode_cpu_buffer_synchronous")) {
        av_frame_free(&frame);
        m_ConsecutiveFailedDecodes++;
        return DR_NEED_IDR;
    }

    m_ConsecutiveFailedDecodes = 0;

    // The Pacer computes its pacing delay from pkt_dts. Leaving it zero would make
    // the stats nonsense (the FFmpeg decoder sets it at ffmpeg.cpp:2033), so it is
    // set to the same clock the pacer reads.
    frame->pkt_dts = LiGetMicroseconds();
    frame->pts = static_cast<int64_t>(du->rtpTimestamp);

    m_FramesOut++;

    // Ownership of the frame passes to the pacer, which defers the free by one frame.
    m_Pacer->submitFrame(frame);
    return DR_OK;
}

int PyroWaveVideoDecoder::submitDecodeUnit(PDECODE_UNIT du) {
    if (m_ConsecutiveFailedDecodes >= 15) {
        // Every frame is intra-only, so there is no reference chain to rebuild and
        // no reason to wait for a keyframe. Bailing out early is what stops a broken
        // session from burning CPU decoding garbage.
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PyroWave: %d consecutive decode failures, giving up",
                     m_ConsecutiveFailedDecodes);
        return DR_NEED_IDR;
    }

    m_BwTracker.AddBytes(du->fullLength);

    if (m_LastFrameNumber != 0) {
        int frames = m_ConsecutiveFailedDecodes;
        int receivedFrames = du->frameNumber - m_LastFrameNumber - 1;

        if (receivedFrames > frames) {
            m_GlobalVideoStats.networkDroppedFrames += (receivedFrames - frames);
        }
    }
    m_LastFrameNumber = du->frameNumber;

    m_DecodeBuffer.resize(du->fullLength);
    if (writeBuffer(du)) {
        m_ConsecutiveFailedDecodes++;
        return DR_NEED_IDR;
    }

    int ret = decodeAndQueueFrame(du);

    // The queued DU's buffer chain is only valid until this returns, so only the
    // metadata is kept for the stats correlation.
    m_FrameInfoQueue.enqueue(*du);
    m_FramesIn++;
    return ret;
}

void PyroWaveVideoDecoder::renderFrameOnMainThread() {
    if (m_Pacer) {
        m_Pacer->renderOnMainThread();
    }
}

void PyroWaveVideoDecoder::setHdrMode(bool enabled) {
    // No-op. The host only encodes SDR, so this is never called with true. Explicit
    // rather than absent so that if the host ever grows HDR, the gap is visible here.
    if (enabled) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "PyroWave: HDR requested but the host encoder is SDR-only");
    }
}

bool PyroWaveVideoDecoder::notifyWindowChanged(PWINDOW_STATE_CHANGE_INFO info) {
    if (m_FrontendRenderer) {
        return m_FrontendRenderer->notifyWindowChanged(info);
    }
    return false;
}

int PyroWaveVideoDecoder::decoderThreadThunk(void *context) {
    auto *self = static_cast<PyroWaveVideoDecoder*>(context);
    self->decoderThreadProc();
    return 0;
}

void PyroWaveVideoDecoder::decoderThreadProc() {
    // No m_FramesIn/m_FramesOut flow control here, unlike the FFmpeg decoder. That
    // pair exists because avcodec_receive_frame() can emit a frame for an earlier
    // send, so the decoder must be allowed to run ahead. PyroWave's CPU decode is
    // synchronous and one call produces exactly one frame, so the natural pacing is
    // simply: block until the next decode unit arrives, decode it, hand it on.
    while (!SDL_AtomicGet(&m_DecoderThreadShouldQuit)) {
        VIDEO_FRAME_HANDLE handle;
        PDECODE_UNIT du;

        if (!LiWaitForNextVideoFrame(&handle, &du)) {
            // Could be the main thread asking us to exit.
            continue;
        }

        LiCompleteVideoFrame(handle, submitDecodeUnit(du));
    }
}
