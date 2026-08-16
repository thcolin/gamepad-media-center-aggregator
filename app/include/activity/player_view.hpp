/*
    GMCA — Plex video player.
    Pipeline: PLEX_MIGRATION.md §2.7 (direct play, universal transcoder, timeline, scrobble).
*/

#pragma once

#include <borealis.hpp>
#include <utils/event.hpp>
#include <api/plex/types.hpp>

class VideoView;

class PlayerView : public brls::Box {
public:
    /// versionIndex selects which item.media[] source to play (default -1 = the
    /// first accessible version, i.e. unchanged Plex/Jellyfin behavior). The
    /// Stremio source picker passes an explicit index to honor the user's choice.
    PlayerView(const plex::Item& item, const int64_t seekMs = 0, int versionIndex = -1);
    ~PlayerView();

    /// Loads the show's episode list (previous/next navigation)
    void setSeries(const std::string& showRatingKey);
    void setTitie(const std::string& title);

#ifdef ANDROID
    void willDisappear(bool resetState) override {
        if (brls::Application::getThemeVariant() == brls::ThemeVariant::LIGHT)
            brls::Application::getTheme().addColor("brls/clear", nvgRGBA(235, 235, 235, 255));
        else
            brls::Application::getTheme().addColor("brls/clear", nvgRGBA(45, 45, 45, 255));
    }

    void willAppear(bool resetState) override {
        brls::Application::getTheme().addColor("brls/clear", nvgRGBA(0, 0, 0, 0));
    }
#endif

private:
    void setChapters(const std::vector<plex::Chapter>& chaps, int64_t durationMs);
    /// Fetches fresh metadata then resolves the playback URL via the backend
    void playMedia(const int64_t seekMs);
    /// Resolves the playback URL through the active backend (resolvePlayback,
    /// which decides direct vs transcode internally). forceDirect bypasses
    /// transcoding for the direct-play fallback after a transcode playback error
    /// (helps the Vita hardware decoder, which can choke on the transcoded stream).
    /// forceBitrate (bps, 0 = user setting) caps the stream for the
    /// transcode fallback after a direct-play error.
    void startPlayback(const int64_t seekMs, bool forceDirect = false, int64_t forceBitrate = 0);
    /// Tears the current Plex transcode session down server-side (no-op for
    /// direct play or non-Plex backends). Fire-and-forget; safe to call after
    /// `this` is gone.
    void stopTranscode();
    /// On a transcode playback error, retry once in direct play (helps Vita,
    /// where the hardware decoder can choke on the transcoded stream). Returns
    /// true when a fallback was started (so the error dialog is suppressed).
    bool tryDirectPlayFallback();
    /// Mirror image: on a direct-play error, retry once through the server
    /// transcoder (issue #50 — a server may refuse to serve the raw part, e.g.
    /// Plex remote/relay, while a transcode session opens fine). Returns true
    /// when a fallback was started (so the error dialog is suppressed).
    bool tryTranscodeFallback();
    bool playIndex(int index);
    /// Resolves external subtitle sidecars for the current item through the
    /// backend (Stremio addons), lazily and only when the played item changes.
    /// Plex/Jellyfin embed theirs in the Media streams, so this is a no-op there.
    void resolveExternalSubtitles();
    /// sub-adds the resolved external subtitles into mpv, selecting the track
    /// matching the preferred-language setting (PLAYER_SUBTITLE_LANG). Called on
    /// every (re)load — mpv drops sub-add'ed tracks on each loadfile.
    void addExternalSubtitles();
    /// POST /:/timeline report (time/duration in ms)
    void reportTimeline(const std::string& state, int64_t timeMs);
    void reportStop();
    /// Marks as watched via /:/scrobble beyond the threshold (90%)
    void maybeScrobble(int64_t timeMs);
    bool toggleQuality();

    // Playback
    std::string itemId;  // ratingKey
    /// playMethod: "directplay" | "transcode" (VideoProfile display)
    std::string playMethod;
    /// stable play-session id for the whole playback session
    std::string sessionId;
    /// Plex universal-transcoder session, extracted from the resolved transcode
    /// URL so stopTranscode() can free it server-side. Empty for direct play or
    /// non-Plex backends. Regenerated (by the backend) on every (re)start.
    std::string transcodeSession;
    plex::Item item;     // fresh metadata (media/chapters/markers)
    plex::Media stream;  // selected version
    /// caller-chosen source index (Stremio picker); -1 = first accessible.
    /// Reset to -1 on episode switch so binge auto-picks the best source.
    int preferredVersion = -1;
    bool scrobbled = false;
    /// guards tryDirectPlayFallback so a failing stream falls back at most once
    /// per (re)load; reset by playMedia on every deliberate (re)start. Each
    /// fallback direction sets BOTH flags: a source that already failed is
    /// never retried (direct -> transcode -> direct again would be pointless).
    bool directPlayFallback = false;
    /// same guard for tryTranscodeFallback (direct play -> transcode)
    bool transcodeFallback = false;
    /// seekMs of the last startPlayback call. A load that fails to OPEN leaves
    /// mpv playback_time at 0, so the fallbacks resume from this instead —
    /// otherwise a track/quality switch mid-film restarts the movie (GH #50).
    int64_t lastSeekMs = 0;
    /// bitrate cap armed by tryTranscodeFallback (0 = none). Once the transcoder
    /// rescued playback, later reloads (subtitle/audio switches, binge) skip the
    /// doomed direct-play attempt instead of replaying fail -> toast -> fallback
    /// on every stream edit. Cleared when the user explicitly picks a quality
    /// (toggleQuality) — an explicit "auto" retries direct play honestly.
    int64_t fallbackBitrate = 0;
    std::vector<plex::Item> episodes;

    /// External subtitle sidecars (Stremio addons) for the current item, resolved
    /// lazily at play time and sub-add'ed on each (re)load. `externalSubsItem` is
    /// the ratingKey they belong to, so quality/track switches (same item) don't
    /// re-fetch while an episode switch does. `mpvLoaded` guards the async->sub-add
    /// timing (add on load OR when the fetch lands, whichever is last).
    std::vector<plex::Stream> externalSubs;
    std::string externalSubsItem;
    bool mpvLoaded = false;

    MPVEvent::Subscription eventSubscribeID;
    brls::VoidEvent::Subscription exitSubscribeID;
    brls::Event<int>::Subscription playSubscribeID;
    brls::VoidEvent::Subscription settingSubscribeID;
    MPVCustomEvent::Subscription customEventSubscribeID;
    VideoView* view = nullptr;
};
