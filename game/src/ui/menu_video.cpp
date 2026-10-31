#include "ui/menu_video.h"

#include <windows.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>

#include "render/renderer.h"

namespace wrally::ui {

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

namespace {

// How long after the last Update the decoding goes on: past it, nobody is looking.
constexpr auto kIdle = std::chrono::milliseconds(250);

} // namespace

MenuVideo::~MenuVideo()
{
    m_stop = true;
    m_wake.notify_all();
    if (m_thread.joinable()) m_thread.join();
    if (m_renderer) m_renderer->ReleaseTexture(m_texture);
}

std::string MenuVideo::Open(const std::filesystem::path& path)
{
    std::error_code ec;
    if (path.empty() || !std::filesystem::is_regular_file(path, ec)) return "no menu video" + (path.empty() ? "" : " (" + path.string() + ")");
    m_thread = std::thread(&MenuVideo::Run, this, path);
    return "";
}

void MenuVideo::Update(render::Renderer& r)
{
    std::lock_guard<std::mutex> lock(m_lock);
    const bool waking = Clock::now() - m_wanted > kIdle;
    m_wanted = Clock::now();
    if (waking) m_wake.notify_all();
    if (!m_fresh) return;
    m_fresh = false;
    m_renderer = &r;
    if (m_texture && (m_texWidth != m_width || m_texHeight != m_height)) r.ReleaseTexture(m_texture), m_texture = 0;
    if (!m_texture) {
        m_texture = r.CreateTextureRgba(m_frame.data(), uint32_t(m_width), uint32_t(m_height));
        m_texWidth = m_width, m_texHeight = m_height;
        if (!m_texture) std::fprintf(stderr, "the menu video: %s\n", r.Error().c_str());
    } else {
        r.UpdateTextureRgba(m_texture, m_frame.data());
    }
}

// The decoding thread: the file's first video stream as RGB32 (Media Foundation's video processing converts it), each
// frame held until its time, then into m_frame; at the end, back to the start.
void MenuVideo::Run(std::filesystem::path path)
{
    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    const bool mf = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    auto fail = [&](const char* what, HRESULT hr) {
        std::fprintf(stderr, "the menu video (%s): %s, 0x%08lx\n", path.string().c_str(), what, static_cast<unsigned long>(hr));
    };
    [&] {
        if (!mf) return fail("Media Foundation would not start", E_FAIL);
        ComPtr<IMFAttributes> attrs;
        MFCreateAttributes(&attrs, 1);
        attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        ComPtr<IMFSourceReader> reader;
        HRESULT hr = MFCreateSourceReaderFromURL(path.wstring().c_str(), attrs.Get(), &reader);
        if (FAILED(hr)) return fail("would not open", hr);
        reader->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS), FALSE);
        reader->SetStreamSelection(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);
        ComPtr<IMFMediaType> want;
        MFCreateMediaType(&want);
        want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        want->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        hr = reader->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, want.Get());
        if (FAILED(hr)) return fail("no video stream it can turn into RGB", hr);

        UINT32 w = 0, h = 0;
        auto size = [&] {
            ComPtr<IMFMediaType> type;
            if (SUCCEEDED(reader->GetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &type)))
                MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &w, &h);
        };
        size();
        if (!w || !h) return fail("no frame size", E_FAIL);
        std::vector<uint8_t> rgba(size_t(w) * h * 4);

        LONGLONG first = -1;          // the first frame's time since the clock started (100 ns); -1: start it again
        Clock::time_point start;
        while (!m_stop) {
            {   // nobody looking: wait, then the clock from the next frame on
                std::unique_lock<std::mutex> lock(m_lock);
                if (Clock::now() - m_wanted > kIdle) {
                    m_wake.wait(lock, [&] { return m_stop || Clock::now() - m_wanted <= kIdle; });
                    first = -1;
                }
            }
            if (m_stop) break;
            DWORD flags = 0;
            LONGLONG at = 0;
            ComPtr<IMFSample> sample;
            hr = reader->ReadSample(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr, &flags, &at, &sample);
            if (FAILED(hr)) return fail("would not decode", hr);
            if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) size(), rgba.resize(size_t(w) * h * 4);
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {   // the loop
                PROPVARIANT zero;
                PropVariantInit(&zero);
                zero.vt = VT_I8;
                zero.hVal.QuadPart = 0;
                reader->SetCurrentPosition(GUID_NULL, zero);
                first = -1;
                continue;
            }
            if (!sample) continue;
            if (first < 0) first = at, start = Clock::now();
            const Clock::time_point due = start + std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds((at - first) * 100));
            {
                std::unique_lock<std::mutex> lock(m_lock);
                m_wake.wait_until(lock, due, [&] { return m_stop.load(); });
            }
            if (m_stop) break;

            // BGRA (RGB32) rows, top down or bottom up as the buffer's pitch says, to RGBA rows top down.
            ComPtr<IMFMediaBuffer> buffer;
            if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) continue;
            ComPtr<IMF2DBuffer> buffer2d;
            BYTE* scan0 = nullptr;
            LONG pitch = 0;
            DWORD length = 0;
            const bool is2d = SUCCEEDED(buffer.As(&buffer2d)) && SUCCEEDED(buffer2d->Lock2D(&scan0, &pitch));
            if (!is2d) {
                if (FAILED(buffer->Lock(&scan0, nullptr, &length))) continue;
                pitch = LONG(w * 4);
                if (length < DWORD(pitch) * h) {
                    buffer->Unlock();
                    continue;
                }
            }
            for (UINT32 y = 0; y < h; ++y) {
                const uint8_t* s = scan0 + ptrdiff_t(y) * pitch;
                uint8_t* d = rgba.data() + size_t(y) * w * 4;
                for (UINT32 x = 0; x < w; ++x, s += 4, d += 4) d[0] = s[2], d[1] = s[1], d[2] = s[0], d[3] = 255;
            }
            if (is2d) buffer2d->Unlock2D();
            else buffer->Unlock();

            std::lock_guard<std::mutex> lock(m_lock);
            m_frame.swap(rgba);
            m_width = int(w), m_height = int(h);
            m_fresh = true;
            rgba.resize(size_t(w) * h * 4);
        }
    }();
    if (mf) MFShutdown();
    if (com) CoUninitialize();
}

} // namespace wrally::ui
