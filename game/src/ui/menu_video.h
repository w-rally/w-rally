#pragma once
// wrally::ui::MenuVideo - the main menu's video loop (the design's third stripe, a window onto stages, weathers and
// cars): a video file Media Foundation reads (an .mp4, H.264), decoded on a thread of its own in real time and looped,
// each new frame uploaded to a UI texture (updateTextureRgba) for the menu to draw. Decoding pauses while nobody asks
// for frames (a stage running, another screen), and starts the clock again when they are asked for once more.

#include <atomic>
#include <condition_variable>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "wrally/iface/renderer.h"

namespace wrally::render {
class Renderer;
}

namespace wrally::ui {

// What the menu draws: the texture and its size (texture 0: no video, the window dark).
struct VideoFrame {
    iface::TextureHandle texture = 0;
    int width = 0, height = 0;
};

class MenuVideo {
public:
    MenuVideo() = default;
    ~MenuVideo();
    MenuVideo(const MenuVideo&) = delete;
    MenuVideo& operator=(const MenuVideo&) = delete;

    // Starts playing `path`. Returns "" or why not (no such file); a file that will not decode says so on stderr.
    std::string Open(const std::filesystem::path& path);
    // The newest frame to its texture on `r` (made the first time). Each frame the menu shows, outside RenderFrame.
    void Update(render::Renderer& r);
    VideoFrame Frame() const { return {m_texture, m_texWidth, m_texHeight}; }

private:
    void Run(std::filesystem::path path);

    std::thread m_thread;
    std::atomic<bool> m_stop{false};
    std::mutex m_lock;                     // m_frame, m_width, m_height, m_fresh, m_wanted
    std::condition_variable m_wake;
    std::vector<uint8_t> m_frame;          // the newest frame, RGBA8
    int m_width = 0, m_height = 0;
    bool m_fresh = false;                  // not uploaded yet
    std::chrono::steady_clock::time_point m_wanted;   // the last Update
    render::Renderer* m_renderer = nullptr;
    iface::TextureHandle m_texture = 0;
    int m_texWidth = 0, m_texHeight = 0;
};

} // namespace wrally::ui
