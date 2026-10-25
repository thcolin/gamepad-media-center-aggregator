// PS4 only. Steps out of the app sandbox for a short scope through GoldHEN's SDK call (syscall 500,
// commands 2 and 3, from GoldHEN_Plugins_SDK, MIT), then restores the saved credentials.
#pragma once
#ifdef __PS4__
#include <string>

namespace ps4 {

/// Persistent escape, reference counted: nested begins are safe, only the outermost end restores.
/// Returns true if the escape is active after the call. endEscape without a begin is a no-op.
bool beginEscape();
void endEscape();

/// RAII: escapes in the constructor, drops back in the destructor. ok() tells if the escape worked.
class SandboxEscape {
public:
    /// Escapes only when `path` is under /mnt/usb (a no-op guard otherwise).
    explicit SandboxEscape(const std::string& path);
    ~SandboxEscape();
    SandboxEscape(const SandboxEscape&) = delete;
    SandboxEscape& operator=(const SandboxEscape&) = delete;
    bool ok() const { return escaped; }

private:
    bool escaped = false;
    bool counted = false;
};

}  // namespace ps4
#endif
