// PS4 only. GMCA's self-update. The system installer won't replace the app that asks for it, and
// its library won't load in an app process at all, so the install runs in a small helper shipped
// in GMCA's package (app/platform/ps4/updater, a system daemon). The app checks the staged
// package, installs the helper under /system/vsh/app and starts it, then closes; the helper waits
// for GMCA to be gone, installs the package over it (keeping /data), says how it went in a
// notification and in status.txt, and removes itself. A failed install leaves GMCA as it was and
// keeps the package.
#pragma once
#ifdef __PS4__
#include <cstdio>
#include <string>

namespace ps4::update {

/// The folder the app and the helper share. BGFT reads it as /user/data/GMCA/update.
constexpr const char* dir = "/data/GMCA/update";
/// Where the app stages the package it hands over.
constexpr const char* stagedPkg = "/data/GMCA/update/update.pkg";
/// app -> helper: key=value lines (title, pkg, name, label).
constexpr const char* requestFile = "/data/GMCA/update/request.txt";
/// helper -> app, one line: "waiting", "installing", "done <label>" or "failed <why>".
constexpr const char* statusFile = "/data/GMCA/update/status.txt";
/// The helper's own log, appended across runs.
constexpr const char* helperLog = "/data/GMCA/update/updater.log";
/// The app's log of its update steps, appended across updates.
constexpr const char* appLog = "/data/GMCA/update/app.log";
/// Written before the helper goes under /system, removed once cleanUp() has taken it out.
constexpr const char* helperMarker = "/data/GMCA/update/helper.txt";

/// Starts the update to the package at `pkgPath` (under /data, normally stagedPkg). `label` is
/// the version the user sees ("1.4.4"). True once the helper is running and waiting: the caller
/// must then close the app. False with `err` set otherwise; the installed GMCA is untouched.
/// When `log` is non-null every step is written to it.
bool start(const std::string& pkgPath, const std::string& label, std::string& err, std::FILE* log = nullptr);

/// The helper's last status line, "" when there is none.
std::string lastStatus();
/// Forgets the last status (once the app has shown it).
void clearStatus();

/// Takes out what an earlier update left: the helper's folder under /system (the helper's own
/// unlink there fails with EROFS, the app's writes there work) and a log past 64 KB. Does nothing
/// while an update may still be running ("waiting" or "installing") or when no helper was
/// installed. Runs inside the escape, so call it off the main thread, once startup is over.
void cleanUp(std::FILE* log = nullptr);

}  // namespace ps4::update
#endif
