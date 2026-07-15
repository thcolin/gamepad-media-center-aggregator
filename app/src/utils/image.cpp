#include "utils/image.hpp"
#include "utils/thread.hpp"
#include <atomic>
#include <fmt/format.h>
#include <borealis/core/cache_helper.hpp>
#ifdef USE_WEBP
#include <webp/decode.h>
#endif
#include <stb_image.h>

#ifdef BOREALIS_USE_GXM
#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif
#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif
#define STB_DXT_IMPLEMENTATION
#include <borealis/extern/nanovg/stb_dxt.h>
#include <borealis/extern/nanovg/nanovg_gxm.h>

static inline __attribute__((always_inline)) uint32_t nearest_po2(uint32_t val) {
    val--;
    val |= val >> 1;
    val |= val >> 2;
    val |= val >> 4;
    val |= val >> 8;
    val |= val >> 16;
    val++;

    return val;
}

static inline __attribute__((always_inline)) uint64_t morton_1(uint64_t x) {
    x = x & 0x5555555555555555;
    x = (x | (x >> 1)) & 0x3333333333333333;
    x = (x | (x >> 2)) & 0x0F0F0F0F0F0F0F0F;
    x = (x | (x >> 4)) & 0x00FF00FF00FF00FF;
    x = (x | (x >> 8)) & 0x0000FFFF0000FFFF;
    x = (x | (x >> 16)) & 0xFFFFFFFFFFFFFFFF;
    return x;
}

static inline __attribute__((always_inline)) void d2xy_morton(uint64_t d, uint64_t* x, uint64_t* y) {
    *x = morton_1(d);
    *y = morton_1(d >> 1);
}

static inline __attribute__((always_inline)) void extract_block(
    const uint8_t* src, uint32_t stride, uint32_t remaining_w, uint32_t remaining_h, uint8_t* block) {
    // Fast path: a fully in-bounds 4x4 block, copy four contiguous rows.
    if (remaining_w >= 4 && remaining_h >= 4) {
        for (int j = 0; j < 4; j++) {
            memcpy(&block[j * 4 * 4], src, 16);
            src += stride * 4;
        }
        return;
    }
    // Edge block: the image width/height is not a multiple of 4, so this
    // block only partially overlaps the image. Copying a full 4x4 here would
    // read the next row's pixels (right edge) or run past the decoded buffer
    // (bottom edge) — that overread is what shears/corrupts logos with awkward
    // dimensions. Clamp to the valid pixels and replicate the last in-bounds
    // row/column into the padding so the block stays a single colour region
    // (clean edge under bilinear CLAMP sampling, fewer colours for DXT).
    uint32_t copy_w = MIN(remaining_w, 4u);
    uint32_t copy_h = MIN(remaining_h, 4u);
    for (uint32_t y = 0; y < 4; y++) {
        const uint8_t* row = src + (y < copy_h ? y : copy_h - 1) * stride * 4;
        uint8_t* drow = &block[y * 16];
        for (uint32_t x = 0; x < 4; x++) {
            memcpy(&drow[x * 4], row + (x < copy_w ? x : copy_w - 1) * 4, 4);
        }
    }
}

static void dxt_compress_ext(
    uint8_t* dst, uint8_t* src, uint32_t w, uint32_t h, uint32_t stride, uint32_t last_size, bool isdxt5) {
    uint8_t block[64];
    uint32_t align_w = MAX(nearest_po2(w), last_size);
    uint32_t align_h = MAX(nearest_po2(h), last_size);
    uint32_t s = MIN(align_w, align_h);
    uint32_t num_blocks = s * s / 16;
    const uint32_t block_size = isdxt5 ? 16 : 8;
    uint64_t d, offs_x, offs_y;

    for (d = 0; d < num_blocks; d++, dst += block_size) {
        d2xy_morton(d, &offs_x, &offs_y);
        if (offs_x * 4 >= h || offs_y * 4 >= w) continue;
        // offs_x indexes rows (height), offs_y indexes columns (width); both
        // remaining counts are > 0 thanks to the bounds check above.
        extract_block(
            src + offs_y * 16 + offs_x * stride * 16, stride, w - offs_y * 4, h - offs_x * 4, block);
        stb_compress_dxt_block(dst, block, isdxt5, STB_DXT_NORMAL);
    }
    if (align_w > align_h) return dxt_compress_ext(dst, src + s * 4, w - s, h, stride, s, isdxt5);
    if (align_w < align_h) return dxt_compress_ext(dst, src + stride * s * 4, w, h - s, stride, s, isdxt5);
}

static void dxt_compress(uint8_t* dst, uint8_t* src, uint32_t w, uint32_t h, bool isdxt5) {
    dxt_compress_ext(dst, src, w, h, w, 64, isdxt5);
}

// 2x box-average downscale of an RGBA8 image into a fresh (w+1)/2 x (h+1)/2
// buffer. Each destination pixel averages its 2x2 source block; an odd last
// row/column clamps to the edge so the shrunk image keeps a clean border. Used
// to cap artwork size before it becomes a GXM texture (see doRequest).
static uint8_t* halve_rgba(const uint8_t* src, int w, int h, int* outW, int* outH) {
    int dw = (w + 1) / 2, dh = (h + 1) / 2;
    auto* dst = (uint8_t*)malloc((size_t)dw * dh * 4);
    if (!dst) return nullptr;
    for (int y = 0; y < dh; y++) {
        int sy0 = y * 2, sy1 = MIN(sy0 + 1, h - 1);
        const uint8_t* r0 = src + (size_t)sy0 * w * 4;
        const uint8_t* r1 = src + (size_t)sy1 * w * 4;
        uint8_t* d = dst + (size_t)y * dw * 4;
        for (int x = 0; x < dw; x++) {
            int sx0 = (x * 2) * 4, sx1 = MIN(x * 2 + 1, w - 1) * 4;
            for (int c = 0; c < 4; c++)
                d[x * 4 + c] = (uint8_t)((r0[sx0 + c] + r0[sx1 + c] + r1[sx0 + c] + r1[sx1 + c] + 2) / 4);
        }
    }
    *outW = dw;
    *outH = dh;
    return dst;
}
#endif

Image::Image() : image(nullptr) {
    this->isCancel = std::make_shared<std::atomic_bool>(false);
    brls::Logger::verbose("new Image {}", fmt::ptr(this));
}

Image::~Image() { brls::Logger::verbose("delete Image {}", fmt::ptr(this)); }

void Image::with(brls::Image* view, const std::string& url, int width, int height) {
    int tex = brls::TextureCache::instance().getCache(url);
    if (tex > 0) {
        // The cache owns this texture. brls::Image defaults freeTexture to
        // true, so a view whose FIRST load is a cache hit (common on Stremio:
        // identical absolute URLs across row cells and detail pages) would
        // nvgDeleteImage a cached texture from clear()/its destructor and
        // leave a dead id in the cache — drawn later, that's a GXM fault.
        view->setFreeTexture(false);
        view->innerSetImage(tex);
        return;
    }

    // One fresh Image per request. Recycling a pooled object whose previous
    // doRequest could still be in flight shared url/image/isCancel between two
    // requests: resetting isCancel here REVOKED the cancellation of the old
    // transfer, which then finished and cached its pixels under this request's
    // key (wrong artwork, persistent), while both sides raced on the fields.
    Ref item = std::make_shared<Image>();

    std::lock_guard<std::mutex> lock(requestMutex);

    auto it = requests.insert(std::make_pair(view, item));
    if (!it.second) {
        brls::Logger::warning("insert Image {} failed", fmt::ptr(view));
        return;
    }

    item->image = view;
    item->url = url;
    item->targetW = width;
    item->targetH = height;
    view->ptrLock();
    // 设置图片组件不处理纹理的销毁，由缓存统一管理纹理销毁
    view->setFreeTexture(false);

    ThreadPool::instance().submit([item](HTTP& s) { item->doRequest(s); });
}

void Image::cancel(brls::Image* view) {
    brls::TextureCache::instance().removeCache(view->getTexture());
    view->clear();

    clear(view);
}

void Image::doRequest(HTTP& s) {
    if (this->isCancel->load()) {
        Image::clear(this->image.load());
        return;
    }
    try {
        std::ostringstream body;
        HTTP::set_option(s, this->isCancel, HTTP::Timeout{});
        s._get(this->url, &body);
        std::string data = body.str();
        uint8_t* imageData = nullptr;
        int imageW = 0, imageH = 0;
        bool isWebp = false;
#ifdef USE_WEBP
        char* ct = nullptr;
        if (url.find("Webp") != std::string::npos || (s.getinfo(&ct) && strcmp(ct, "image/webp") == 0)) {
            imageData = WebPDecodeRGBA((const uint8_t*)data.c_str(), data.size(), &imageW, &imageH);
            isWebp = true;
        } else
#endif
        {
            int n;
            imageData = stbi_load_from_memory((unsigned char*)data.c_str(), data.size(), &imageW, &imageH, &n, 4);
        }

        bool hasAlpha = isWebp;
        // exact GPU footprint of the upload, forwarded to the TextureCache
        // byte capacity; 0 = let addCache estimate (w*h*4)
        size_t texBytes = 0;
#ifdef BOREALIS_USE_GXM
        if (imageData) {
            brls::Logger::info("[DBG] img decode {} {}x{} bytes={} target={}x{}", this->url, imageW, imageH,
                data.size(), this->targetW, this->targetH);
            // Downscale to the smallest power-of-two texture that still covers the
            // intended DISPLAY size before the GXM upload. GXM rounds texture
            // dimensions up to the next power of two, so any source Stremio can't
            // resize server-side (absolute Cinemeta/RPDB urls) bloats the GPU:
            // a 1920x1080 backdrop -> 2048² (~4 MB), and — the case that crashed
            // 1.0.5 — a 580x859 RPDB poster shown at 300px -> a full 1024² texture
            // (~1 MB) instead of 512². A show page stacks many of these and the
            // Vita runs out of GPU memory -> GXM fault / blue light. Capping to
            // the display size (poster 300 -> 512², backdrop 1080 -> 1024², hard
            // ceiling 1024) keeps each texture minimal while staying crisp on the
            // 960x544 screen. 2x box-averaging keeps the downscale cheap.
            int tW = this->targetW, tH = this->targetH;
            if (tW > 0 && tH == 0) tH = (int)((int64_t)tW * imageH / imageW);  // poster: derive H from ratio
            if (tH > 0 && tW == 0) tW = (int)((int64_t)tH * imageW / imageH);
            uint32_t capW = tW > 0 ? MIN(nearest_po2((uint32_t)tW), 1024u) : 1024u;
            uint32_t capH = tH > 0 ? MIN(nearest_po2((uint32_t)tH), 1024u) : 1024u;
            while (imageData && (imageW > 1024 || imageH > 1024 ||
                      (nearest_po2((uint32_t)imageW) > capW && nearest_po2((uint32_t)imageH) > capH))) {
                int nw, nh;
                uint8_t* half = halve_rgba(imageData, imageW, imageH, &nw, &nh);
                if (!half) break;
#ifdef USE_WEBP
                if (isWebp)
                    WebPFree(imageData);
                else
#endif
                    stbi_image_free(imageData);
                // The shrunk buffer is a plain malloc now, so route later frees
                // through stbi_image_free (STBI_FREE == free), not WebPFree.
                isWebp = false;
                imageData = half;
                imageW = nw;
                imageH = nh;
            }
            // DXT1 drops the alpha channel entirely: transparent PNGs (clear
            // logos...) would expose the RGB garbage hidden under their
            // alpha-0 areas as opaque blocks. Scan the decoded pixels and
            // keep DXT5 (8-bit alpha) for images with real transparency.
            if (!hasAlpha) {
                size_t bytes = (size_t)imageW * imageH * 4;
                for (size_t i = 3; i < bytes; i += 4) {
                    if (imageData[i] != 255) {
                        hasAlpha = true;
                        break;
                    }
                }
            }
            size_t size = nearest_po2(imageW) * nearest_po2(imageH);
            if (!hasAlpha) size >>= 1;
            // GXM allocates exactly the po2 DXT buffer (4 KB-rounded)
            texBytes = size;
            // calloc: the compressor skips blocks outside the image, and the
            // whole power-of-two buffer is uploaded to GPU memory — padding
            // must be deterministic zeros, not heap garbage
            auto* compressed = (uint8_t*)calloc(size, 1);
            if (compressed) dxt_compress(compressed, imageData, imageW, imageH, hasAlpha);
#ifdef USE_WEBP
            if (isWebp)
                WebPFree(imageData);
            else
#endif
                stbi_image_free(imageData);

            // compressed == nullptr (RAM exhausted): drop this artwork — the
            // sync below treats a null imageData as "nothing to upload"
            imageData = compressed;
        }
#endif
        auto* imagePtr = this->image.load();
        auto urlCopy = this->url;
        auto isCancelCopy = this->isCancel;
#ifdef BOREALIS_USE_GXM
        int imageFlags = (hasAlpha ? NVG_IMAGE_DXT5 : NVG_IMAGE_DXT1) | NVG_IMAGE_LPDDR;
#else
        int imageFlags = 0;
        (void)hasAlpha;
#endif

        brls::Logger::verbose("request Image {} size {}", urlCopy, data.size());
        brls::sync([imagePtr, urlCopy, isCancelCopy, imageData, imageW, imageH, isWebp, imageFlags, texBytes] {
            if (!isCancelCopy->load()) {
                // Load texture
                int tex = brls::TextureCache::instance().getCache(urlCopy);
                if (tex == 0 && imageData != nullptr) {
                    NVGcontext* vg = brls::Application::getNVGContext();
                    // [DBG] This nvgCreateImageRGBA is the GXM texture upload — the
                    // prime suspect for the "GPU crash / freeze". The "begin" line is
                    // flushed BEFORE the call, so if the GPU dies here it is the last
                    // line in the log (with the offending url + dimensions).
                    static std::atomic<int> dbgTexCount{0};
                    brls::Logger::info(
                        "[DBG] gxm upload begin #{} {} {}x{} flags={}", dbgTexCount.load() + 1, urlCopy, imageW, imageH, imageFlags);
                    tex = nvgCreateImageRGBA(vg, imageW, imageH, imageFlags, imageData);
                    brls::Logger::info("[DBG] gxm upload done  #{} tex={}", dbgTexCount.fetch_add(1) + 1, tex);
                    brls::TextureCache::instance().addCache(urlCopy, tex, texBytes);
                }
                if (tex > 0) imagePtr->innerSetImage(tex);
                clear(imagePtr);
            }
            if (imageData) {
#ifdef BOREALIS_USE_GXM
                free(imageData);
#else
#ifdef USE_WEBP
                if (isWebp)
                    WebPFree(imageData);
                else
#endif
                    stbi_image_free(imageData);
#endif
            }
        });
    } catch (const std::exception& ex) {
        brls::Logger::warning("request image {} {}", this->url, ex.what());
        Image::clear(this->image.load());
    }
}

void Image::clear(brls::Image* view) {
    std::lock_guard<std::mutex> lock(requestMutex);

    auto it = requests.find(view);
    if (it == requests.end()) return;

    view->ptrUnlock();
    it->second->image = nullptr;
    it->second->isCancel->store(true);
    requests.erase(it);
}