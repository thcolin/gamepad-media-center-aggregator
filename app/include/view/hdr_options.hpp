// HDR video on the PS4 (mpv 0.36): what a file gets. Header-only and free of borealis and mpv,
// so tests/test_hdr_options.cpp builds it alone.
//
// Why: the PS4 build has no runtime shader compiler (SDL loads libSceShaccVSH only when the hint
// SDL_PS4_PIGLET_MODULES_PATH is set, and GMCA never sets it). Every shader is a precompiled
// binary: nanovg's through glShaderBinary, mpv's through a table keyed by the SHA-256 of the
// shader text (scripts/ps4/libmpv/mpv.patch, ps4_shaders_list). mpv's plain video shader is in the
// table; an HDR file makes mpv generate a different text (linearize, tone map, matrix), which is
// not, so the compile from source fails and no picture is drawn while the sound plays. The fix
// gives mpv nothing to generate: the filter `format=gamma=bt.1886:primaries=bt.709:sig-peak=1`
// re-tags the frames as SDR (mpv 0.36 then skips color mapping: no linearize, no tone mapping, no
// matrix), and the shader text is the SDR one. The picture is washed out (PQ code values shown as
// SDR gamma, BT.2020 colors as BT.709), so saturation and contrast are raised a little for PQ
// files: they only feed the colormatrix uniform of that shader, so the text stays the same. (The
// video equalizer's brightness is the same kind; its gamma adds a pow line, another text, and is
// not used.)
//
// The filter is in the chain from the start of mpv, for every file, not added when a file turns out
// to be HDR: mpv reconfigures the video output only when a frame's tags differ from the output's,
// and waits for the frame on screen first; a filter added after the first frame was drawn with
// HDR tags (the video-params event reaches the app after it) was not picked up. For SDR files in
// the usual BT.709 tags it changes nothing: mpv's output colorspace follows the source.
// Wide-gamut SDR (BT.2020 or P3 primaries) is not converted either, which suits the PS4: the
// conversion needs a shader the console can't make.
#pragma once

#include <string>

namespace hdr {

/// The labeled filter that re-tags every frame as SDR: the value of mpv's `vf` option on the PS4.
inline std::string retagFilter() { return "@hdr:format=gamma=bt.1886:primaries=bt.709:sig-peak=1"; }

/// The value of mpv's `vf` for the mirror setting (0 off, 1 horizontal, 2 vertical), with the
/// re-tag filter first when `withRetag` (the mirror filter, a libavfilter graph, then sees SDR
/// tags). One list, set in one step, so there is never a moment without the re-tag filter (a
/// video output configured with HDR tags freezes the picture).
inline std::string filterChain(int mirror, bool withRetag) {
    const std::string flip = mirror == 1 ? "hflip" : (mirror == 2 ? "vflip" : "");
    if (!withRetag) return flip;
    return flip.empty() ? retagFilter() : retagFilter() + "," + flip;
}

/// What one file gets on top of the re-tag filter: the saturation and contrast added to the
/// viewer's own settings (mpv's -100 to 100 scale).
struct Plan {
    bool boost = false;
    int saturation = 0;
    int contrast = 0;
};

/// The plan for a file whose transfer is `gamma`, as mpv's video-params/gamma prints it. Nothing
/// for a file that is not PQ: HLG is close to SDR-compatible, and +15/+10 could over-boost it (it
/// still gets the re-tag filter, which every file has).
///
/// The amounts: PQ puts reference white (203 nits) at a code value near 0.58 and BT.2020 colors
/// are shown on BT.709 primaries, so shown as SDR the picture is flat and desaturated. Contrast
/// +10 lifts the whole range by a tenth, saturation +15 gives back part of the lost chroma; both
/// are moderate on purpose, as clipped highlights and neon colors are worse than flat ones.
inline Plan planFor(const std::string& gamma) {
    if (gamma != "pq") return {};
    return {true, 15, 10};
}

/// A value kept inside mpv's -100 to 100 range.
inline int clampEqualizer(int value) { return value < -100 ? -100 : (value > 100 ? 100 : value); }

/// "saturation +15, contrast +10" for the log; "none" for no boost.
inline std::string describe(const Plan& plan) {
    if (!plan.boost) return "none";
    return "saturation +" + std::to_string(plan.saturation) + ", contrast +" + std::to_string(plan.contrast);
}

}  // namespace hdr
