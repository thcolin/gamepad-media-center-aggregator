#pragma once

#include <atomic>
#include <borealis.hpp>
#include "api/http.hpp"
#include "api/backend.hpp"
#include "config.hpp"
#include "image_cache.hpp"

class Image {
    using Ref = std::shared_ptr<Image>;

public:
    Image();
    Image(const Image&) = delete;

    virtual ~Image();

    /// Loads an image for a backend path: the on-disk cached asset if present
    /// (offline), else the active backend's image URL. width/height > 0 requests
    /// backend-side resize where the backend supports it.
    static void load(brls::Image* view, const std::string& path, int width = 0, int height = 0) {
        if (path.empty()) return;
        // offline cache wins: a locally cached asset renders without the server
        // and gives downloaded content instant local artwork even online
        // (SPEC §4.2, AC6/AC17). Keyed by the raw path/url passed here.
        if (ImageCache::has(path)) {
            view->setImageFromFile(ImageCache::localPath(path));
            return;
        }
        // backend-specific URL building (Plex /photo/:/transcode, Jellyfin /Images...);
        // absolute external paths (cast faces...) are returned unchanged by the backend
        std::string url = AppConfig::instance().backend().imageUrl(path, width, height);
        // width/height are also forwarded to the decoder: backends that can't
        // resize server-side (Stremio's absolute Cinemeta/RPDB urls) still get
        // the artwork downscaled to its display size before the GPU upload, so a
        // 580x859 RPDB poster becomes a 512² texture instead of a 1024² one — the
        // Vita GPU-memory exhaustion behind the overview crash (GXM only).
        if (!url.empty()) with(view, url, width, height);
    }

    /// @brief 设置要加载内容的图片组件。此函数需要工作在主线程。
    /// width/height (>0) = the intended display size, used on GXM to cap the
    /// decoded texture to the smallest power-of-two that still covers it.
    static void with(brls::Image* view, const std::string& url, int width = 0, int height = 0);

    /// @brief 取消请求，并清空图片。此函数需要工作在主线程。
    static void cancel(brls::Image* view);

private:
    void doRequest(HTTP& s);

    static void clear(brls::Image* view);

private:
    std::string url;
    // written by clear() (UI thread) while doRequest (worker) reads it on its
    // cancel/error paths — atomic so neither side sees a torn pointer
    std::atomic<brls::Image*> image;
    HTTP::Cancel isCancel;
    int targetW = 0;  // intended display size (GXM texture cap); 0 = unknown
    int targetH = 0;

    inline static std::mutex requestMutex;
    inline static std::unordered_map<brls::Image*, Ref> requests;
};