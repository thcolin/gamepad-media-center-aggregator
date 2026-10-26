#include <borealis.hpp>

#include "utils/config.hpp"
#include "utils/download.hpp"
#include "utils/offline_library.hpp"
#include "utils/image_cache.hpp"
#include "utils/network_state.hpp"
#include "utils/thread.hpp"

#include "view/svg_image.hpp"
#include "view/disclosure_cell.hpp"
#include "view/icon_button.hpp"
#include "view/context_menu.hpp"
#include "view/custom_button.hpp"
#include "view/auto_tab_frame.hpp"
#include "view/recycling_grid.hpp"
#include "view/h_recycling.hpp"
#include "view/recyling_video.hpp"
#include "view/video_progress_slider.hpp"
#include "view/gallery_view.hpp"
#include "view/search_list.hpp"
#include "view/video_view.hpp"
#include "view/selector_cell.hpp"
#include "view/button_close.hpp"
#include "view/text_box.hpp"
#include "view/mpv_core.hpp"

#include "activity/main_activity.hpp"
#include "activity/server_list.hpp"
#include "activity/hint_activity.hpp"
#include "activity/loading_activity.hpp"
#include "tab/home_tab.hpp"
#include "tab/search_tab.hpp"
#include "tab/remote_tab.hpp"
#include "tab/remote_view.hpp"
#include "tab/setting_tab.hpp"
#include "tab/playlists_tab.hpp"
#include "tab/watchlist_tab.hpp"

#if defined(__SDL2__)
#include <SDL2/SDL_main.h>
#endif

#ifdef __PS4__
#include <csignal>
#include <sys/stat.h>
#include "utils/dialog.hpp"
#include "utils/ps4_self_update.hpp"
#endif

using namespace brls::literals;  // for _i18n

#if defined(__SWITCH__) && defined(BUILTIN_NSP)
#include <switch.h>

/// Is the HOME tile (forwarder NSP, title id FORWARDER_TITLEID =
/// PROJECT_TITLEID from CMakeLists.txt) already installed? Best effort: if
/// the ns service fails we answer "no" (the prompt is only shown once
/// anyway, cf. AppConfig::HINT_FORWARDER).
static bool isForwarderInstalled() {
    if (R_FAILED(nsInitialize())) return false;
    bool found = false;
    NsApplicationRecord record;
    s32 count = 0;
    for (s32 offset = 0; R_SUCCEEDED(nsListApplicationRecord(&record, 1, offset, &count)) && count > 0; offset++) {
        if (record.application_id == FORWARDER_TITLEID) {
            found = true;
            break;
        }
    }
    nsExit();
    return found;
}

/// First launch in application mode: offers to install the HOME tile
/// (launched from the tile itself or tile already there -> ns sees it -> nothing).
static void proposeForwarderInstall() {
    if (isForwarderInstalled()) return;  // a GMCA HOME tile is already present

    auto& conf = AppConfig::instance();
    // One-time GMCA-era nudge, keyed separately from HINT_FORWARDER on purpose:
    // pleNx users who self-updated to GMCA are past the pleNx-era HINT_FORWARDER
    // gate yet have no GMCA tile (fresh title id), so re-offer it exactly once.
    if (conf.getItem(AppConfig::HINT_FORWARDER_GMCA, false)) return;
    conf.setItem(AppConfig::HINT_FORWARDER_GMCA, true);
    conf.setItem(AppConfig::HINT_FORWARDER, true);

    auto dialog = new brls::Dialog("main/hints/prompt"_i18n);
    dialog->addButton("hints/cancel"_i18n, []() {});
    dialog->addButton("hints/ok"_i18n, []() { brls::Application::pushActivity(new HintActivity()); });
    dialog->open();
}
#endif

#ifdef GMCA_UPDATE_TEST
#include "utils/ps4_self_update.hpp"
#include "utils/ps4_system.hpp"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>
#ifndef GMCA_UPDATE_TEST_TAG
#define GMCA_UPDATE_TEST_TAG "?"
#endif
// Diag-only self-update test (a throwaway diag title id, never real GMCA). If a package was
// dropped at target.pkg, move it to the update folder and hand it to the update helper the way
// the real updater will, then close so the helper can install it. Every launch logs which build
// runs and the helper's last status, then (with nothing dropped) launches normally.
static void gmca_update_test_maybe_run() {
    const char* dir = "/data/GMCA-update-test";
    const char* target = "/data/GMCA-update-test/target.pkg";
    const char* logpath = "/data/GMCA-update-test/update-test.log";
    mkdir(dir, 0777);
    std::FILE* log = std::fopen(logpath, "a");
    if (log) {
        std::fprintf(log, "\n==== launched, tag=%s version=%s ====\n", GMCA_UPDATE_TEST_TAG,
            AppVersion::getVersion().c_str());
        std::string last = ps4::update::lastStatus();
        std::fprintf(log, "helper's last status: %s (its log: %s)\n", last.empty() ? "none" : last.c_str(),
            ps4::update::helperLog);
        std::fflush(log);
    }
    struct stat st;
    if (stat(target, &st) != 0) {
        if (log) {
            std::fprintf(log, "no target.pkg, continuing to a normal launch\n");
            std::fclose(log);
        }
        return;
    }
    // Moved, not copied: a failed install keeps it in the update folder instead of retrying on
    // every launch.
    mkdir("/data/GMCA", 0777);
    mkdir(ps4::update::dir, 0777);
    unlink(ps4::update::stagedPkg);
    int moved = rename(target, ps4::update::stagedPkg);
    if (log) std::fprintf(log, "move target.pkg -> %s: %s\n", ps4::update::stagedPkg, moved == 0 ? "ok" : std::strerror(errno));
    std::string err;
    bool ok = moved == 0 && ps4::update::start(ps4::update::stagedPkg, "(test build)", err, log);
    if (log) {
        std::fprintf(log, "update start %s%s\n", ok ? "OK, closing for the helper" : "FAILED: ", ok ? "" : err.c_str());
        std::fclose(log);
    }
    if (!ok) {
        ps4::notify("Update test failed: " + (moved == 0 ? err : std::string("could not move target.pkg")));
        return;  // launch normally; update-test.log has the steps
    }
    ps4::loadExec("exit");  // the system's way out, so the helper sees the title closed
    std::exit(0);
}
#endif

int main(int argc, char* argv[]) {
#ifdef __SWITCH__
    if (argc > 0 && argv[0]) AppVersion::nro_path = argv[0];
#endif
#ifdef __PS4__
    // A send on a socket the server already closed raises SIGPIPE, whose default
    // action kills the app with nothing logged. curl runs with CURLOPT_NOSIGNAL so it
    // leaves SIGPIPE alone, and on PS4 MSG_NOSIGNAL doesn't take effect (musl's Linux
    // 0x4000 is FreeBSD's MSG_NBIO) and SO_NOSIGPIPE isn't defined in the headers.
    // Ignore it: the send fails with EPIPE instead.
    std::signal(SIGPIPE, SIG_IGN);
#endif
    std::vector<std::string> items;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "-d") == 0) {
            brls::Logger::setLogLevel(brls::LogLevel::LOG_DEBUG);
        } else if (std::strcmp(argv[i], "-v") == 0) {
            brls::Application::enableDebuggingView(true);
        } else if (std::strcmp(argv[i], "-t") == 0) {
            MPVCore::DEBUG = true;
        } else if (std::strcmp(argv[i], "-o") == 0) {
            const char* path = (i + 1 < argc) ? argv[++i] : "gmca.log";
            FILE* logFile = std::fopen(path, "w+");
            // line-buffered: without this the last ~16 KB of logs (including
            // the line preceding a crash) stay in the buffer and die with
            // the process — unbearable for diagnosing
            if (logFile) std::setvbuf(logFile, nullptr, _IOLBF, 0);
            brls::Logger::setLogOutput(logFile);
        } else if (std::strcmp(argv[i], "-version") == 0) {
            brls::Logger::info("{} {}", AppVersion::getDeviceName(), AppVersion::getCommit());
            return 0;
        } else {
            items.push_back(argv[i]);
        }
    }

#ifdef GMCA_UPDATE_TEST
    gmca_update_test_maybe_run();
#endif
    std::setlocale(LC_ALL, "C.UTF-8");
    // Load cookies and settings
    auto& conf = AppConfig::instance();
    if (!conf.init()) {
        return 0;
    }

    // Init the app and i18n
    if (!brls::Application::init()) {
        brls::Logger::error("Unable to init application");
        return EXIT_FAILURE;
    }

    conf.initThemes();

    // Scroll indicator (scrollbar) visibility — global, driven by config
    // ("scrollbar", default true). Off = clean captures / a quieter chrome.
    brls::ScrollingFrame::setScrollingIndicatorVisibleGlobal(conf.getItem(AppConfig::SCROLLBAR, true));

    // Screenshot/automation harness (GMCA_NAV_PIPE input hook): keep the render
    // + input loop at full speed even while unfocused, so background navigation
    // and captures stay in sync (the default 5 FPS idle throttle desyncs them).
    if (std::getenv("GMCA_NAV_PIPE")) brls::Application::setDeactivatedFPS(60);

    ImageCache::init();
    DownloadManager::instance().init();
    OfflineLibrary::instance().init();

    // Return directly to the desktop when closing the application (only for NX)
    brls::Application::getPlatform()->exitToHomeMode(true);

    brls::Application::createWindow(fmt::format("{} for {}", AppVersion::getPackageName(), AppVersion::getPlatform()));

    // Have the application register an action on every activity that will quit when you press BUTTON_START
    brls::Application::setGlobalQuit(false);

    // Register custom views (including tabs, which are views)
    brls::Application::registerXMLView("SVGImage", SVGImage::create);
    brls::Application::registerXMLView("DisclosureCell", DisclosureCell::create);
    brls::Application::registerXMLView("IconButton", IconButton::create);
    brls::Application::registerXMLView("MenuItem", MenuItem::create);
    brls::Application::registerXMLView("CustomButton", CustomButton::create);
    brls::Application::registerXMLView("SelectorCell", SelectorCell::create);
    brls::Application::registerXMLView("TextBox", TextBox::create);
    brls::Application::registerXMLView("ButtonClose", ButtonClose::create);
    brls::Application::registerXMLView("AutoTabFrame", AutoTabFrame::create);
    brls::Application::registerXMLView("MainTabFrame", MainTabFrame::create);
    brls::Application::registerXMLView("RecyclingGrid", RecyclingGrid::create);
    brls::Application::registerXMLView("HRecyclerFrame", HRecyclerFrame::create);
    brls::Application::registerXMLView("RecylingVideo", RecylingVideo::create);
    brls::Application::registerXMLView("GalleryView", GalleryView::create);
    brls::Application::registerXMLView("SearchList", SearchList::create);
    brls::Application::registerXMLView("VideoProgressSlider", VideoProgressSlider::create);

    brls::Application::registerXMLView("HomeTab", HomeTab::create);
    brls::Application::registerXMLView("SearchTab", SearchTab::create);
    brls::Application::registerXMLView("RemoteTab", RemoteTab::create);
    brls::Application::registerXMLView("SettingTab", SettingTab::create);
    brls::Application::registerXMLView("PlaylistsTab", PlaylistsTab::create);
    brls::Application::registerXMLView("WatchlistTab", WatchlistTab::create);

    if (!brls::Application::getPlatform()->isApplicationMode()) {
        brls::Application::pushActivity(new HintActivity());
    } else if (items.size() > 0) {
        RemoteView::play(items.front());
    } else {
        // checkLogin() probes the remembered URLs of the active server
        // (config.cpp:checkLogin, now raced in parallel): called here on the
        // main thread it froze the very first frame for several seconds
        // (changed network, server off...). Show the loading screen and probe
        // in the background.
        brls::Application::pushActivity(new LoadingActivity(), brls::TransitionAnimation::NONE);
        brls::Application::blockInputs();
        brls::async([]() {
            const bool logged = AppConfig::instance().checkLogin();
            brls::sync([logged]() {
                brls::Application::unblockInputs();
                brls::Application::clear();
                if (logged) {
                    brls::Application::pushActivity(new MainActivity(), brls::TransitionAnimation::NONE);
                    // downloads interrupted by the last exit (init() re-queued them)
                    DownloadManager::instance().resumeQueue();
#if defined(__SWITCH__) && defined(BUILTIN_NSP)
                    proposeForwarderInstall();
#endif
                } else if (!OfflineLibrary::instance().empty() ||
                           !AppConfig::instance().getServers().empty()) {
                    // a server is remembered (just unreachable) and/or downloads
                    // exist: enter the offline shell (browse downloads + a Retry
                    // to reconnect) instead of the server picker (SPEC §4.4).
                    // A fresh install with no server still goes to ServerList.
                    NetworkState::setOffline(true);
                    brls::Application::pushActivity(new MainActivity(), brls::TransitionAnimation::NONE);
                } else {
                    brls::Application::pushActivity(new ServerList(), brls::TransitionAnimation::NONE);
                }
            });
        });
    }

    std::string v = conf.getItem(AppConfig::APP_UPDATE, std::string("NaN"));
    if (AppVersion::getVersion().compare(v)) AppVersion::checkUpdate();

#ifdef __PS4__
    // What the last in-app update did (utils/ps4_self_update.hpp): a failure is shown once, a
    // success was already announced by the system's notification. "waiting" or "installing" stay:
    // the helper may still be at work, and the next update clears them.
    std::string lastUpdate = ps4::update::lastStatus();
    if (lastUpdate.rfind("failed", 0) == 0) {
        std::string why = lastUpdate.size() > 7 ? lastUpdate.substr(7) : lastUpdate;
        struct stat st;
        bool kept = stat(ps4::update::stagedPkg, &st) == 0;
        brls::sync([why, kept]() {
            std::string msg = brls::getStr("main/setting/others/ps4_update_failed", why);
            if (kept) msg += "\n" + brls::getStr("main/setting/others/ps4_update_kept", ps4::update::stagedPkg);
            Dialog::show(msg);
        });
    }
    if (lastUpdate.rfind("failed", 0) == 0 || lastUpdate.rfind("done", 0) == 0) ps4::update::clearStatus();
    // and take out what it left under /system, once startup is over (the cleanup runs inside the
    // escape, where a module load from another thread would end the app)
    ThreadPool::instance().submit([](HTTP& s) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        std::FILE* log = std::fopen(ps4::update::appLog, "a");
        ps4::update::cleanUp(log);
        if (log) std::fclose(log);
    });
#endif

    // Run the app
    while (brls::Application::mainLoop());

    ThreadPool::instance().stop();

    conf.checkRestart(argv);
    // Exit
    return EXIT_SUCCESS;
}
