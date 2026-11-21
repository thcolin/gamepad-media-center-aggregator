#include "api/http.hpp"
#include "utils/config.hpp"
#include "utils/url_origin.hpp"
#include "utils/atomic_file.hpp"
#ifdef USE_LIBROMFS
#include <romfs/romfs.hpp>
#endif
#include <borealis/core/logger.hpp>
#include <curl/curl.h>
#include <mutex>
#if defined(BOREALIS_USE_GXM)
#include <mbedtls/platform.h>
#include <psp2/gxm.h>
#include <psp2/kernel/sysmem.h>
static SceUID mempool_id = 0;
static void* mempool_addr = nullptr;
static size_t mempool_size = 20 * 1024 * 1024;
static void* s_mspace = nullptr;
int __attribute__((optimize("no-optimize-sibling-calls"))) malloc_finalize() {
    if (s_mspace) sceClibMspaceDestroy(s_mspace);
    if (mempool_addr) sceGxmUnmapMemory(mempool_addr);
    if (mempool_id) sceKernelFreeMemBlock(mempool_id);
    return 0;
}

int malloc_init() {
    int res;
    if (s_mspace) return 0;
    mempool_id = sceKernelAllocMemBlock("curl", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_RW, mempool_size, nullptr);
    sceKernelGetMemBlockBase(mempool_id, &mempool_addr);
    if (!mempool_addr) goto error;
    res = sceGxmMapMemory(mempool_addr, mempool_size, SCE_GXM_MEMORY_ATTRIB_RW);
    if (res != SCE_OK) goto error;
    s_mspace = sceClibMspaceCreate(mempool_addr, mempool_size);
    if (!s_mspace) goto error;

    return 0;
error:
    malloc_finalize();
    return 1;
}

void __attribute__((optimize("no-optimize-sibling-calls"))) * sce_malloc(size_t size) {
    if (!s_mspace) malloc_init();
    return sceClibMspaceMalloc(s_mspace, size);
}

void __attribute__((optimize("no-optimize-sibling-calls"))) sce_free(void* ptr) {
    if (!ptr || !s_mspace) return;
    sceClibMspaceFree(s_mspace, ptr);
}

void __attribute__((optimize("no-optimize-sibling-calls"))) * sce_calloc(size_t nelem, size_t size) {
    if (!s_mspace) malloc_init();
    return sceClibMspaceCalloc(s_mspace, nelem, size);
}

void __attribute__((optimize("no-optimize-sibling-calls"))) * sce_realloc(void* ptr, size_t size) {
    if (!s_mspace) malloc_init();
    return sceClibMspaceRealloc(s_mspace, ptr, size);
}

char __attribute__((optimize("no-optimize-sibling-calls"))) * sce_strdup(const char* str) {
    size_t len;
    char* newstr;
    if (!str) return (char*)nullptr;
    len = strlen(str) + 1;
    newstr = (char*)sce_malloc(len);
    if (!newstr) return (char*)nullptr;
    sceClibMemcpy(newstr, str, len);
    return newstr;
}
#endif

#ifndef CURL_PROGRESSFUNC_CONTINUE
#define CURL_PROGRESSFUNC_CONTINUE 0x10000001
#endif

static HTTP::Error curl_error(CURLcode code) {
    auto kind = code == CURLE_COULDNT_RESOLVE_HOST        ? HTTP::ErrorKind::ResolveFailed
                : code == CURLE_PEER_FAILED_VERIFICATION ? HTTP::ErrorKind::CertificateUntrusted
                                                         : HTTP::ErrorKind::Other;
    return HTTP::Error(kind, curl_easy_strerror(code));
}
static HTTP::Error curl_error(const std::string& arg) { return HTTP::Error(HTTP::ErrorKind::Other, arg); }

static std::string user_agent =
    fmt::format("{}/{} ({})", AppVersion::getPackageName(), AppVersion::getVersion(), AppVersion::getPlatform());

/// @brief Verrous du cache partagé libcurl (CURLSH), indexés par curl_lock_data.
/// libcurl exige des callbacks lock/unlock dès qu'un objet partagé (ici le cache DNS)
/// est utilisé par des easy handles répartis sur plusieurs threads. Sans eux, les accès
/// concurrents corrompent la hash table du cache DNS (SIGABRT dans Curl_hash_delete /
/// Curl_resolv). Un mutex exclusif par type de donnée suffit : la section critique se
/// limite à la lecture/écriture du cache, les requêtes restent parallèles. Le garde
/// `data < CURL_LOCK_DATA_LAST` protège contre un index hors bornes.
static std::mutex share_locks[CURL_LOCK_DATA_LAST];

static void curl_share_lock_cb(CURL* /*handle*/, curl_lock_data data, curl_lock_access /*access*/,
                               void* /*userptr*/) {
    if (data < CURL_LOCK_DATA_LAST) share_locks[data].lock();
}

static void curl_share_unlock_cb(CURL* /*handle*/, curl_lock_data data, void* /*userptr*/) {
    if (data < CURL_LOCK_DATA_LAST) share_locks[data].unlock();
}

/// @brief curl context

HTTP::HTTP() : chunk(nullptr) {
    static struct Global {
        Global() {
#ifdef BOREALIS_USE_GXM
            mbedtls_platform_set_calloc_free(sce_calloc, sce_free);
            curl_global_init_mem(CURL_GLOBAL_DEFAULT, sce_malloc, sce_free, sce_realloc, sce_strdup, sce_calloc);
#else
            CURLcode rc = curl_global_init(CURL_GLOBAL_ALL);
            brls::Logger::debug("curl global init {}", std::to_string(rc));
#endif
            this->share = curl_share_init();
            // Callbacks de verrouillage obligatoires pour un partage inter-threads sûr
            curl_share_setopt(share, CURLSHOPT_LOCKFUNC, curl_share_lock_cb);
            curl_share_setopt(share, CURLSHOPT_UNLOCKFUNC, curl_share_unlock_cb);
            curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
        }
        ~Global() {
            curl_share_cleanup(this->share);
            curl_global_cleanup();
            brls::Logger::debug("curl cleanup");
        }
        // Avoids initalization order problems
        std::mutex init_lock;
        CURLSH* share;
    } global;

    global.init_lock.lock();
    this->easy = curl_easy_init();
    global.init_lock.unlock();

    curl_easy_setopt(this->easy, CURLOPT_USERAGENT, user_agent.c_str());
    curl_easy_setopt(this->easy, CURLOPT_SHARE, global.share);
    // enable all supported built-in compressions
    curl_easy_setopt(this->easy, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(this->easy, CURLOPT_VERBOSE, 0L);
#if defined(_WIN32)
    // SChannel verifies against the Windows store; an unreachable revocation server must not fail the request
    curl_easy_setopt(this->easy, CURLOPT_SSL_OPTIONS, CURLSSLOPT_REVOKE_BEST_EFFORT);
#elif !defined(__SWITCH2__)
    // Switch 2 (libnx) curl verifies against the console's store
    curl_easy_setopt(this->easy, CURLOPT_CAINFO, ca_file().c_str());
#endif
    // Every request runs from a background thread (ThreadPool / brls::async), so
    // libcurl must never reach for SIGALRM to time out a name resolve — signals
    // only work on the main thread and are unsafe multi-threaded (curl docs:
    // "libcurl cannot function properly multi-threaded unless CURLOPT_NOSIGNAL
    // is set"). With NOSIGNAL, DNS timeouts rely solely on the async (threaded)
    // resolver, which is why the Vita curl build must enable it.
    curl_easy_setopt(this->easy, CURLOPT_NOSIGNAL, 1L);
#if LIBCURL_VERSION_NUM >= 0x071900 && !defined(__PS4__)
    curl_easy_setopt(this->easy, CURLOPT_TCP_KEEPALIVE, 1L);
#endif
}

HTTP::~HTTP() {
    if (this->chunk != nullptr) curl_slist_free_all(this->chunk);
    if (this->connect_to != nullptr) curl_slist_free_all(this->connect_to);
    if (this->easy != nullptr) curl_easy_cleanup(this->easy);
}

const std::string& HTTP::default_user_agent() { return user_agent; }

const std::string& HTTP::ca_file() {
    static const std::string path = [] {
#ifdef USE_LIBROMFS
        // the resources are embedded in the binary: write the bundle out once
        std::string out = AppConfig::instance().configDir() + "/cacert.pem";
        try {
            auto& res = romfs::get("cacert.pem");
            std::string bundle(reinterpret_cast<const char*>(res.data()), res.size()), current;
            if (!AtomicFile::read(out, current) || current != bundle) {
                fs::create_directories(AtomicFile::toPath(AppConfig::instance().configDir()));
                AtomicFile::write(out, bundle);
            }
        } catch (const std::exception& ex) {
            brls::Logger::error("cannot write the CA bundle to {}: {}", out, ex.what());
        }
        return out;
#else
        return std::string(BRLS_RESOURCES) + "cacert.pem";
#endif
    }();
    return path;
}

static std::mutex insecure_lock;
static std::set<std::string> insecure_origins;

void HTTP::set_insecure_origins(std::set<std::string> origins) {
    std::lock_guard<std::mutex> lock(insecure_lock);
    insecure_origins = std::move(origins);
}

void HTTP::allow_insecure(const std::string& url) {
    std::lock_guard<std::mutex> lock(insecure_lock);
    insecure_origins.insert(misc::urlOrigin(url));
}

bool HTTP::insecure(const std::string& url) {
    std::string origin = misc::urlOrigin(url);
    std::lock_guard<std::mutex> lock(insecure_lock);
    return !origin.empty() && insecure_origins.count(origin) > 0;
}

void HTTP::set_user_agent(const std::string& agent) { curl_easy_setopt(this->easy, CURLOPT_USERAGENT, agent.c_str()); }

void HTTP::add_header(const std::string& header) { this->chunk = curl_slist_append(this->chunk, header.c_str()); }

void HTTP::set_option(const Header& hs) {
    curl_slist* chunk = nullptr;
    for (auto& h : hs) {
        chunk = curl_slist_append(chunk, h.c_str());
    }
    curl_slist_free_all(this->chunk);
    this->chunk = chunk;
    if (chunk != nullptr) curl_easy_setopt(this->easy, CURLOPT_HTTPHEADER, chunk);
}

void HTTP::set_option(const Range& r) {
    const std::string range_str = std::to_string(r.start) + "-" + std::to_string(r.end);
    curl_easy_setopt(this->easy, CURLOPT_RANGE, range_str.c_str());
}

void HTTP::set_option(const Timeout& t) {
    curl_easy_setopt(this->easy, CURLOPT_TIMEOUT_MS, t.timeout);
    curl_easy_setopt(this->easy, CURLOPT_CONNECTTIMEOUT_MS, t.connect > 0 ? t.connect : t.timeout);
}

int HTTP::easy_progress_cb(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
    HTTP* ctx = reinterpret_cast<HTTP*>(clientp);
    ctx->event.fire(dltotal, dlnow);
    return ctx->is_cancel->load() ? 1 : CURL_PROGRESSFUNC_CONTINUE;
}

void HTTP::set_option(const Cancel& c) {
    this->is_cancel = std::move(c);
    curl_easy_setopt(this->easy, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(this->easy, CURLOPT_XFERINFOFUNCTION, easy_progress_cb);
    curl_easy_setopt(this->easy, CURLOPT_XFERINFODATA, this);
}

void HTTP::set_option(Progress::Callback p) {
    this->event.subscribe(p);
    curl_easy_setopt(this->easy, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(this->easy, CURLOPT_XFERINFOFUNCTION, easy_progress_cb);
    curl_easy_setopt(this->easy, CURLOPT_XFERINFODATA, this);
}

void HTTP::set_option(const Cookies& cookies) {
    std::stringstream ss;
    char* escaped;
    for (auto& c : cookies) {
        ss << c.name << "=";
        escaped = curl_easy_escape(this->easy, c.value.c_str(), c.value.size());
        if (escaped) {
            ss << escaped;
            curl_free(escaped);
        }
        ss << "; ";
    }
    this->cookie = ss.str();
}

void HTTP::set_basic_auth(const std::string& user, const std::string& passwd) {
    curl_easy_setopt(this->easy, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
    this->user = user;
    this->passwd = passwd;
}

static bool is_redirect(long code) { return code == 301 || code == 302 || code == 303 || code == 307 || code == 308; }

size_t HTTP::easy_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    HTTP* ctx = reinterpret_cast<HTTP*>(userdata);
    size_t count = size * nmemb;
    // the body of a redirect is not the answer, perform() follows it
    long code = 0;
    curl_easy_getinfo(ctx->easy, CURLINFO_RESPONSE_CODE, &code);
    if (!is_redirect(code)) ctx->body->write(ptr, count);
    return count;
}

/// libcurl sends the custom headers (X-Plex-Token, X-Emby-Token...) to whatever
/// host a redirect points at, so redirects are followed here: once the origin
/// changes, the request goes on without its headers, cookies or credentials.
int HTTP::perform(std::string url, std::ostream* body) {
    constexpr int max_redirects = 10;
    this->body = body;
    curl_easy_setopt(this->easy, CURLOPT_WRITEFUNCTION, easy_write_cb);
    curl_easy_setopt(this->easy, CURLOPT_WRITEDATA, this);
    curl_easy_setopt(this->easy, CURLOPT_PROXY, PROXY_STATUS ? PROXY.c_str() : nullptr);
    // a handle is reused across requests: restore what a previous redirect dropped
    curl_easy_setopt(this->easy, CURLOPT_HTTPHEADER, this->chunk);
    curl_easy_setopt(this->easy, CURLOPT_COOKIE, this->cookie.empty() ? nullptr : this->cookie.c_str());
    curl_easy_setopt(this->easy, CURLOPT_USERNAME, this->user.empty() ? nullptr : this->user.c_str());
    curl_easy_setopt(this->easy, CURLOPT_PASSWORD, this->user.empty() ? nullptr : this->passwd.c_str());

    for (int hops = 0;; ++hops) {
        // checked at each hop: a redirect can leave the server the user allowed
        bool verify = !insecure(url);
        curl_easy_setopt(this->easy, CURLOPT_SSL_VERIFYPEER, verify ? 1L : 0L);
        curl_easy_setopt(this->easy, CURLOPT_SSL_VERIFYHOST, verify ? 2L : 0L);
        // a plex.direct name is reached at the address it names, without DNS: a
        // router's DNS-rebinding protection refuses it for a LAN server, and the
        // certificate is still checked against the name
        curl_slist_free_all(this->connect_to);
        this->connect_to = nullptr;
        std::string ip = misc::plexDirectIp(url);
        if (!ip.empty()) {
            std::string origin = misc::urlOrigin(url);
            size_t hostStart = origin.find("://") + 3, colon = origin.rfind(':');
            std::string host = origin.substr(hostStart, colon - hostStart), port = origin.substr(colon + 1);
            this->connect_to = curl_slist_append(nullptr, fmt::format("{}:{}:{}:{}", host, port, ip, port).c_str());
        }
        curl_easy_setopt(this->easy, CURLOPT_CONNECT_TO, this->connect_to);
        curl_easy_setopt(this->easy, CURLOPT_URL, url.c_str());
        CURLcode res = curl_easy_perform(this->easy);
        if (res != CURLE_OK) throw curl_error(res);

        long status_code = 0;
        char* location = nullptr;
        curl_easy_getinfo(this->easy, CURLINFO_RESPONSE_CODE, &status_code);
        curl_easy_getinfo(this->easy, CURLINFO_REDIRECT_URL, &location);
        if (!is_redirect(status_code) || !location) return status_code;
        std::string next = location;

        if (hops == max_redirects) throw curl_error("too many redirects");
        std::string origin = misc::urlOrigin(next);
        if (origin.rfind("http://", 0) != 0 && origin.rfind("https://", 0) != 0)
            throw curl_error("redirect to an unsupported url");
        if (origin != misc::urlOrigin(url)) {
            // a 307/308 would send the body (a login form) to the other server as well
            if (this->body_method && (status_code == 307 || status_code == 308))
                throw curl_error("redirect of a request with a body to another server");
            curl_easy_setopt(this->easy, CURLOPT_HTTPHEADER, nullptr);
            curl_easy_setopt(this->easy, CURLOPT_COOKIE, nullptr);
            curl_easy_setopt(this->easy, CURLOPT_USERNAME, nullptr);
            curl_easy_setopt(this->easy, CURLOPT_PASSWORD, nullptr);
        }
        // what libcurl does by default: 303, and 301/302 after a POST, go on as GET
        if (status_code == 303 || ((status_code == 301 || status_code == 302) && this->body_method)) {
            curl_easy_setopt(this->easy, CURLOPT_CUSTOMREQUEST, nullptr);
            curl_easy_setopt(this->easy, CURLOPT_HTTPGET, 1L);
        }
        url = next;
    }
}

std::string HTTP::encode_form(const Form& form) {
    std::ostringstream ss;
    char* escaped;
    for (auto it = form.begin(); it != form.end(); ++it) {
        if (it->second.empty()) continue;
        if (it != form.begin()) ss << '&';
        escaped = curl_escape(it->second.c_str(), it->second.size());
        ss << it->first << '=' << escaped;
        curl_free(escaped);
    }
    return ss.str();
}

void HTTP::_get(const std::string& url, std::ostream* out) {
    curl_easy_setopt(this->easy, CURLOPT_HTTPGET, 1L);
    this->body_method = false;
    int code = this->perform(url, out);
    if (code >= 400) throw curl_error(fmt::format("http status {}", code));
}

bool HTTP::getinfo(char** arg) { return curl_easy_getinfo(this->easy, CURLINFO_CONTENT_TYPE, arg) == CURLE_OK; }

int HTTP::propfind(const std::string& url, std::ostream* out) {
    curl_easy_setopt(this->easy, CURLOPT_CUSTOMREQUEST, "PROPFIND");
    this->body_method = false;
    return this->perform(url, out);
}

std::string HTTP::_post(const std::string& url, const std::string& data) {
    std::ostringstream body;
    curl_easy_setopt(this->easy, CURLOPT_POSTFIELDS, data.c_str());
    curl_easy_setopt(this->easy, CURLOPT_POSTFIELDSIZE, data.size());
    this->body_method = true;
    int code = this->perform(url, &body);
    if (code >= 400) throw curl_error(fmt::format("http status {}", code));
    return body.str();
}

std::string HTTP::_put(const std::string& url, const std::string& data) {
    std::ostringstream body;
    curl_easy_setopt(this->easy, CURLOPT_POSTFIELDS, data.c_str());
    curl_easy_setopt(this->easy, CURLOPT_POSTFIELDSIZE, data.size());
    curl_easy_setopt(this->easy, CURLOPT_CUSTOMREQUEST, "PUT");
    this->body_method = true;
    int code = this->perform(url, &body);
    if (code >= 400) throw curl_error(fmt::format("http status {}", code));
    return body.str();
}

void HTTP::_delete(const std::string& url, std::ostream* out) {
    curl_easy_setopt(this->easy, CURLOPT_CUSTOMREQUEST, "DELETE");
    this->body_method = false;
    int code = this->perform(url, out);
    if (code >= 400) throw curl_error(fmt::format("http status {}", code));
}
