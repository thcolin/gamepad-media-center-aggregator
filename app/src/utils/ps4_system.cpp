#ifdef __PS4__
#include "utils/ps4_system.hpp"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <sys/uio.h>
#include <unistd.h>
#include <vector>

#include <orbis/libkernel.h>

extern "C" int gmca_orbis_syscall(int num, ...);  // utils/ps4_escape.cpp

#ifdef GMCA_PS4_APP
// The app links libSceSystemService (borealis calls it), so these stand in when the lookup by
// name finds nothing (it found nothing in GMCA's first console run). Declared here because
// OpenOrbis' SystemService.h has LaunchApp as void(void).
extern "C" int sceSystemServiceLaunchApp(const char* titleId, const char** argv, void* param);
extern "C" int sceSystemServiceLoadExec(const char* path, const char** args);
#endif

namespace {

// sceSystemServiceLaunchApp's last argument, as Itemzflow fills it
struct LaunchParam {
    uint32_t size;
    uint32_t userId;
    uint32_t appOpt;
    uint64_t crashReport;
    uint32_t checkFlag;  // 2: skip the system update check
};

struct SystemService {
    int (*registerDaemon)() = nullptr;
    int (*loadExec)(const char* path, const char** args) = nullptr;
    int (*launchApp)(const char* titleId, const char** argv, LaunchParam* param) = nullptr;
    int (*bigAppId)() = nullptr;
};
SystemService sys;

// FreeBSD <sys/mount.h>; musl's headers carry Linux values
constexpr long bsdMntRdonly = 0x00000001;
constexpr long bsdMntUpdate = 0x00010000;
constexpr int sysNmount = 378;

// The handle of a module this process has loaded, by file name; -1 if it isn't.
int moduleHandle(const char* name) {
    OrbisKernelModule mods[256];
    size_t n = 0;
    if (sceKernelGetModuleList(mods, 256, &n) != 0) return -1;
    for (size_t i = 0; i < n && i < 256; ++i) {
        OrbisKernelModuleInfo info;
        std::memset(&info, 0, sizeof info);
        info.size = sizeof info;
        if (sceKernelGetModuleInfo(mods[i], &info) == 0 && std::strcmp(info.name, name) == 0) return int(mods[i]);
    }
    return -1;
}

template <class F>
bool resolve(int handle, const char* name, F& fn) {
    void* p = nullptr;
    fn = sceKernelDlsym(handle, name, &p) == 0 ? reinterpret_cast<F>(p) : nullptr;
    return fn != nullptr;
}

}  // namespace

namespace ps4 {

int resolveSystemService() {
    const int h = moduleHandle("libSceSystemService.sprx");
    if (h >= 0) {
        resolve(h, "sceSystemServiceRegisterDaemon", sys.registerDaemon);
        resolve(h, "sceSystemServiceLoadExec", sys.loadExec);
        resolve(h, "sceSystemServiceLaunchApp", sys.launchApp);
        resolve(h, "sceSystemServiceGetAppIdOfBigApp", sys.bigAppId);
    }
#ifdef GMCA_PS4_APP
    if (!sys.loadExec) sys.loadExec = &sceSystemServiceLoadExec;
    if (!sys.launchApp)
        sys.launchApp = reinterpret_cast<int (*)(const char*, const char**, LaunchParam*)>(&sceSystemServiceLaunchApp);
#endif
    return int(sys.registerDaemon != nullptr) + int(sys.loadExec != nullptr) + int(sys.launchApp != nullptr) +
           int(sys.bigAppId != nullptr);
}

std::string loadedModules() {
    OrbisKernelModule mods[256];
    size_t n = 0;
    const int rc = sceKernelGetModuleList(mods, 256, &n);
    std::string out = "list rc " + std::to_string(rc) + ", " + std::to_string(n) + " modules:";
    for (size_t i = 0; i < n && i < 256; ++i) {
        OrbisKernelModuleInfo info;
        std::memset(&info, 0, sizeof info);
        info.size = sizeof info;
        const int r = sceKernelGetModuleInfo(mods[i], &info);
        out += r == 0 ? std::string(" ") + info.name : " (info rc " + std::to_string(r) + ")";
    }
    return out;
}


int launchApp(const char* titleId) {
    if (!sys.launchApp) return -1;
    LaunchParam p{};
    p.size = sizeof p;
    p.userId = 0xFFFFFFFFu;
    p.checkFlag = 2;
    return sys.launchApp(titleId, nullptr, &p);
}

int loadExec(const char* path) { return sys.loadExec ? sys.loadExec(path, nullptr) : -1; }

int registerDaemon() { return sys.registerDaemon ? sys.registerDaemon() : -1; }

int foregroundAppId() {
    if (!sys.bigAppId) return -1;
    const int id = sys.bigAppId();
    // an app's id is 0x60xxxxxx; anything else means none (or an error)
    return (unsigned(id) & ~0xFFFFFFu) == 0x60000000u ? id : -1;
}

int notify(const std::string& text) {
    OrbisNotificationRequest req;
    std::memset(&req, 0, sizeof req);
    req.type = NotificationRequest;
    req.targetId = -1;
    std::strncpy(req.message, text.c_str(), sizeof req.message - 1);
    return sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
}

int remountSystem(bool writable) {
    std::vector<iovec> iov;
    auto add = [&](const char* name, const char* value) {
        iov.push_back({const_cast<char*>(name), std::strlen(name) + 1});
        iov.push_back({const_cast<char*>(value), std::strlen(value) + 1});
    };
    add("fstype", "exfatfs");
    add("fspath", "/system");
    add("from", "/dev/da0x4.crypt");
    add("large", "yes");
    add("timezone", "static");
    add("async", "");
    add("ignoreacl", "");
    add("dirmask", "511");
    add("mask", "511");
    const long flags = writable ? bsdMntUpdate : bsdMntUpdate | bsdMntRdonly;
    return gmca_orbis_syscall(sysNmount, iov.data(), (unsigned)iov.size(), flags) == 0 ? 0 : errno;
}

// param.sfo: magic, version, key table offset (8), data table offset (12), entry count (16), then
// 16-byte entries from 20: key offset (u16), format (u16), length, max length, data offset.
std::string installedVersion(const std::string& titleId) {
    std::string sfo;
    if (!readFile("/system_data/priv/appmeta/" + titleId + "/param.sfo", sfo) || sfo.size() < 20) return "";
    auto u32 = [&](size_t at) {
        uint32_t v = 0;
        if (at + 4 <= sfo.size()) std::memcpy(&v, sfo.data() + at, 4);
        return v;
    };
    auto u16 = [&](size_t at) {
        uint16_t v = 0;
        if (at + 2 <= sfo.size()) std::memcpy(&v, sfo.data() + at, 2);
        return v;
    };
    const uint32_t keys = u32(8), data = u32(12), count = u32(16);
    for (uint32_t i = 0; i < count && 20 + size_t(i) * 16 + 16 <= sfo.size(); ++i) {
        const size_t e = 20 + size_t(i) * 16;
        const size_t k = keys + u16(e);
        if (k >= sfo.size()) break;
        if (std::strcmp(sfo.c_str() + k, "VERSION") != 0) continue;
        const size_t at = data + u32(e + 12), len = u32(e + 4);
        if (at + len > sfo.size()) break;
        return std::string(sfo.data() + at, strnlen(sfo.data() + at, len));
    }
    return "";
}

// The content id sits at 0x40 of the header, 36 characters, after the magic 7F 'CNT'.
std::string pkgContentId(const std::string& path) {
    const int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return "";
    char buf[0x80] = {};
    const ssize_t n = read(fd, buf, sizeof buf);
    close(fd);
    if (n < 0x64 || std::memcmp(buf, "\x7F" "CNT", 4) != 0) return "";
    return std::string(buf + 0x40, strnlen(buf + 0x40, 36));
}

bool readFile(const std::string& path, std::string& out) {
    out.clear();
    const int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    char buf[64 * 1024];
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0) out.append(buf, size_t(n));
    const bool ok = n == 0;
    close(fd);
    return ok;
}

int writeFile(const std::string& path, const std::string& data) {
    unlink(path.c_str());
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0777);
    if (fd < 0) return errno;
    size_t done = 0;
    while (done < data.size()) {
        const ssize_t n = write(fd, data.data() + done, data.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            const int e = errno ? errno : EIO;
            close(fd);
            return e;
        }
        done += size_t(n);
    }
    fsync(fd);
    close(fd);
    return 0;
}

}  // namespace ps4
#endif
