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
    void startPlayback(const int64_t seekMs, bool forceDirect = false);
    /// Tears the current Plex transcode session down server-side (no-op for
    /// direct play or non-Plex backends). Fire-and-forget; safe to call after
    /// `this` is gone.
    void stopTranscode();
    /// On a transcode playback error, retry once in direct play (helps Vita,
    /// where the hardware decoder can choke on the transcoded stream). Returns
    /// true when a fallback was started (so the error dialog is suppressed).
    bool tryDirectPlayFallback();
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
    /// per (re)load; reset by playMedia on every deliberate (re)start
    bool directPlayFallback = false;
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

#if defined(ENABLE_TORRENT)
    /// Live P2P buffering feedback (torrent sources only). Instead of a separate
    /// pill, this drives the VideoView's CENTRAL loading box (the spinner already
    /// shown on load): showTorrentLoading() puts up "connecting to the swarm…" for
    /// the whole resolvePlayback + mpv buffering window, a periodic timer samples
    /// torrent::EngineSession::stats() and rewrites the central label (peers / ⬇
    /// speed / buffered %), and it is retired the moment playback really starts
    /// (first LOADING_END / progress) or the player goes away. No extra view is
    /// created and nothing focusable is added, so OSD navigation is untouched. All
    /// of this compiles only when ENABLE_TORRENT is defined — an OFF build is
    /// byte-for-byte unchanged.
    void showTorrentLoading();    ///< central loading + "connecting" line, start the ticker
    void updateTorrentLoading();  ///< one stats() sample -> central label (ticker callback)
    void hideTorrentLoading();    ///< stop the ticker and clear the central message (idempotent)

    brls::RepeatingTimer torrentTicker;
    bool torrentBuffering = false;
#endif
};
