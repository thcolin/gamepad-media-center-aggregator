/*
    Copyright 2023 dragonflylee
*/

#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <atomic>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <curl/system.h>

#include <borealis/core/event.hpp>

class HTTP {
public:
    using Header = std::vector<std::string>;
    using Form = std::unordered_map<std::string, std::string>;
    // For cancellable requests
    using Cancel = std::shared_ptr<std::atomic_bool>;
    using Progress = brls::Event<curl_off_t, curl_off_t>;

    inline static bool PROXY_STATUS = false;
    inline static std::string PROXY;

    struct Range {
        int start = 0;
        int end = 0;
    };

    struct Timeout {
        // Whole-request budget in ms (-1 = unlimited). `connect` is the
        // connect+DNS budget: keeping it shorter than `timeout` lets an
        // unreachable host fail fast while a reachable but high-latency endpoint
        // still gets the full budget to complete its TLS handshake + response
        // (GH #36 — roaming/WAN probes). `connect <= 0` keeps them identical.
        long timeout = TIMEOUT;
        long connect = 0;
    };

    struct Cookie {
        std::string name;
        std::string value;
        std::string domain;
        std::string path;
        bool http_only;
    };

    using Cookies = std::vector<Cookie>;

    /// Coarse category of a failed request, exposed so callers can react to a
    /// class of failure without depending on <curl/curl.h> error codes. Kept
    /// deliberately small — only distinctions the UI acts on live here.
    enum class ErrorKind {
        Other,          ///< HTTP status >= 400, connect/TLS failure, timeout, proxy resolve, …
        ResolveFailed,  ///< the server host failed to resolve (curl COULDNT_RESOLVE_HOST)
    };

    /// Exception thrown by every request path. `what()` carries a
    /// human-readable message (curl's strerror or an "http status N" string);
    /// `kind` lets a caller special-case a category — e.g. turn a DNS failure
    /// into an actionable "use the server's IP address" hint (the consoles have
    /// no mDNS and are IPv4-only, so .local / IPv6-only hosts never resolve).
    class Error : public std::runtime_error {
    public:
        Error(ErrorKind kind, const std::string& message) : std::runtime_error(message), kind(kind) {}
        ErrorKind kind;
    };

    HTTP();
    HTTP(const HTTP& other) = delete;
    ~HTTP();

    static std::string encode_form(const Form& form);
    void _get(const std::string& url, std::ostream* out);
    bool getinfo(char** arg);
    int propfind(const std::string& url, std::ostream* out);
    std::string _post(const std::string& url, const std::string& data);
    std::string _put(const std::string& url, const std::string& data);
    void set_user_agent(const std::string& agent);
    void set_basic_auth(const std::string& user, const std::string& passwd);
    void _delete(const std::string& url, std::ostream* out);

    template <typename... Ts>
    static void set_option(HTTP& s, Ts&&... ts) {
        s.set_option_internal<Ts...>(std::forward<Ts>(ts)...);
    }

    // Get methods
    template <typename... Ts>
    static std::string get(const std::string& url, Ts&&... ts) {
        HTTP s;
        std::ostringstream body;
        set_option(s, std::forward<Ts>(ts)...);
        s._get(url, &body);
        return body.str();
    }

    template <typename... Ts>
    static void download(const std::string& url, const std::string& path, Ts&&... ts) {
        HTTP s;
        std::ofstream of(path, std::ios_base::binary);
        set_option(s, std::forward<Ts>(ts)...);
        s._get(url, &of);
    }

    // Post methods
    template <typename... Ts>
    static std::string post(const std::string& url, const std::string& data, Ts&&... ts) {
        HTTP s;
        set_option(s, std::forward<Ts>(ts)...);
        return s._post(url, data);
    }

    template <typename... Ts>
    static std::string post(const std::string& url, const Form& form, Ts&&... ts) {
        HTTP s;
        set_option(s, std::forward<Ts>(ts)...);
        return s._post(url, s.encode_form(form));
    }

    // Put methods (the plex.tv provider API uses PUT for watchlist actions;
    // parameters go in the query string, empty body)
    template <typename... Ts>
    static std::string put(const std::string& url, const std::string& data, Ts&&... ts) {
        HTTP s;
        set_option(s, std::forward<Ts>(ts)...);
        return s._put(url, data);
    }

    inline static long TIMEOUT = 3000L;

private:
    static size_t easy_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata);
    static int easy_progress_cb(
        void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow);
    int perform(std::ostream* body);

    void add_header(const std::string& header);
    void set_option(const Header& hs);
    void set_option(const Range& r);
    void set_option(const Timeout& t);
    void set_option(const Cancel& c);
    void set_option(const Cookies& cookies);
    void set_option(Progress::Callback p);

    template <typename CT>
    void set_option_internal(CT&& option) {
        set_option(std::forward<CT>(option));
    }

    template <typename CT, typename... Ts>
    void set_option_internal(CT&& option, Ts&&... ts) {
        set_option_internal<CT>(std::forward<CT>(option));
        set_option_internal<Ts...>(std::forward<Ts>(ts)...);
    }

    void* easy;
    struct curl_slist* chunk;
    Cancel is_cancel;
    Progress event;
};
