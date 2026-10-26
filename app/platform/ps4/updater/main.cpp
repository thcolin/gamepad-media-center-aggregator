// GMCA's update helper on the PS4: a system daemon the app installs under
// /system/vsh/app/<updater title> and starts when the user updates (app/src/utils/ps4_self_update.cpp,
// which describes the whole flow). It reads the request, says "waiting" in status.txt, waits for
// GMCA to close, then installs the package over it the way Gold Torrent's update test proved on a
// console: sceAppInstUtilAppPrepareOverwritePkg, then BGFT's register by storage with the HB Store's
// fields, then start, following the task until the system has finished. It never removes GMCA: a
// refused or failed install leaves it as it was and keeps the package. Then it notifies, writes
// "done <label>" or "failed <why>", takes its own files out of /system and exits.
//
// Signed as a system daemon (CMakeLists.txt), which is what lets it load the installer's library.
// Log: /data/GMCA/update/updater.log
#include "utils/ps4_escape.hpp"
#include "utils/ps4_self_update.hpp"
#include "utils/ps4_system.hpp"

#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include <orbis/Sysmodule.h>

#ifndef GMCA_PS4_UPDATER_TITLE_ID
#error "GMCA_PS4_UPDATER_TITLE_ID is not defined"
#endif

// Layouts from LightningMods' PS4-Store (Store/include/installpkg.h), as Gold Torrent's test used:
// OpenOrbis' Bgft.h has 32-bit sizes where the system's are 64-bit.
extern "C" {
struct bgft_init_params {
    void* heap;
    size_t heap_size;
};
struct bgft_download_param {
    int user_id;
    int entitlement_type;
    const char* id;
    const char* content_url;
    const char* content_ex_url;
    const char* content_name;
    const char* icon_path;
    const char* sku_id;
    int option;
    const char* playgo_scenario_id;
    const char* release_date;
    const char* package_type;
    const char* package_sub_type;
    unsigned long package_size;
};
struct bgft_download_param_ex {
    bgft_download_param param;
    unsigned int slot;
};
struct bgft_task_progress {
    unsigned int bits;
    int error_result;
    unsigned long length;
    unsigned long transferred;
    unsigned long length_total;
    unsigned long transferred_total;
    unsigned int num_index;
    unsigned int num_total;
    unsigned int rest_sec;
    unsigned int rest_sec_total;
    int preparing_percent;
    int local_copy_percent;
    char spare[64];  // room, in case the system's is longer
};
int sceBgftServiceInit(bgft_init_params* params);
int sceBgftServiceIntDownloadRegisterTaskByStorageEx(bgft_download_param_ex* params, int* task);
int sceBgftServiceDownloadStartTask(int task);
int sceBgftServiceDownloadGetProgress(int task, bgft_task_progress* progress);
int sceAppInstUtilInitialize(void);
int sceAppInstUtilGetTitleIdFromPkg(const char* pkg, char* title, int* is_app);
int sceAppInstUtilAppPrepareOverwritePkg(const char* pkg);
bool sceAppInstUtilAppIsInInstalling(const char* content_id);
}

namespace {

namespace upd = ps4::update;

constexpr int bgftOptInvisible = 0x2;
constexpr int stepMs = 500;

std::FILE* logFile = nullptr;

void lg(const char* fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (!logFile) return;
    const std::time_t now = std::time(nullptr);
    std::tm t{};
    gmtime_r(&now, &t);
    std::fprintf(logFile, "%02d:%02d:%02dZ %s\n", t.tm_hour, t.tm_min, t.tm_sec, msg);
    std::fflush(logFile);
    fsync(fileno(logFile));  // what happened right before the process ended is the point
}

std::string hex(int r) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%08x", (unsigned)r);
    return b;
}

void setStatus(const std::string& line) {
    lg("status: %s", line.c_str());
    ps4::writeFile(upd::statusFile, line + "\n");
}

struct Request {
    std::string title, pkg, name, label;
};

bool readRequest(Request& r) {
    std::string text;
    if (!ps4::readFile(upd::requestFile, text)) return false;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(at, end - at);
        at = end + 1;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == "title") r.title = value;
        else if (key == "pkg") r.pkg = value;
        else if (key == "name") r.name = value;
        else if (key == "label") r.label = value;
    }
    if (r.name.empty()) r.name = "GMCA";
    return !r.title.empty() && !r.pkg.empty();
}

// The same package as the app sees it, for the header read and the delete.
std::string appPath(const std::string& systemPath) {
    return systemPath.rfind("/user/data/", 0) == 0 ? systemPath.substr(5) : systemPath;
}

long fileSize(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 ? (long)st.st_size : -1;
}

// After the start: 0 once the install has finished, the task's error, or -1 on a timeout
// (nothing happening for 2 minutes, or 30 in all), with `why` set.
int waitInstall(int task, const std::string& contentId, const std::string& title, const std::string& oldVersion,
                std::string& why) {
    const int quietSteps = 2 * 60 * 1000 / stepMs, maxSteps = 30 * 60 * 1000 / stepMs;
    int lastSign = 0;
    unsigned long lastTransferred = 0;
    bool sawInstalling = false;
    bool allIn = false;  // the task has had every byte in once
    int settled = 0;     // checks in a row that look finished
    for (int step = 0; step < maxSteps; ++step) {
        bgft_task_progress p{};
        const int pr = sceBgftServiceDownloadGetProgress(task, &p);
        const bool installing = sceAppInstUtilAppIsInInstalling(contentId.c_str());
        if (p.transferred != lastTransferred || installing) lastSign = step;
        lastTransferred = p.transferred;
        sawInstalling = sawInstalling || installing;
        const std::string version = ps4::installedVersion(title);
        if (step % 4 == 0 || pr != 0 || p.error_result != 0)
            lg("task %d: progress %s, error %s, %lu of %lu bytes, preparing %d%%, copy %d%%, installing %s, version %s",
               task, hex(pr).c_str(), hex(p.error_result).c_str(), p.transferred, p.length, p.preparing_percent,
               p.local_copy_percent, installing ? "yes" : "no", version.empty() ? "-" : version.c_str());
        // A task the system drops once it has every byte can't be read any more: that only
        // fails the install before then.
        if (pr != 0 && !allIn) {
            why = "progress " + hex(pr);
            return pr;
        }
        if (pr == 0 && p.error_result != 0) {
            why = "install error " + hex(p.error_result);
            return p.error_result;
        }
        // Done once every byte is in (or the install ran and the version moved) and the system
        // has stopped installing for 2 s. The version in its records changes when the install
        // starts, so it never counts alone.
        allIn = allIn || (pr == 0 && p.length > 0 && p.transferred >= p.length);
        const bool newVersion = !version.empty() && version != oldVersion;
        settled = !installing && (allIn || (sawInstalling && newVersion)) ? settled + 1 : 0;
        if (settled >= 4) {
            if (!newVersion) lg("note: the installed version is still %s", version.c_str());
            return 0;
        }
        if (step - lastSign > quietSteps) {
            why = "nothing happened for 2 minutes";
            return -1;
        }
        usleep(stepMs * 1000);
    }
    why = "still not done after 30 minutes";
    return -1;
}

// Prepare, register and start, then follow the task. "" when it worked, else why not.
std::string install(const Request& r) {
    const std::string contentId = ps4::pkgContentId(appPath(r.pkg));
    lg("package %s (%ld bytes), content id '%s'", r.pkg.c_str(), fileSize(appPath(r.pkg)), contentId.c_str());
    if (contentId.empty()) return "the package is missing";

    lg("load AppInstUtil -> %s", hex(sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_APP_INST_UTIL)).c_str());
    lg("load Bgft -> %s", hex(sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_BGFT)).c_str());
    int rc = sceAppInstUtilInitialize();
    lg("sceAppInstUtilInitialize -> %s", hex(rc).c_str());
    if (rc != 0) return "installer init " + hex(rc);
    static bgft_init_params init{std::calloc(1, 1 << 20), 1 << 20};
    rc = sceBgftServiceInit(&init);
    lg("sceBgftServiceInit -> %s", hex(rc).c_str());
    if (rc != 0) return "BGFT init " + hex(rc);

    char title[16] = {};
    int isApp = 0;
    rc = sceAppInstUtilGetTitleIdFromPkg(r.pkg.c_str(), title, &isApp);
    lg("sceAppInstUtilGetTitleIdFromPkg -> %s, '%s', app %d", hex(rc).c_str(), title, isApp);
    if (rc == 0 && r.title != title) return std::string("the package is for ") + title;

    const std::string oldVersion = ps4::installedVersion(r.title);
    lg("installed version before: %s", oldVersion.empty() ? "-" : oldVersion.c_str());

    rc = sceAppInstUtilAppPrepareOverwritePkg(r.pkg.c_str());
    lg("sceAppInstUtilAppPrepareOverwritePkg -> %s", hex(rc).c_str());

    bgft_download_param_ex p{};
    p.param.entitlement_type = 5;
    p.param.id = "";
    p.param.content_url = r.pkg.c_str();
    p.param.content_name = r.name.c_str();
    p.param.icon_path = "";
    p.param.playgo_scenario_id = "0";
    p.param.option = bgftOptInvisible;
    p.slot = 0;
    int task = -1;
    rc = sceBgftServiceIntDownloadRegisterTaskByStorageEx(&p, &task);
    lg("register by storage -> %s, task %d", hex(rc).c_str(), task);
    if (rc != 0) return "the installer refused it (" + hex(rc) + ")";
    rc = sceBgftServiceDownloadStartTask(task);
    lg("start task %d -> %s", task, hex(rc).c_str());
    if (rc != 0) return "the install did not start (" + hex(rc) + ")";

    setStatus("installing");
    std::string why;
    rc = waitInstall(task, contentId, r.title, oldVersion, why);
    lg("install finished: %s; version now %s", rc == 0 ? "ok" : why.c_str(), ps4::installedVersion(r.title).c_str());
    return rc == 0 ? "" : why;
}

// Out of /system: the files, the folders, read-only again. A failure here is harmless: the next
// update writes over what is left.
void removeSelf() {
    const std::string dir = std::string("/system/vsh/app/") + GMCA_PS4_UPDATER_TITLE_ID;
    lg("/system read-write -> %d", ps4::remountSystem(true));
    for (const char* f : {"/eboot.bin", "/sce_module/libc.prx", "/sce_module/libSceFios2.prx", "/sce_sys/param.sfo"})
        lg("unlink %s%s -> %s", dir.c_str(), f, unlink((dir + f).c_str()) == 0 ? "ok" : std::strerror(errno));
    for (const char* d : {"/sce_module", "/sce_sys", ""})
        lg("rmdir %s%s -> %s", dir.c_str(), d, rmdir((dir + d).c_str()) == 0 ? "ok" : std::strerror(errno));
    lg("/system read-only -> %d", ps4::remountSystem(false));
}

[[noreturn]] void leave() {
    removeSelf();
    lg("helper done");
    if (logFile) std::fclose(logFile);
    _exit(0);
}

}  // namespace

int main() {
    // The order Gold Torrent's service proved: system libraries, SystemService by dlsym, then the
    // escape (held until the end: the /system writes and the installed version need it).
    const unsigned sysService = sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_SYSTEM_SERVICE);
    const unsigned userService = sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_USER_SERVICE);
    const int resolved = ps4::resolveSystemService();
    const bool escaped = ps4::beginEscape();

    mkdir("/data/GMCA", 0777);
    mkdir(upd::dir, 0777);
    logFile = std::fopen(upd::helperLog, "a");
    lg("==== GMCA update helper %s (pid %d) ====", GMCA_PS4_UPDATER_TITLE_ID, (int)getpid());
    lg("modules SystemService 0x%x, UserService 0x%x; %d of 4 SystemService functions; escape %s", sysService,
       userService, resolved, escaped ? "ok" : "FAILED");
    if (resolved < 4) lg("modules: %s", ps4::loadedModules().c_str());
    lg("sceSystemServiceRegisterDaemon -> %s", hex(ps4::registerDaemon()).c_str());

    Request r;
    if (!readRequest(r)) {
        setStatus("failed no update request");
        leave();
    }
    lg("request: title %s, pkg %s, name %s, label %s", r.title.c_str(), r.pkg.c_str(), r.name.c_str(), r.label.c_str());
    unlink(upd::requestFile);  // one install per request

    // GMCA closes once it has seen "waiting". It is the foreground app until then (it just started
    // this helper, and it waits for "waiting"), so it is gone when the foreground app id changes.
    // When that id can't be read, give it a fixed while: installing over a title that still runs
    // is refused.
    const int gmcaId = ps4::foregroundAppId();
    lg("foreground app id 0x%08x", (unsigned)gmcaId);
    setStatus("waiting");
    if (gmcaId < 0) {
        lg("can't tell when %s closes: waiting 7 s", r.title.c_str());
        sleep(7);  // GMCA usually closes within 5 s; a refused install leaves it as it was
    } else {
        int waited = 0;
        while (ps4::foregroundAppId() == gmcaId && waited < 60 * 1000) {
            usleep(stepMs * 1000);
            waited += stepMs;
        }
        if (ps4::foregroundAppId() == gmcaId) {
            lg("%s still in the foreground after 60 s", r.title.c_str());
            setStatus("failed " + r.name + " did not close");
            ps4::notify(r.name + " didn't update: it didn't close. Nothing changed.");
            leave();
        }
        lg("%s closed after %.1f s (foreground now 0x%08x)", r.title.c_str(), waited / 1000.0,
           (unsigned)ps4::foregroundAppId());
    }
    sleep(2);  // let the system finish closing it

    const std::string why = install(r);
    if (why.empty()) {
        unlink(appPath(r.pkg).c_str());
        setStatus("done " + r.label);
        ps4::notify(r.name + " " + r.label + " is installed. Open it when you like.");
    } else {
        setStatus("failed " + why);
        ps4::notify(r.name + " didn't update (" + why + "). The installed version is unchanged.");
    }
    leave();
}
