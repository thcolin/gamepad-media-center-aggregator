// PS4 only. System calls the app and its update helper (app/platform/ps4/updater) share:
// SystemService functions resolved at run time, a notification, the /system remount, the
// version of an installed title and small file helpers. Ported from Gold Torrent's ps4 layer,
// where console runs proved each of them.
#pragma once
#ifdef __PS4__
#include <string>

namespace ps4 {

/// Resolves the functions below from libSceSystemService.sprx, which must already be loaded (the
/// app links it, the helper loads it first). OpenOrbis declares several of them wrongly, so a
/// missing one shows up as a -1 result instead of a process that won't start. In the app
/// (GMCA_PS4_APP), the linked LaunchApp and LoadExec stand in when the lookup by name fails. How
/// many of the four were found.
int resolveSystemService();

/// The names of the modules this process has loaded, for a log line when a lookup fails.
std::string loadedModules();

/// Each returns the call's result, or -1 when the function was not resolved.
int launchApp(const char* titleId);  // sceSystemServiceLaunchApp; 0x8094000C: already running
int loadExec(const char* path);      // sceSystemServiceLoadExec(path, nullptr); "exit" closes the app
int registerDaemon();                // sceSystemServiceRegisterDaemon

/// The app id of the app in the foreground (sceSystemServiceGetAppIdOfBigApp), or -1 when there is
/// none or the call is missing. sceLncUtilGetAppId, the by-title check, never resolves (Gold
/// Torrent's logs and GMCA's first runs), so the helper tries this one. In the helper it read -1
/// on the console (2026-10-26), and the helper's fixed wait is what closes the gap today.
int foregroundAppId();

/// A notification bubble (sceKernelSendNotificationRequest). The call's result.
int notify(const std::string& text);

/// Remounts /system read-write (true) or read-only again (false), with Itemzflow's options.
/// Only inside the escape (utils/ps4_escape.hpp). 0, or the errno.
int remountSystem(bool writable);

/// The VERSION ("00.34") of an installed title from the system's copy of its param.sfo, or "".
/// Only inside the escape.
std::string installedVersion(const std::string& titleId);

/// The content id in a package's header ("IV0001-GMCA00000_00-GMCA000000003300"), or "" when
/// the file isn't a package.
std::string pkgContentId(const std::string& path);

bool readFile(const std::string& path, std::string& out);
/// Replaces `path` with `data` and syncs it. 0, or the errno.
int writeFile(const std::string& path, const std::string& data);

}  // namespace ps4
#endif
