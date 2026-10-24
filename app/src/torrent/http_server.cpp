#include "torrent/http_server.hpp"

#if defined(__vita__)
// SceNet directly, as in socket.cpp.
#include <psp2/net/net.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <vector>

#include "torrent/log.hpp"

namespace torrent {

namespace {
std::string guessContentType(const std::string& name) {
    auto ends = [&](const char* ext) {
        size_t n = std::strlen(ext);
        return name.size() >= n && name.compare(name.size() - n, n, ext) == 0;
    };
    if (ends(".mp4") || ends(".m4v")) return "video/mp4";
    if (ends(".mkv")) return "video/x-matroska";
    if (ends(".webm")) return "video/webm";
    if (ends(".avi")) return "video/x-msvideo";
    if (ends(".ts")) return "video/mp2t";
    return "application/octet-stream";
}

std::string urlEncodePath(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~' || c == '/') {
            o += (char)c;
        } else {
            o += '%';
            o += hex[c >> 4];
            o += hex[c & 0xF];
        }
    }
    return o;
}

// The server sockets are blocking: only EINTR needs a retry. SceNet returns its
// error code instead of setting errno.
#if defined(__vita__)
inline int sockRecv(int fd, void* b, size_t n) { return sceNetRecv(fd, b, (unsigned)n, 0); }
inline int sockSend(int fd, const void* b, size_t n) { return sceNetSend(fd, b, (unsigned)n, 0); }
inline bool sockIsEintr(int r) { return (unsigned)r == (unsigned)SCE_NET_ERROR_EINTR; }
inline void sockClose(int fd) { sceNetSocketClose(fd); }
inline void sockShutdown(int fd) { sceNetShutdown(fd, SCE_NET_SHUT_RDWR); }
#else
inline int sockRecv(int fd, void* b, size_t n) { return (int)::recv(fd, b, n, 0); }
inline int sockSend(int fd, const void* b, size_t n) { return (int)::send(fd, b, n, 0); }
inline bool sockIsEintr(int) { return errno == EINTR; }
inline void sockClose(int fd) { ::close(fd); }
inline void sockShutdown(int fd) { ::shutdown(fd, SHUT_RDWR); }
#endif

bool sendAll(int fd, const char* data, size_t len) {
    size_t off = 0;
    while (off < len) {
        int n = sockSend(fd, data + off, len - off);
        if (n > 0)
            off += (size_t)n;
        else if (n < 0 && sockIsEintr(n))
            continue;
        else
            return false;
    }
    return true;
}
}  // namespace

HttpServer::HttpServer(PieceStore& store, int fileIdx, const std::string& fileName)
    : store_(store), fileIdx_(fileIdx), fileName_(fileName) {
    const FileEntry* fe = store_.meta().file(fileIdx_);
    fileLength_ = fe ? fe->length : 0;
}

HttpServer::~HttpServer() { stop(); }

bool HttpServer::start(uint16_t port) {
#if defined(__vita__)
    listenFd_ = sceNetSocket("gmca-http", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
    if (listenFd_ < 0) {
        listenFd_ = -1;
        return false;
    }
    int one = 1;
    sceNetSetsockopt(listenFd_, SCE_NET_SOL_SOCKET, SCE_NET_SO_REUSEADDR, &one, sizeof(one));

    SceNetSockaddrIn addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = SCE_NET_AF_INET;
    addr.sin_port = sceNetHtons(port);
    sceNetInetPton(SCE_NET_AF_INET, "127.0.0.1", &addr.sin_addr);
    if (sceNetBind(listenFd_, reinterpret_cast<SceNetSockaddr*>(&addr), sizeof(addr)) < 0) {
        sceNetSocketClose(listenFd_);
        listenFd_ = -1;
        return false;
    }
    unsigned alen = sizeof(addr);
    if (sceNetGetsockname(listenFd_, reinterpret_cast<SceNetSockaddr*>(&addr), &alen) == 0)
        port_ = sceNetNtohs(addr.sin_port);
    if (sceNetListen(listenFd_, 4) < 0) {
        sceNetSocketClose(listenFd_);
        listenFd_ = -1;
        return false;
    }
#else
    listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) return false;
    int one = 1;
    ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::bind(listenFd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(listenFd_);
        listenFd_ = -1;
        return false;
    }
    socklen_t alen = sizeof(addr);
    if (::getsockname(listenFd_, reinterpret_cast<struct sockaddr*>(&addr), &alen) == 0) port_ = ntohs(addr.sin_port);
    if (::listen(listenFd_, 4) < 0) {
        ::close(listenFd_);
        listenFd_ = -1;
        return false;
    }
#endif
    running_ = true;
    thread_ = std::thread([this] { acceptLoop(); });
    return true;
}

void HttpServer::stop() {
    running_ = false;
    store_.stop();  // unblocks readFile in the client threads
    if (listenFd_ >= 0) {
#if defined(__vita__)
        sceNetShutdown(listenFd_, SCE_NET_SHUT_RDWR);
        sceNetSocketClose(listenFd_);
#else
        ::shutdown(listenFd_, SHUT_RDWR);
        ::close(listenFd_);
#endif
        listenFd_ = -1;
    }
    if (thread_.joinable()) thread_.join();
    // Clients may be blocked sending to mpv: shut their sockets so they return,
    // they reference this server and the store the engine frees next.
    std::list<Client> clients;
    {
        std::lock_guard<std::mutex> lk(clientsMutex_);
        for (auto& c : clients_)
            if (!c.done->load()) sockShutdown(c.fd);
        clients.swap(clients_);
    }
    for (auto& c : clients) c.thread.join();
}

std::string HttpServer::url() const {
    return "http://127.0.0.1:" + std::to_string(port_) + "/" + urlEncodePath(fileName_);
}

void HttpServer::acceptLoop() {
    while (running_) {
#if defined(__vita__)
        int fd = sceNetAccept(listenFd_, nullptr, nullptr);
#else
        int fd = ::accept(listenFd_, nullptr, nullptr);
#endif
        if (fd < 0) {
            if (!running_) break;
            continue;
        }
        // mpv may hold a probe and a stream connection at once, and opens a new one per seek.
        std::lock_guard<std::mutex> lk(clientsMutex_);
        for (auto it = clients_.begin(); it != clients_.end();) {
            if (it->done->load()) {
                it->thread.join();
                it = clients_.erase(it);
            } else {
                ++it;
            }
        }
        auto done = std::make_shared<std::atomic<bool>>(false);
        clients_.push_back({fd, done, std::thread([this, fd, done] {
                                handleClient(fd);
                                std::lock_guard<std::mutex> lk(clientsMutex_);
                                sockClose(fd);
                                done->store(true);
                            })});
    }
}

void HttpServer::handleClient(int fd) {
    std::string req;
    char buf[4096];
    while (req.find("\r\n\r\n") == std::string::npos) {
        int n = sockRecv(fd, buf, sizeof(buf));
        if (n <= 0) return;
        req.append(buf, (size_t)n);
        if (req.size() > 16384) return;
    }

    bool head = req.rfind("HEAD ", 0) == 0;
    if (!head && req.rfind("GET ", 0) != 0) {
        const char* r = "HTTP/1.1 405 Method Not Allowed\r\nAllow: GET, HEAD\r\nContent-Length: 0\r\n\r\n";
        sendAll(fd, r, std::strlen(r));
        return;
    }

    int64_t start = 0, end = fileLength_ - 1;
    bool partial = false;
    {
        std::string lower = req;
        for (auto& c : lower) c = (char)tolower((unsigned char)c);
        size_t rp = lower.find("range:");
        if (rp != std::string::npos) {
            size_t eq = lower.find("bytes=", rp);
            if (eq != std::string::npos) {
                eq += 6;
                size_t dash = lower.find('-', eq);
                size_t crlf = lower.find('\r', dash);
                std::string a = lower.substr(eq, dash - eq);
                std::string b = lower.substr(dash + 1, crlf - dash - 1);
                // An exception escaping a detached thread terminates the process.
                try {
                    if (!a.empty()) {
                        start = std::stoll(a);
                        if (!b.empty()) end = std::stoll(b);
                    } else if (!b.empty()) {  // suffix range: the last N bytes
                        start = fileLength_ - std::stoll(b);
                    }
                    partial = true;
                } catch (const std::exception&) {
                    start = 0;
                    end = fileLength_ - 1;
                    partial = false;
                }
            }
        }
    }
    if (fileLength_ <= 0) {
        const char* r = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n\r\n";
        sendAll(fd, r, std::strlen(r));
        return;
    }
    start = std::max<int64_t>(0, start);
    end = std::min<int64_t>(end, fileLength_ - 1);
    if (start > end) {
        std::string r = "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */" + std::to_string(fileLength_) +
                        "\r\nContent-Length: 0\r\n\r\n";
        sendAll(fd, r.data(), r.size());
        return;
    }
    int64_t length = end - start + 1;

    store_.setPlayhead(fileIdx_, start);

    std::string header;
    header += partial ? "HTTP/1.1 206 Partial Content\r\n" : "HTTP/1.1 200 OK\r\n";
    header += "Content-Type: " + guessContentType(fileName_) + "\r\n";
    header += "Accept-Ranges: bytes\r\n";
    header += "Content-Length: " + std::to_string(length) + "\r\n";
    if (partial)
        header += "Content-Range: bytes " + std::to_string(start) + "-" + std::to_string(end) + "/" +
                  std::to_string(fileLength_) + "\r\n";
    header += "Connection: close\r\n\r\n";
    if (!sendAll(fd, header.data(), header.size())) return;
    if (head) return;

    const int64_t kChunk = 256 * 1024;
    std::vector<uint8_t> chunk((size_t)kChunk);
    int64_t pos = start;
    while (pos <= end && running_) {
        int64_t want = std::min(kChunk, end - pos + 1);
        int64_t got = store_.readFile(fileIdx_, pos, chunk.data(), want);
        if (got <= 0) break;
        if (!sendAll(fd, reinterpret_cast<char*>(chunk.data()), (size_t)got)) break;
        pos += got;
        store_.setPlayhead(fileIdx_, pos);
    }
}

}  // namespace torrent
