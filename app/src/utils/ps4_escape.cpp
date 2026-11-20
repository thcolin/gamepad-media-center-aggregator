#ifdef __PS4__
#include "utils/ps4_escape.hpp"

#include <cstdint>
#include <cstring>
#include <mutex>

// Raw syscall stub (the same one the GoldHEN SDK ships): rax = 0 slot, args shifted, errno on carry
extern "C" int gmca_orbis_syscall(int num, ...);
__asm__(
    ".att_syntax prefix\n"
    ".globl gmca_orbis_syscall\n"
    "gmca_orbis_syscall:\n"
    "  movq $0, %rax\n"
    "  movq %rcx, %r10\n"
    "  syscall\n"
    "  jb gmca_orbis_syscall_err\n"
    "  retq\n"
    "gmca_orbis_syscall_err:\n"
    "  pushq %rax\n"
    "  callq __error\n"
    "  popq %rcx\n"
    "  movl %ecx, 0(%rax)\n"
    "  movq $0xFFFFFFFFFFFFFFFF, %rax\n"
    "  movq $0xFFFFFFFFFFFFFFFF, %rdx\n"
    "  retq\n");

namespace {
// GoldHEN SDK struct jailbreak_backup (sizeof 72); the shared buffer below is larger
struct JailbreakBackup {
    uint32_t cr_uid, cr_ruid, cr_rgid, cr_groups;
    uint64_t cr_paid;
    uint64_t cr_caps[2];
    void* cr_prison;
    void* fd_cdir;
    void* fd_jdir;
    void* fd_rdir;
};
static_assert(sizeof(JailbreakBackup) <= 128, "backup buffer too small");
const uint64_t CMD_JAILBREAK = 2;
const uint64_t CMD_UNJAILBREAK = 3;
}  // namespace

namespace {
std::mutex escapeMutex;
int escapeDepth = 0;           // outstanding begins that count toward the held escape
bool escapeActive = false;     // jailbreak syscall succeeded and is not yet restored
alignas(8) unsigned char escapeBackup[128];
}  // namespace

namespace ps4 {

bool beginEscape() {
    std::lock_guard<std::mutex> lock(escapeMutex);
    if (escapeDepth++ == 0) {
        std::memset(escapeBackup, 0, sizeof(escapeBackup));
        escapeActive = gmca_orbis_syscall(500, CMD_JAILBREAK, escapeBackup) == 0;
    }
    return escapeActive;
}

void endEscape() {
    std::lock_guard<std::mutex> lock(escapeMutex);
    if (escapeDepth == 0) return;
    if (--escapeDepth == 0 && escapeActive) {
        gmca_orbis_syscall(500, CMD_UNJAILBREAK, escapeBackup);
        escapeActive = false;
    }
}

SandboxEscape::SandboxEscape(const std::string& path) {
    if (path.rfind("/mnt/usb", 0) != 0) return;
    counted = true;
    escaped = beginEscape();
}

SandboxEscape::~SandboxEscape() {
    // balance the begin even if the syscall failed (depth was still counted)
    if (counted) endEscape();
}

}  // namespace ps4
#endif
