#ifdef __PS4__
#include "utils/ps4_self_update.hpp"

#include "utils/ps4_escape.hpp"
#include "utils/ps4_system.hpp"

#include <cerrno>
#include <cstdarg>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include <orbis/Sysmodule.h>

// Set by CMakeLists.txt: this title, the helper's and the name on the home screen.
#ifndef GMCA_PS4_TITLE_ID
#error "GMCA_PS4_TITLE_ID is not defined"
#endif
#ifndef GMCA_PS4_UPDATER_TITLE_ID
#error "GMCA_PS4_UPDATER_TITLE_ID is not defined"
#endif
#ifndef GMCA_PS4_APP_NAME
#define GMCA_PS4_APP_NAME "GMCA"
#endif

namespace {

void lg(std::FILE* f, const char* fmt, ...) {
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(f, fmt, ap);
    va_end(ap);
    std::fputc('\n', f);
    std::fflush(f);
}

const std::string helperDir = std::string("/system/vsh/app/") + GMCA_PS4_UPDATER_TITLE_ID;

struct HelperFile {
    const char* name;  // under helperDir
    std::string data;
};

// A system daemon's param.sfo to give the helper (Gold Torrent's service uses the same two).
std::string daemonSfo(std::FILE* log) {
    std::string sfo;
    for (const char* from : {"/system/vsh/app/NPXS21007/sce_sys/param.sfo", "/system/vsh/app/NPXS20119/sce_sys/param.sfo"}) {
        if (ps4::readFile(from, sfo) && !sfo.empty()) {
            lg(log, "helper param.sfo from %s (%zu bytes)", from, sfo.size());
            return sfo;
        }
    }
    lg(log, "no system daemon param.sfo to copy");
    return "";
}

// Writes the helper under /system, eboot.bin last. /system is read-only again on every way out.
bool installHelper(const std::vector<HelperFile>& files, std::FILE* log) {
    int rc = ps4::remountSystem(true);
    lg(log, "/system read-write -> %d", rc);
    bool ok = rc == 0;
    if (ok) {
        for (const char* sub : {"", "/sce_sys", "/sce_module"}) {
            const std::string d = helperDir + sub;
            if (mkdir(d.c_str(), 0777) != 0 && errno != EEXIST) {
                lg(log, "mkdir %s: %s", d.c_str(), std::strerror(errno));
                ok = false;
            }
        }
        for (const HelperFile& f : files) {
            if (!ok) break;
            const std::string path = helperDir + "/" + f.name;
            const int e = ps4::writeFile(path, f.data);
            lg(log, "write %s (%zu bytes) -> %s", path.c_str(), f.data.size(), e ? std::strerror(e) : "ok");
            ok = e == 0;
        }
    }
    rc = ps4::remountSystem(false);
    lg(log, "/system read-only -> %d", rc);
    return ok;
}

}  // namespace

namespace ps4::update {

bool start(const std::string& pkgPath, const std::string& label, std::string& err, std::FILE* log) {
    // BGFT reads the package through the system's path for /data
    if (pkgPath.rfind("/data/", 0) != 0) {
        err = "the package must be under /data";
        return false;
    }
    const std::string contentId = ps4::pkgContentId(pkgPath);
    lg(log, "package %s: content id '%s'", pkgPath.c_str(), contentId.c_str());
    if (contentId.size() < 16 || contentId.compare(7, 9, GMCA_PS4_TITLE_ID) != 0) {
        err = contentId.empty() ? "the download is not a PS4 package" : "the package is not for this app";
        return false;
    }

    // The helper's files, read while /app0 is still this app's own (the escape changes the root).
    std::vector<HelperFile> files = {
        {"sce_sys/param.sfo", ""},
        {"sce_module/libc.prx", ""},
        {"sce_module/libSceFios2.prx", ""},
        {"eboot.bin", ""},
    };
    const char* from[] = {nullptr, "/app0/sce_module/libc.prx", "/app0/sce_module/libSceFios2.prx", "/app0/updater.self"};
    for (size_t i = 1; i < files.size(); ++i) {
        const bool read = ps4::readFile(from[i], files[i].data) && !files[i].data.empty();
        lg(log, "read %s: %s (%zu bytes)", from[i], read ? "ok" : std::strerror(errno), files[i].data.size());
        if (!read) {
            err = "the update helper is missing from this build";
            return false;
        }
    }

    // Loaded and resolved before the escape: loader calls inside it end the app. Loaded here, as
    // Gold Torrent's app does, because the diag runs before anything else has loaded it.
    const unsigned loaded = sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_SYSTEM_SERVICE);
    const int resolved = ps4::resolveSystemService();
    lg(log, "SystemService: load 0x%08x, %d of 4 functions", loaded, resolved);
    if (resolved < 4) lg(log, "modules: %s", ps4::loadedModules().c_str());

    mkdir("/data/GMCA", 0777);
    mkdir(dir, 0777);
    unlink(statusFile);
    const std::string request = std::string("title=") + GMCA_PS4_TITLE_ID + "\npkg=/user" + pkgPath +
                                "\nname=" + GMCA_PS4_APP_NAME + "\nlabel=" + label + "\n";
    int e = ps4::writeFile(requestFile, request);
    lg(log, "request -> %s", e ? std::strerror(e) : "ok");
    if (e) {
        err = "could not write the update request";
        return false;
    }

    if (!ps4::beginEscape()) {
        ps4::endEscape();
        err = "GoldHEN's sandbox escape is not available";
        lg(log, "escape failed");
        return false;
    }
    // before any write under /system, so even a half-written helper gets taken out later
    ps4::writeFile(helperMarker, helperDir + "\n");
    files[0].data = daemonSfo(log);
    bool ok = !files[0].data.empty() && installHelper(files, log);
    int launched = 0;
    if (ok) {
        launched = ps4::launchApp(GMCA_PS4_UPDATER_TITLE_ID);
        lg(log, "launch %s -> 0x%08x", GMCA_PS4_UPDATER_TITLE_ID, (unsigned)launched);
        ok = launched >= 0;
    }
    ps4::endEscape();
    if (!ok) {
        // a helper still running keeps its own copy of the files we just replaced
        err = launched == int(0x8094000C) ? "an update is already being installed"
              : launched < 0              ? "the update helper did not start"
                                          : "could not install the update helper";
        unlink(requestFile);
        return false;
    }

    // The helper says "waiting" once it has read the request; then it waits for this app to close.
    for (int i = 0; i < 40; ++i) {
        const std::string status = lastStatus();
        if (!status.empty()) {
            lg(log, "helper status after %.1f s: %s", i * 0.5, status.c_str());
            if (status.rfind("failed", 0) == 0) {
                err = "the update helper stopped: " + status.substr(status.size() > 7 ? 7 : status.size());
                return false;
            }
            return true;
        }
        usleep(500 * 1000);
    }
    err = "the update helper did not answer";
    lg(log, "no helper status after 20 s (its log: %s)", helperLog);
    return false;
}

std::string lastStatus() {
    std::string s;
    if (!ps4::readFile(statusFile, s)) return "";
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

void clearStatus() { unlink(statusFile); }

void cleanUp(std::FILE* log) {
    struct stat st;
    for (const char* f : {helperLog, appLog})
        if (stat(f, &st) == 0 && st.st_size > 64 * 1024) unlink(f);

    if (stat(helperMarker, &st) != 0) return;  // no helper went under /system
    const std::string status = lastStatus();
    if (status == "waiting" || status == "installing") return;  // it may still be at work

    if (!ps4::beginEscape()) {
        ps4::endEscape();
        lg(log, "cleanup: escape failed, the helper stays for now");
        return;
    }
    int rc = ps4::remountSystem(true);
    lg(log, "cleanup: /system read-write -> %d", rc);
    if (rc == 0) {
        for (const char* f : {"/eboot.bin", "/sce_module/libc.prx", "/sce_module/libSceFios2.prx", "/sce_sys/param.sfo"})
            if (unlink((helperDir + f).c_str()) != 0 && errno != ENOENT)
                lg(log, "cleanup: unlink %s%s: %s", helperDir.c_str(), f, std::strerror(errno));
        for (const char* d : {"/sce_module", "/sce_sys", ""})
            if (rmdir((helperDir + d).c_str()) != 0 && errno != ENOENT)
                lg(log, "cleanup: rmdir %s%s: %s", helperDir.c_str(), d, std::strerror(errno));
    }
    const bool gone = stat(helperDir.c_str(), &st) != 0;
    rc = ps4::remountSystem(false);
    ps4::endEscape();
    lg(log, "cleanup: /system read-only -> %d; helper %s", rc, gone ? "removed" : "still there, retried next launch");
    if (gone) unlink(helperMarker);
}

}  // namespace ps4::update
#endif
