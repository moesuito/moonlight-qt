/**
 * @file app/streaming/video/pyrowave.h
 * @brief PyroWave video decoder for the Moonlight client.
 *
 * WHY THE CPU DECODE PATH (deliberate, and the reason this is not the "fast" one):
 * the codec also offers pyrowave_decoder_decode_gpu_buffer(), which would keep the
 * planes on the GPU. That path requires interop between the codec's Vulkan device
 * and whatever the renderer uses. That interop is completely unvalidated on this
 * project - the device validation in Phase 0 only covered the host path, and the
 * client renderer is chosen at runtime from SDL D3D11/D3D12/OpenGL/Vulkan/DRM.
 * Wiring the decoder to one of those would be guesswork.
 *
 * pyrowave_decoder_decode_cpu_buffer_synchronous() is a first-class API and takes
 * that risk off the table: it hands back planar YUV in ordinary memory, which the
 * existing SdlRenderer uploads and converts on the GPU. The decode becomes the only
 * CPU-bound stage instead of the whole pipeline. Moving to the GPU path later is a
 * change behind the same wrapper and does not touch the presentation code.
 *
 * The presentation side is NOT reimplemented. Pacer and SdlRenderer are reused
 * as-is, so frame pacing, vsync, overlays, aspect scaling and the video stats all
 * keep working through the code paths that already ship. Only the decode is new.
 */
#pragma once

#include "decoder.h"
#include "ffmpeg-renderers/renderer.h"
#include "ffmpeg-renderers/pacer/pacer.h"

#include <QByteArray>
#include <QQueue>

/**
 * @brief Opaque handle to the codec's device, hiding <vulkan.h> from this header.
 *
 * The whole point of the wrapper in pyrowave.cpp is that nothing else in the
 * project may include <pyrowave.h>: its API is 0.6.0 and the author states it is
 * not stable until MAJOR hits 1 (AGENTS.md rule 6, plan risk R6). A breaking
 * upstream change then lands as a local edit instead of a sweep.
 */
class PyroWaveDevice;

/**
 * @brief The PyroWave decoder.
 *
 * Chosen by Session::chooseDecoder() when the negotiated video format is
 * VIDEO_FORMAT_PYROWAVE. Every other format keeps going to the FFmpeg decoder,
 * which already has hardware acceleration for H.264/HEVC/AV1 that this codec
 * cannot and should not replace.
 */
class PyroWaveVideoDecoder : public IVideoDecoder {
public:
    PyroWaveVideoDecoder(bool testOnly);
    ~PyroWaveVideoDecoder() override;

    bool initialize(PDECODER_PARAMETERS params) override;
    bool isHardwareAccelerated() override;
    bool isAlwaysFullScreen() override;
    bool isHdrSupported() override;
    int getDecoderCapabilities() override;
    int getDecoderColorspace() override;
    int getDecoderColorRange() override;
    QSize getDecoderMaxResolution() override;
    int submitDecodeUnit(PDECODE_UNIT du) override;
    void renderFrameOnMainThread() override;
    void setHdrMode(bool enabled) override;
    bool notifyWindowChanged(PWINDOW_STATE_CHANGE_INFO info) override;

private:
    static int decoderThreadThunk(void* context);
    void decoderThreadProc();

    /**
     * @brief Flatten a decode unit's packet chain into m_DecodeBuffer.
     *
     * The LENTRY chain is a flat singly-linked list, and for a non-H.264/HEVC
     * codec every entry is BUFFER_TYPE_PICDATA (Limelight.h:108-110), so this is
     * a plain ordered concatenation with no per-packet header handling.
     *
     * @return 0 on success, -1 if the reassembly is inconsistent.
     */
    int writeBuffer(PDECODE_UNIT du);

    /**
     * @brief Decode the reassembled frame and hand it to the pacer.
     *
     * Wraps the codec's output planes in an AVFrame so the existing
     * Pacer/SdlRenderer path can present it unchanged. Ownership of the AVFrame
     * passes to the Pacer, which frees it.
     */
    int decodeAndQueueFrame(PDECODE_UNIT du);

    /**
     * @brief Pull a free AVFrame with YUV420P storage attached.
     *
     * The plane storage is a single contiguous AVBufferRef so that av_frame_free()
     * releases everything, which is what the Pacer relies on when it defers a free
     * by one frame.
     */
    AVFrame* acquireFrameBuffer();

    void reset();

    PyroWaveDevice* m_Device;
    void* m_Decoder;

    // Reassembly scratch. Reused across frames to avoid a per-frame allocation.
    QByteArray m_DecodeBuffer;

    // Metadata of the frames in flight. The LENTRY chain in a queued DECODE_UNIT is
    // only valid until submitDecodeUnit() returns, so only the metadata is kept.
    QQueue<DECODE_UNIT> m_FrameInfoQueue;

    IFFmpegRenderer* m_FrontendRenderer;
    Pacer* m_Pacer;

    VIDEO_STATS m_GlobalVideoStats;
    VIDEO_STATS m_LastWndVideoStats;
    VIDEO_STATS m_ActiveWndVideoStats;

    BandwidthTracker m_BwTracker;

    int m_FramesIn;
    int m_FramesOut;
    int m_LastFrameNumber;
    int m_StreamFps;
    int m_VideoFormat;
    int m_Width;
    int m_Height;

    int m_ConsecutiveFailedDecodes;

    bool m_NeedsSpsFixup;
    bool m_TestOnly;

    SDL_Thread* m_DecoderThread;
    SDL_atomic_t m_DecoderThreadShouldQuit;
};
