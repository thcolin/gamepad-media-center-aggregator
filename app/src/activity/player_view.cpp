/*
    GMCA — Plex video player.
    Verified pipeline: PLEX_MIGRATION.md §2.7.
    Units: mpv positions in seconds, Plex API in milliseconds,
    transcoder offset in whole seconds.
*/

#include <cstdlib>

#include "activity/player_view.hpp"
#include "api/plex.hpp"
#include "api/backend.hpp"
#include "utils/dialog.hpp"
#include "utils/misc.hpp"
#include "view/mpv_core.hpp"
#include "view/player_setting.hpp"
#include "view/video_view.hpp"
#include "view/video_profile.hpp"
#include "view/audio_player.hpp"
#if defined(ENABLE_TORRENT)
#include "torrent/session.hpp"  // ephemeral on-device torrent engine (desktop/switch, gated)
#endif

using namespace brls::literals;

/// "Watched" threshold: default value of the server preference
/// LibraryVideoPlayedThreshold
static const double SCROBBLE_THRESHOLD = 0.90;

PlayerView::PlayerView(const plex::Item& item, const int64_t seekMs, int versionIndex)
    : itemId(item.ratingKey), item(item), preferredVersion(versionIndex) {
    // take sole ownership of MPVCore: if music was playing, the audio controller
    // must stop owning the shared event bus (else it reports this video's
    // progress against the audio track and auto-advances over it). SPEC.md §11.
    AudioPlayer::instance().release();
    float width = brls::Application::contentWidth;
    float height = brls::Application::contentHeight;
    view = new VideoView();
    view->setDimensions(width, height);
    view->setWidthPercentage(100);
    view->setHeightPercentage(100);
    view->setId("video");
    this->setDimensions(width, height);
    this->addView(view);
    view->registerVideoQuality([this](...) { return this->toggleQuality(); });
    // direct-access OSD pickers; &stream lets them switch transcode-side
    // tracks (the Vita default) as well as embedded ones
    view->registerVideoSubtitle([this](...) {
        PlayerSetting::showSubtitleMenu(&this->stream);
        return true;
    });
    view->registerVideoAudio([this](...) {
        PlayerSetting::showAudioMenu(&this->stream);
        return true;
    });
    // transcode stream failed to play -> retry once in direct play before the
    // error dialog (Vita hardware decode can reject the transcoded stream)
    view->registerError([this](...) { return this->tryDirectPlayFallback(); });

    // stable session identifier (24 characters)
    this->sessionId = misc::randHex(12);

    auto& mpv = MPVCore::instance();

    brls::Application::pushActivity(new brls::Activity(this), brls::TransitionAnimation::NONE);

    playSubscribeID = view->getPlayEvent()->subscribe([this](int index) { this->playIndex(index); });

    settingSubscribeID = view->getSettingEvent()->subscribe([]() {
        brls::View* setting = new PlayerSetting();
        brls::Application::pushActivity(new brls::Activity(setting));
    });

    eventSubscribeID = mpv.getEvent()->subscribe([this](MpvEventEnum event) {
        auto& mpv = MPVCore::instance();
        switch (event) {
        case MpvEventEnum::MPV_RESUME:
            this->reportTimeline("playing", int64_t(mpv.video_progress) * 1000);
            view->getProfile()->init(this->playMethod);
            break;
        case MpvEventEnum::MPV_PAUSE:
            this->reportTimeline("paused", int64_t(mpv.video_progress) * 1000);
            break;
        case MpvEventEnum::LOADING_END:
            this->reportTimeline("playing", int64_t(mpv.playback_time) * 1000);
#if defined(ENABLE_TORRENT)
            // mpv finished its initial buffering from the local torrent HTTP server
            // — playback really started, so retire the P2P buffering message.
            this->hideTorrentLoading();
#endif
            break;
        case MpvEventEnum::MPV_STOP:
            this->mpvLoaded = false;
            this->reportStop();
            break;
        case MpvEventEnum::MPV_LOADED: {
            const char* flag = MPVCore::SUBS_FALLBACK ? "select" : "auto";
            // External (sidecar) subtitles embedded in the Media streams at detail
            // time (Plex/Jellyfin direct play).
            for (auto& part : this->stream.parts) {
                for (auto& s : part.streams) {
                    if (s.streamType != media::streamTypeSubtitle || s.key.empty()) continue;
                    std::string url = AppConfig::instance().backend().subtitleSidecarUrl(s.key);
                    mpv.command("sub-add", url.c_str(), flag, s.displayTitle.c_str());
                }
            }
            // External subtitles resolved lazily by the backend (Stremio addons):
            // mpv dropped the previous load's tracks, so (re)add them here. If the
            // fetch is still in flight, its callback adds them once it lands.
            this->mpvLoaded = true;
            this->addExternalSubtitles();
            break;
        }
        case MpvEventEnum::UPDATE_PROGRESS:
#if defined(ENABLE_TORRENT)
            // Safety net: the clock is advancing, so playback is under way even if
            // LOADING_END was missed — make sure the buffering message is gone.
            if (this->torrentBuffering && mpv.video_progress > 0) this->hideTorrentLoading();
#endif
            // report cadence: every 10 s
            if (mpv.video_progress % 10 == 0) {
                this->reportTimeline("playing", int64_t(mpv.video_progress) * 1000);
                this->maybeScrobble(int64_t(mpv.video_progress) * 1000);
            }
            break;
        default:;
        }
    });
    customEventSubscribeID = mpv.getCustomEvent()->subscribe([this](const std::string& event, void* data) {
        if (event == QUALITY_CHANGE) {
            // Quality/audio/subtitle change: the HLS transcode carries a single
            // audio track and no selectable subtitle, so switching means asking
            // the server for a fresh transcode and reloading it. Mirror
            // playIndex and reset() mpv first: reloading in place kept the old
            // (Vita hardware) decoder pinned, so the switch stalled and then
            // failed with a playback error. Read the position before reset()
            // zeroes it so the new transcode resumes where we were.
            int64_t pos = int64_t(MPVCore::instance().playback_time) * 1000;
            MPVCore::instance().reset();
            this->playMedia(pos);
        } else if (event == "PreviousTrack") {
            this->view->playNext(-1);
        } else if (event == "NextTrack") {
            this->view->playNext(1);
        }
    });

    this->playMedia(seekMs > 0 ? seekMs : item.viewOffset);

    // Report stop when application exit
    this->exitSubscribeID = brls::Application::getExitEvent()->subscribe([this]() {
        if (!MPVCore::instance().isStopped()) this->reportStop();
    });
}

PlayerView::~PlayerView() {
    auto& mpv = MPVCore::instance();
    mpv.getEvent()->unsubscribe(eventSubscribeID);
    mpv.getCustomEvent()->unsubscribe(customEventSubscribeID);
    view->getPlayEvent()->unsubscribe(playSubscribeID);
    view->getSettingEvent()->unsubscribe(settingSubscribeID);

    brls::sync([&mpv]() { mpv.getCustomEvent()->fire(VIDEO_CLOSE, nullptr); });

    PlayerSetting::selectedSubtitle = 0;
    PlayerSetting::selectedAudio = 0;

    if (!mpv.isStopped()) this->reportStop();
    // Free the server-side transcode session on exit (else it lingers orphaned).
    this->stopTranscode();
#if defined(ENABLE_TORRENT)
    // Stop the buffering ticker before we go (its callback captures this). The
    // RepeatingTimer would also self-stop on destruction, but do it explicitly.
    this->hideTorrentLoading();
    // Ephemeral torrent engine: tear it down when the player goes away (no-op when
    // this playback was not a torrent). The teardown is detached, so this returns
    // immediately (TORRENT_STREAMING.md §1 — moteur détruit à l'arrêt).
    torrent::EngineSession::instance().close();
#endif
    brls::Application::getExitEvent()->unsubscribe(this->exitSubscribeID);
    brls::Logger::debug("trying delete PlayerView...");
}

void PlayerView::setSeries(const std::string& showRatingKey) {
    ASYNC_RETAIN
    // all episodes of the show
    AppConfig::instance().backend().getAllEpisodes(showRatingKey, true,
        [ASYNC_TOKEN](const media::Container<media::Item>& r) {
            ASYNC_RELEASE
            int index = -1;
            std::vector<std::string> values;
            for (size_t i = 0; i < r.Items.size(); i++) {
                auto& it = r.Items.at(i);
                if (it.ratingKey == this->itemId) index = i;
                values.push_back(fmt::format("S{}E{} - {}", it.parentIndex, it.index, it.title));
            }
            view->setList(values, index);
            this->episodes = std::move(r.Items);
        },
        [ASYNC_TOKEN](const std::string& error) {
            ASYNC_RELEASE
            Dialog::show(error);
        });
}

void PlayerView::setTitie(const std::string& title) { this->view->setTitie(title); }

void PlayerView::setChapters(const std::vector<plex::Chapter>& chaps, int64_t durationMs) {
    std::vector<float> clips;
    if (durationMs > 0) {
        for (auto& c : chaps) {
            clips.push_back(float(c.startTimeOffset) / float(durationMs));
        }
    }
    this->view->setClipPoint(clips);
}

bool PlayerView::playIndex(int index) {
    if (index < 0 || index >= (int)this->episodes.size()) {
        return VideoView::close();
    }
    MPVCore::instance().reset();

    auto next = this->episodes.at(index);
    this->itemId = next.ratingKey;
    this->item = next;
    this->scrobbled = false;
    this->preferredVersion = -1;  // binge: auto-pick the best source for the new episode
    this->playMedia(0);
    view->setTitie(next.grandparentTitle.empty()
                       ? fmt::format("S{}E{} — {}", next.parentIndex, next.index, next.title)
                       : fmt::format("{} · S{}E{} — {}", next.grandparentTitle, next.parentIndex, next.index,
                             next.title));
    return true;
}

void PlayerView::playMedia(const int64_t seekMs) {
    // Capture/automation guard: in GMCA_NAV_PIPE mode a stray "Play" from the
    // screenshot harness must never actually start playback — doing so pushes a
    // watch-progress report to the server and pollutes Continue Watching. Bail
    // out immediately (the empty player pops itself, leaving us on the detail).
    if (std::getenv("GMCA_NAV_PIPE")) { VideoView::close(); return; }

    // Release any transcode session we were running before (re)loading. Covers
    // quality/track switches, episode navigation, and transcode->direct play.
    // Without it each reload orphaned a server-side session (verified on dev:
    // they stack up at ~0% progress and never free), starving new transcodes.
    this->stopTranscode();
#if defined(ENABLE_TORRENT)
    // Drop any torrent engine from the previous source before (re)loading — episode
    // navigation, quality/track switches, transcode->direct. A torrent (re)start
    // re-opens a fresh one in resolvePlayback; switching to a non-torrent source
    // frees it here so it never lingers (one playback at a time).
    torrent::EngineSession::instance().close();
#endif
    // deliberate (re)start: allow the direct-play fallback to trigger again
    this->directPlayFallback = false;

    // Fast path: the caller already resolved the exact source (Stremio source
    // picker passes the fully-resolved item + chosen index). Re-fetching would
    // re-resolve streams and could return a different order/set, silently playing
    // a different release than the one selected — so play the chosen one directly.
    {
        auto accessible = [](const plex::Media& m) {
            for (auto& p : m.parts)
                if (p.accessible && p.exists && !p.key.empty()) return true;
#if defined(ENABLE_TORRENT)
            // A raw-infoHash torrent carries no part yet (the engine mints the URL
            // at resolve time) — treat it as accessible so the chosen source
            // survives to resolvePlayback instead of being skipped for lack of key.
            if (m.kind == media::SourceKind::Torrent && !m.infoHash.empty()) return true;
#endif
            return false;
        };
        if (this->preferredVersion >= 0 && this->preferredVersion < (int)this->item.media.size() &&
            accessible(this->item.media[this->preferredVersion])) {
            this->stream = this->item.media[this->preferredVersion];
            this->setChapters(this->item.chapters, this->item.duration);
            this->startPlayback(seekMs);
            return;
        }
    }

    ASYNC_RETAIN
    // fresh metadata: Media/Part/Stream + chapters
    AppConfig::instance().backend().getItemDetail(
        this->itemId, true,
        [ASYNC_TOKEN, seekMs](const media::Item& item) {
            ASYNC_RELEASE
            this->item = item;

            // caller-chosen source (Stremio picker) if it still resolves to an
            // accessible file; otherwise the first accessible version.
            const plex::Media* chosen = nullptr;
            auto accessible = [](const plex::Media& m) {
                for (auto& p : m.parts)
                    if (p.accessible && p.exists && !p.key.empty()) return true;
#if defined(ENABLE_TORRENT)
                if (m.kind == media::SourceKind::Torrent && !m.infoHash.empty()) return true;
#endif
                return false;
            };
            if (this->preferredVersion >= 0 && this->preferredVersion < (int)this->item.media.size() &&
                accessible(this->item.media[this->preferredVersion])) {
                chosen = &this->item.media[this->preferredVersion];
            }
            for (auto& m : this->item.media) {
                if (chosen) break;
                if (accessible(m)) chosen = &m;
            }
            if (!chosen) {
                Dialog::show("main/player/error"_i18n, []() { VideoView::close(); });
                return;
            }
            this->stream = *chosen;
            this->setChapters(this->item.chapters, this->item.duration);
            this->startPlayback(seekMs);
        },
        [ASYNC_TOKEN](const std::string& ex) {
            ASYNC_RELEASE
            Dialog::show(ex, []() { VideoView::close(); });
        });
}

void PlayerView::startPlayback(const int64_t seekMs, bool forceDirect) {
    // We are about to (re)load: mpv will drop any sub-add'ed tracks. Clear the
    // loaded flag so a subtitle fetch landing mid-load waits for MPV_LOADED to
    // re-add. (External subtitles are resolved AFTER the playback task is queued
    // — see the note at the end of this function.)
    this->mpvLoaded = false;

    media::PlaybackOptions opts;
    opts.seekMs = seekMs;
    opts.bitrateCap = MPVCore::VIDEO_QUALITY;
    // forceDirect: the transcode->direct-play fallback re-resolves with direct
    // play forced (resolvePlayback returns the direct source when set).
    opts.forceDirectPlay = MPVCore::FORCE_DIRECTPLAY || forceDirect;
    opts.audioStreamId = PlayerSetting::selectedAudio;
    opts.subtitleStreamId = PlayerSetting::selectedSubtitle;
    opts.burnSubtitles = PlayerSetting::selectedSubtitle > 0;
    // transcode target codec: kept identical to the former hard-coded value
    // (MPVCore::VIDEO_CODEC was never wired into the Plex transcoder — see
    // MULTI_BACKEND.md §6); revisit when exposing the codec choice per backend
    opts.videoCodec = "h264";
    opts.sessionId = this->sessionId;

    // copies for the worker thread (avoids racing on this->item during a switch)
    media::Item item = this->item;
    media::Media version = this->stream;

#if defined(ENABLE_TORRENT)
    // Live buffering feedback: resolvePlayback below blocks (on the worker) while the
    // engine acquires metadata + finds peers, and mpv's own buffering spinner only
    // kicks in once it has the URL. Drive the central loading label (peers / ⬇ speed /
    // buffered %) for the whole window; it retires itself when playback starts. A
    // non-torrent source clears any message left over from a previous torrent.
    if (version.kind == media::SourceKind::Torrent && !version.infoHash.empty())
        this->showTorrentLoading();
    else
        this->hideTorrentLoading();
#endif

    ASYNC_RETAIN
    brls::async([ASYNC_TOKEN, item, version, opts]() {
        try {
            // resolvePlayback runs the transcode decision synchronously and
            // throws on failure; the direct-play fallback is internal. The Plex
            // universal-transcoder request (incl. the Vita 1080p height cap) is
            // built here — see PlexBackend::resolvePlayback.
            media::PlaybackSource src = AppConfig::instance().backend().resolvePlayback(item, version, opts);
            brls::sync([ASYNC_TOKEN, src]() {
                ASYNC_RELEASE
                // A backend may report "nothing playable" with an empty url
                // (e.g. Stremio with no direct/debrid stream) instead of throwing
                // across the async/TU boundary; surface it as a player error.
                if (src.url.empty()) {
                    Dialog::show("main/player/error"_i18n, []() { VideoView::close(); });
                    return;
                }
                this->playMethod = src.playMethod;
                // Remember the backend transcode session so we can tear it down
                // server-side on reload/exit — the dev Vita fix. Set by the backend
                // (Plex); empty for direct play or backends without a server
                // transcode session, leaving stopTranscode a safe no-op.
                this->transcodeSession = src.transcodeSession;
                MPVCore::instance().setUrl(src.url, src.mpvExtra);
            });
        } catch (const std::exception& ex) {
            std::string msg = ex.what();
            brls::sync([ASYNC_TOKEN, msg]() {
                ASYNC_RELEASE
                Dialog::show(msg, []() { VideoView::close(); });
            });
        }
    });

    // Resolve external subtitles AFTER queuing the playback task above. brls::async
    // is a single FIFO worker thread (not a pool): the Stremio subtitle fan-out
    // (ensureLoaded + one getSync per subtitles addon, up to a 15 s timeout each)
    // would otherwise run to completion BEFORE the fast resolvePlayback task and
    // stall the video start behind it. Queuing playback first lets mpv start
    // loading while subtitles resolve; the mpvLoaded/addExternalSubtitles handoff
    // adds them whenever the fetch lands. (No-op for Plex/Jellyfin: getSubtitles
    // returns synchronously.)
    this->resolveExternalSubtitles();
}

void PlayerView::resolveExternalSubtitles() {
    // Per-video set, same across every source/quality — skip when already resolved
    // for the current item (startPlayback re-enters on quality/track switches).
    const std::string& key = this->item.ratingKey;
    if (key.empty() || key == this->externalSubsItem) return;
    this->externalSubsItem = key;
    this->externalSubs.clear();  // drop the previous item's subs before the switch lands

    ASYNC_RETAIN
    AppConfig::instance().backend().getSubtitles(
        this->item,
        [ASYNC_TOKEN, key](std::vector<media::Stream> subs) {
            ASYNC_RELEASE
            // a newer switch superseded this fetch -> its result is stale
            if (key != this->externalSubsItem) return;
            this->externalSubs = std::move(subs);
            // if the file is already playing, add now; otherwise MPV_LOADED will
            if (this->mpvLoaded) this->addExternalSubtitles();
        },
        [ASYNC_TOKEN, key](const std::string&) {
            ASYNC_RELEASE
            // resolution failed (offline / addon error): leave the set empty, the
            // player still plays; no dialog (subtitles are best-effort).
        });
}

void PlayerView::addExternalSubtitles() {
    if (this->externalSubs.empty()) return;
    auto& mpv = MPVCore::instance();
    auto& backend = AppConfig::instance().backend();

    // Preferred language: "auto" follows the app locale, "off" disables auto-
    // selection, otherwise an explicit 2-letter code (PLAYER_SUBTITLE_LANG).
    std::string pref = AppConfig::instance().getItem(AppConfig::PLAYER_SUBTITLE_LANG, std::string("auto"));
    if (pref == "auto") {
        std::string loc = brls::Application::getLocale();  // "es", "en-US", "zh-Hans"...
        pref = loc.substr(0, loc.find('-'));
    } else if (pref == "off") {
        pref.clear();
    }

    for (auto& s : this->externalSubs) {
        if (s.key.empty()) continue;
        std::string url = backend.subtitleSidecarUrl(s.key);
        // select the track matching the preferred language; add the rest as
        // "auto" so they stay pickable in the subtitle menu without stealing it.
        bool preferred = !pref.empty() && s.languageTag == pref;
        const char* flag = preferred ? "select" : "auto";
        mpv.command("sub-add", url.c_str(), flag, s.displayTitle.c_str(), s.languageTag.c_str());
    }
}

bool PlayerView::tryDirectPlayFallback() {
    auto& mpv = MPVCore::instance();
    // only recover a failed transcode, and only once per (re)load
    if (this->playMethod != "transcode" || this->directPlayFallback) return false;
    this->directPlayFallback = true;

    int64_t pos = int64_t(mpv.playback_time) * 1000;  // read before reset() zeroes it
    brls::Logger::error("PlayerView: transcode playback failed ({}) — falling back to direct play at {} ms",
        mpv.getError(), pos);
    mpv.reset();            // release the (Vita hardware) decoder held by the failed stream
    this->stopTranscode();  // drop the dead transcode session server-side
    this->startPlayback(pos, /*forceDirect=*/true);  // re-resolve, forcing direct play
    // surface the reason to the user too, so bug reports carry the mpv code
    brls::Application::notify(fmt::format("{} ({})", "main/player/direct_fallback"_i18n, mpv.getError()));
    return true;  // handled: no error dialog
}

void PlayerView::stopTranscode() {
    if (this->transcodeSession.empty()) return;
    auto& conf = AppConfig::instance();
    // Fire-and-forget: getAction copies url+token, so it is safe even if this
    // PlayerView is being destroyed. A stale session id just 404s server-side.
    plex::getAction(conf.getUrl(), conf.getToken(), nullptr, plex::apiTranscodeStop,
        HTTP::encode_form({{"session", this->transcodeSession}}));
    this->transcodeSession.clear();
}

void PlayerView::reportTimeline(const std::string& state, int64_t timeMs) {
    media::PlayState st = state == "paused"    ? media::PlayState::Paused
                          : state == "stopped" ? media::PlayState::Stopped
                                               : media::PlayState::Playing;
    AppConfig::instance().backend().reportProgress(this->itemId, st, timeMs, this->item.duration, this->sessionId);
}

void PlayerView::reportStop() {
    int64_t timeMs = int64_t(MPVCore::instance().playback_time) * 1000;
    this->reportTimeline("stopped", timeMs);
    this->maybeScrobble(timeMs);
    brls::Logger::debug("PlayerView reportStop {}", this->sessionId);
}

void PlayerView::maybeScrobble(int64_t timeMs) {
    // state=stopped is NOT enough to mark as watched: explicit scrobble required
    if (this->scrobbled || this->item.duration <= 0) return;
    if (double(timeMs) / double(this->item.duration) < SCROBBLE_THRESHOLD) return;
    this->scrobbled = true;
    AppConfig::instance().backend().markWatched(this->itemId);
}

bool PlayerView::toggleQuality() {
    std::vector<std::string> options = {"main/player/auto"_i18n};
    std::vector<int64_t> values = {0};
    int64_t videoBitRate = this->stream.bitrate * 1000;  // Plex: kbps -> bps

    if (videoBitRate >= 15000000) options.push_back("20 Mbps"), values.push_back(20000000);
    if (videoBitRate >= 10000000) options.push_back("15 Mbps"), values.push_back(15000000);
    if (videoBitRate >= 8000000) options.push_back("10 Mbps"), values.push_back(10000000);
    if (videoBitRate >= 6000000) options.push_back("8 Mbps"), values.push_back(8000000);
    if (videoBitRate >= 4000000) options.push_back("6 Mbps"), values.push_back(6000000);
    if (videoBitRate >= 3000000) options.push_back("4 Mbps"), values.push_back(4000000);
    if (videoBitRate >= 1500000) options.push_back("3 Mbps"), values.push_back(3000000);
    if (videoBitRate >= 720000) options.push_back("1.5 Mbps"), values.push_back(1500000);
    options.push_back("720 kbps"), values.push_back(720000);
    options.push_back("420 kbps"), values.push_back(420000);

    auto it = std::find(values.begin(), values.end(), MPVCore::VIDEO_QUALITY);
    if (it == values.end()) it = values.begin();

    brls::Dropdown* dropdown = new brls::Dropdown(
        "main/player/quality"_i18n, options,
        [values](int selected) {
            MPVCore::VIDEO_QUALITY = values[selected];
            // remember the choice across launches (Vita users had to re-lower
            // it every session otherwise — see config.cpp default)
            AppConfig::instance().setItem(AppConfig::PLAYER_VIDEO_QUALITY, MPVCore::VIDEO_QUALITY);
            MPVCore::instance().getCustomEvent()->fire(QUALITY_CHANGE, nullptr);
            return true;
        },
        std::distance(values.begin(), it));

    brls::Application::pushActivity(new brls::Activity(dropdown));
    return true;
}

#if defined(ENABLE_TORRENT)
// --- torrent buffering feedback (central loading) --------------------------------
//
// A raw-infoHash torrent needs time before playback: resolvePlayback blocks on the
// worker while the engine fetches metadata and finds peers, and mpv's own spinner
// only starts once it has the local URL. So we reuse the VideoView's CENTRAL loading
// box (already the app's "loading" affordance) and, for torrents only, drive its
// label with a live P2P status line. A brls::RepeatingTimer samples
// torrent::EngineSession::stats() on the main thread and rewrites the label; it is
// retired the moment playback really starts (LOADING_END / first progress) or the
// player is destroyed. No extra view is created and nothing focusable is added, so
// OSD navigation is untouched.

void PlayerView::showTorrentLoading() {
    this->torrentBuffering = true;
    // Put the central loading (spinner + label) up front with the "connecting to the
    // swarm" line: mpv has no URL yet, so this covers the resolvePlayback window too.
    this->view->setCenterLoadingMessage("main/stremio/source/torrent_buffering"_i18n);
    this->torrentTicker.setCallback([this]() { this->updateTorrentLoading(); });
    this->torrentTicker.start(800);  // ~1.25 samples/s, main-thread (RepeatingTimer)
    this->updateTorrentLoading();
}

void PlayerView::updateTorrentLoading() {
    if (!this->torrentBuffering) return;
    torrent::Stats st = torrent::EngineSession::instance().stats();

    std::string text;
    if (!st.metadataReady || st.peersConnected == 0) {
        // Still bootstrapping (fetching metadata / discovering peers): keep the
        // connecting line — the log below carries the granular state for diagnostics.
        text = "main/stremio/source/torrent_buffering"_i18n;
    } else {
        // 🌐 N peers  ·  ⬇ speed  ·  P% — speed unit and % are language-neutral; only
        // the peer word is localized (torrent_peers = "{} peers", one positional arg).
        std::string speed =
            st.downloadRateBps > 0 ? misc::formatSize((uint64_t)st.downloadRateBps) + "/s" : "0KB/s";
        int pct = st.piecesTotal > 0 ? (int)(100.0 * st.piecesHave / st.piecesTotal) : 0;
        std::string peers =
            fmt::format(fmt::runtime("main/stremio/source/torrent_peers"_i18n), st.peersConnected);
        text = fmt::format("\xF0\x9F\x8C\x90 {}  \xC2\xB7  \xE2\xAC\x87 {}  \xC2\xB7  {}%", peers, speed, pct);
    }
    this->view->setCenterLoadingMessage(text);

    // Buffering diagnostics (also the proof-of-refresh trace asked for by the spec).
    brls::Logger::debug(
        "torrent buffering: meta={} peers={}/{} rate={:.0f}B/s pieces={}/{} contiguous={}B webseeds={}",
        st.metadataReady, st.peersConnected, st.peersKnown, st.downloadRateBps, st.piecesHave, st.piecesTotal,
        st.contiguousReadyBytes, st.webSeeds);
}

void PlayerView::hideTorrentLoading() {
    if (!this->torrentBuffering) return;  // idempotent — only clear an active session
    this->torrentBuffering = false;
    this->torrentTicker.stop();
    this->view->clearCenterLoadingMessage();
}
#endif
