// PS4 only. GMCA's self-update. The system installer won't replace the app that asks for it, and
// its library won't load in an app process at all, so the install runs in a small helper shipped
// in GMCA's package (app/platform/ps4/updater, a system daemon). The app checks the staged
// package, installs the helper under /system/vsh/app and starts it, then closes; the helper waits
// for GMCA to be gone, installs the package over it (keeping /data), and says how it went in a
// notification and in status.txt. GMCA's next launch takes out the helper's files (cleanUp). A
// failed install leaves GMCA as it was and keeps the package.
#pragma once
#ifdef __PS4__
#include <cstdio>
#include <string>

#include "utils/ps4_update_status.hpp"

namespace ps4::update {

/// The folder the app and the helper share. BGFT reads it as /user/data/GMCA/update.
constexpr const char* dir = "/data/GMCA/update";
/// Where the app stages the package it hands over.
constexpr const char* stagedPkg = "/data/GMCA/update/update.pkg";
/// app -> helper: key=value lines (title, pkg, name, label).
constexpr const char* requestFile = "/data/GMCA/update/request.txt";
/// helper -> app, one line: "<state> <time> <detail>" (utils/ps4_update_status.hpp).
constexpr const char* statusFile = "/data/GMCA/update/status.txt";
/// The helper's own log, appended across runs.
constexpr const char* helperLog = "/data/GMCA/update/updater.log";
/// The app's log of its update steps, appended across updates.
constexpr const char* appLog = "/data/GMCA/update/app.log";
/// Written before the helper goes under /system, removed once cleanUp() has taken it out.
constexpr const char* helperMarker = "/data/GMCA/update/helper.txt";

/// Hands the package at `pkgPath` (under /data, normally stagedPkg) to the update helper: checks it
/// is for this app, installs the helper and starts it. `label` is the version the user sees
/// ("v1.4.4"). Call it on the main thread: it holds the escape for about a second, during which
/// the UI must not read the app's files or load a module, so the UI stands still for it. True once
/// the helper is started, then call waitForHelper(). False with `err` set otherwise; the installed
/// GMCA is untouched. When `log` is non-null every step is written to it.
bool handOver(const std::string& pkgPath, const std::string& label, std::string& err, std::FILE* log = nullptr);

/// Waits, up to 20 s, for the helper handOver() started to read the request. True then: the caller
/// must close the app, which the helper waits for. Any thread.
bool waitForHelper(std::string& err, std::FILE* log = nullptr);

/// The helper's last status, state "" when there is none.
Status lastStatus();
/// What the last status means now (cf. outcomeOf), `why` set for a failure. `appVersion` is the
/// running version, AppVersion::getVersion().
Outcome lastOutcome(const std::string& appVersion, std::string& why);
/// Forgets the last status (once the app has shown it).
void clearStatus();

/// Takes out what an earlier update left: the helper's files under /system and a log past 64 KB.
/// The console refuses most deletes there, so a file that won't go is emptied instead. One try
/// per update. Does nothing while the helper may still be at work, or when no helper was
/// installed. It holds the escape, so call it first thing in main(), while the app has no other
/// thread that could read its files or load a module meanwhile.
void cleanUp(std::FILE* log = nullptr);

}  // namespace ps4::update
#endif
