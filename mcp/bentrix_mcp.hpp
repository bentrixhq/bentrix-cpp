// bentrix_mcp.hpp - the hand-written half of BENTRIX's MCP client for C++17: the connection and everything
// that does not change when a capability is added (bentrix_mcp_generated.hpp is the rest, from the
// capability catalogue). Header-only, no dependency: the endpoint is always http://127.0.0.1:<port> and
// the server answers one request per connection and closes it, so HTTP is the platform's own sockets
// (POSIX, or Winsock: link ws2_32 on Windows) and a few lines.
//
// It speaks MCP revision 2026-07-28 over Streamable HTTP: every request is its own POST carrying the
// protocol version, the client's identity and its (empty) capabilities in _meta - the same wire as the
// Python client's _runtime.py and the JavaScript client's client.js, field for field.
//
//   #include "bentrix_mcp_generated.hpp"
//   auto b = bentrix::mcp::Bentrix::connect("my-agent");
//   auto state = b.workspace.get_state(bentrix::mcp::WorkspaceApi::GetStateArgs());
#pragma once

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace bentrix::mcp {

inline constexpr const char *PROTOCOL_VERSION = "2026-07-28";
inline constexpr const char *CLIENT_NAME = "bentrix-cpp-sdk";
inline constexpr const char *CLIENT_VERSION = "0.1.0";
// A call that asks the server to wait is given that long and this much more (found 2026-11-02: wait_for
// beyond the client's own timeout ended in the client before the server could answer).
inline constexpr int LONG_POLL_MARGIN_S = 15;
inline constexpr int TIMEOUT_S = 60;
/// **Busy, try again**: the JSON-RPC code Bentrix answers when it refused a request BEFORE doing
/// anything with it — one program's 20 calls a second spent, too many connections or streams, a task
/// it could not read in time. Nothing ran, so the same request is sent again after the wait the answer
/// names (retryAfterMs, a second when none), at most BUSY_RETRIES times and each wait at most
/// BUSY_WAIT_MAX_MS; then the refusal is thrown. An order is never sent twice by this.
inline constexpr int64_t BUSY = -40001;
inline constexpr int BUSY_RETRIES = 10;
inline constexpr int64_t BUSY_WAIT_MAX_MS = 5000;

/// A refusal or a failure, in BENTRIX's own words.
struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};
/// A JSON-RPC error: the request itself was refused.
struct RpcError : Error {
    int64_t code;
    /// The wait a busy answer names, in milliseconds; 0 when it names none.
    int64_t retry_after_ms = 0;
    RpcError(int64_t c, const std::string &m, int64_t after_ms = 0) : Error("[" + std::to_string(c) + "] " + m), code(c), retry_after_ms(after_ms) {}
};
/// A tool ran and answered isError: the product's own sentence, verbatim. `machine` is the server's
/// word for the kind (busy, expired, insufficient_scope...) when it gave one.
struct ToolError : Error {
    std::string tool;
    std::string machine;
    ToolError(const std::string &t, const std::string &m, const std::string &k = "") : Error(t + ": " + m), tool(t), machine(k) {}
};
struct NotRunning : Error {
    using Error::Error;
};
struct PairingRefused : Error {
    using Error::Error;
};

// ---- a small JSON value -----------------------------------------------------------------------------

struct Json;
using Array = std::vector<Json>;
using Object = std::map<std::string, Json>;

/// A JSON value: null, bool, an integer, a double, a string, an array or an object. A number read is a
/// double (strtod reads the double the engine wrote); an integer given is written whole.
struct Json {
    using Array = mcp::Array;
    using Object = mcp::Object;
    std::variant<std::nullptr_t, bool, int64_t, double, std::string, Array, Object> v = nullptr;

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool b) : v(b) {}
    Json(int i) : v(int64_t(i)) {}
    Json(long i) : v(int64_t(i)) {}
    Json(long long i) : v(int64_t(i)) {}
    Json(double d) : v(d) {}
    Json(const char *s) : v(std::string(s)) {}
    Json(std::string s) : v(std::move(s)) {}
    Json(Array a) : v(std::move(a)) {}
    Json(Object o) : v(std::move(o)) {}

    template <class T>
    static Json list(const std::vector<T> &xs) {
        Array a;
        for (const auto &x : xs) a.push_back(Json(x));
        return Json(std::move(a));
    }

    bool is_null() const { return std::holds_alternative<std::nullptr_t>(v); }
    bool is_object() const { return std::holds_alternative<Object>(v); }
    bool is_array() const { return std::holds_alternative<Array>(v); }
    bool is_string() const { return std::holds_alternative<std::string>(v); }
    bool is_number() const { return std::holds_alternative<int64_t>(v) || std::holds_alternative<double>(v); }
    double num() const { return std::holds_alternative<int64_t>(v) ? double(std::get<int64_t>(v)) : std::get<double>(v); }
    bool boolean() const { return std::get<bool>(v); }
    const std::string &str() const { return std::get<std::string>(v); }
    const Array &arr() const { return std::get<Array>(v); }
    const Object &obj() const { return std::get<Object>(v); }
    bool has(const std::string &k) const { return is_object() && obj().count(k) > 0; }
    const Json &operator[](const std::string &k) const {
        static const Json none;
        if (!is_object()) return none;
        auto it = obj().find(k);
        return it == obj().end() ? none : it->second;
    }
    const Json &operator[](size_t i) const { return arr().at(i); }
    bool operator==(const Json &o) const { return v == o.v; }
    bool operator!=(const Json &o) const { return !(*this == o); }

    static Json parse(const std::string &text) {
        size_t i = 0;
        Json out = value(text, i);
        space(text, i);
        if (i != text.size()) throw Error("JSON: trailing text");
        return out;
    }

    /// The shortest of 15 or 17 significant digits that reads back as the same double.
    static std::string number(double d) {
        if (!std::isfinite(d)) throw Error("JSON: a number that is not finite");
        char buf[40];
        std::snprintf(buf, sizeof buf, "%.15g", d);
        if (std::strtod(buf, nullptr) != d) std::snprintf(buf, sizeof buf, "%.17g", d);
        return buf;
    }

    std::string dump() const {
        std::string out;
        write(out);
        return out;
    }

    void write(std::string &out) const {
        if (is_null()) {
            out += "null";
        } else if (auto b = std::get_if<bool>(&v)) {
            out += *b ? "true" : "false";
        } else if (auto n = std::get_if<int64_t>(&v)) {
            out += std::to_string(*n);
        } else if (auto d = std::get_if<double>(&v)) {
            out += number(*d);
        } else if (auto s = std::get_if<std::string>(&v)) {
            quote(*s, out);
        } else if (auto a = std::get_if<Array>(&v)) {
            out += '[';
            for (size_t i = 0; i < a->size(); i++) {
                if (i) out += ',';
                (*a)[i].write(out);
            }
            out += ']';
        } else {
            out += '{';
            bool first = true;
            for (const auto &[k, x] : std::get<Object>(v)) {
                if (x.is_null()) continue;  // an argument not given is absent, never null
                if (!first) out += ',';
                first = false;
                quote(k, out);
                out += ':';
                x.write(out);
            }
            out += '}';
        }
    }

  private:
    static void quote(const std::string &s, std::string &out) {
        out += '"';
        for (unsigned char c : s) {
            if (c == '"' || c == '\\') {
                out += '\\';
                out += char(c);
            } else if (c == '\n') {
                out += "\\n";
            } else if (c == '\r') {
                out += "\\r";
            } else if (c == '\t') {
                out += "\\t";
            } else if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else {
                out += char(c);
            }
        }
        out += '"';
    }
    static void space(const std::string &s, size_t &i) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) i++;
    }
    static void utf8(unsigned cp, std::string &out) {
        if (cp < 0x80) out += char(cp);
        else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
        else { out += char(0xF0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 0x3F)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
    }
    static std::string string_at(const std::string &s, size_t &i) {
        std::string out;
        i++;
        while (i < s.size() && s[i] != '"') {
            char c = s[i++];
            if (c != '\\') {
                out += c;
                continue;
            }
            char e = s.at(i++);
            switch (e) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    unsigned cp = std::stoul(s.substr(i, 4), nullptr, 16);
                    i += 4;
                    if (cp >= 0xD800 && cp <= 0xDBFF && s.compare(i, 2, "\\u") == 0) {
                        unsigned lo = std::stoul(s.substr(i + 2, 4), nullptr, 16);
                        i += 6;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    utf8(cp, out);
                    break;
                }
                default: out += e;
            }
        }
        i++;
        return out;
    }
    static Json value(const std::string &s, size_t &i) {
        space(s, i);
        if (i >= s.size()) throw Error("JSON: unexpected end");
        char c = s[i];
        if (c == '{') {
            Object o;
            i++;
            space(s, i);
            if (s.at(i) == '}') { i++; return Json(std::move(o)); }
            for (;;) {
                space(s, i);
                std::string k = string_at(s, i);
                space(s, i);
                i++;  // ':'
                o[k] = value(s, i);
                space(s, i);
                if (s.at(i++) != ',') break;
            }
            return Json(std::move(o));
        }
        if (c == '[') {
            Array a;
            i++;
            space(s, i);
            if (s.at(i) == ']') { i++; return Json(std::move(a)); }
            for (;;) {
                a.push_back(value(s, i));
                space(s, i);
                if (s.at(i++) != ',') break;
            }
            return Json(std::move(a));
        }
        if (c == '"') return Json(string_at(s, i));
        if (s.compare(i, 4, "true") == 0) { i += 4; return Json(true); }
        if (s.compare(i, 5, "false") == 0) { i += 5; return Json(false); }
        if (s.compare(i, 4, "null") == 0) { i += 4; return Json(); }
        const char *start = s.c_str() + i;
        char *end = nullptr;
        double d = std::strtod(start, &end);
        if (end == start) throw Error("JSON: not a value");
        i += size_t(end - start);
        return Json(d);
    }
};

// ---- loopback HTTP ------------------------------------------------------------------------------------

struct Response {
    int status = 0;
    std::string content_type;
    std::string body;
};

namespace detail {

#ifdef _WIN32
using Socket = SOCKET;
inline void close_socket(Socket s) { closesocket(s); }
inline bool bad(Socket s) { return s == INVALID_SOCKET; }
struct WinsockUp {
    WinsockUp() {
        WSADATA d;
        WSAStartup(MAKEWORD(2, 2), &d);
    }
};
#else
using Socket = int;
inline void close_socket(Socket s) { ::close(s); }
inline bool bad(Socket s) { return s < 0; }
#endif

inline std::string lower(std::string s) {
    for (auto &c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

/// One POST to http://127.0.0.1:<port>/<path>: the server closes after its answer, so the body is
/// everything to the close, cut at a Content-Length when one is given.
inline Response post(const std::string &url, const std::string &body, const std::vector<std::pair<std::string, std::string>> &headers, int timeout_s = 60) {
#ifdef _WIN32
    static WinsockUp up;
#endif
    const std::string prefix = "http://";
    if (url.compare(0, prefix.size(), prefix) != 0) throw Error(url + " is not an http:// URL");
    std::string rest = url.substr(prefix.size());
    auto slash = rest.find('/');
    std::string hostport = rest.substr(0, slash), path = slash == std::string::npos ? "/" : rest.substr(slash);
    auto colon = hostport.rfind(':');
    std::string host = hostport.substr(0, colon);
    int port = colon == std::string::npos ? 80 : std::atoi(hostport.c_str() + colon + 1);

    Socket s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (bad(s)) throw Error(url + ": no socket");
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(uint16_t(port));
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        close_socket(s);
        throw Error(url + ": not a numeric address");
    }
#ifdef _WIN32
    DWORD tv = DWORD(timeout_s * 1000);
#else
    timeval tv{timeout_s, 0};
#endif
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&tv), sizeof tv);
    if (::connect(s, reinterpret_cast<sockaddr *>(&addr), sizeof addr) != 0) {
        close_socket(s);
        throw Error(url + ": the connection was refused");
    }
    std::string req = "POST " + path + " HTTP/1.1\r\nHost: " + hostport + "\r\nConnection: close\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
    for (const auto &[k, v] : headers) req += k + ": " + v + "\r\n";
    req += "\r\n" + body;
    for (size_t sent = 0; sent < req.size();) {
        auto n = ::send(s, req.data() + sent, int(req.size() - sent), 0);
        if (n <= 0) {
            close_socket(s);
            throw Error(url + ": the request could not be sent");
        }
        sent += size_t(n);
    }
    std::string raw;
    char buf[65536];
    for (;;) {
        auto n = ::recv(s, buf, sizeof buf, 0);
        if (n < 0) {
            close_socket(s);
            throw Error(url + ": no answer within " + std::to_string(timeout_s) + " s");
        }
        if (n == 0) break;
        raw.append(buf, size_t(n));
    }
    close_socket(s);
    auto split = raw.find("\r\n\r\n");
    if (split == std::string::npos) throw Error(url + ": a response with no head");
    Response r;
    std::istringstream head(raw.substr(0, split));
    std::string line, proto;
    std::getline(head, line);
    std::istringstream(line) >> proto >> r.status;
    long length = -1;
    while (std::getline(head, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto c = line.find(':');
        if (c == std::string::npos) continue;
        std::string k = lower(line.substr(0, c)), v = line.substr(c + 1);
        while (!v.empty() && v.front() == ' ') v.erase(0, 1);
        if (k == "content-type") r.content_type = v;
        if (k == "content-length") length = std::atol(v.c_str());
    }
    r.body = raw.substr(split + 4);
    if (length >= 0 && size_t(length) < r.body.size()) r.body.resize(size_t(length));
    return r;
}

inline std::string base64(const std::string &in) {
    static const char *t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < in.size(); i += 3) {
        uint32_t n = uint32_t((unsigned char)in[i]) << 16;
        if (i + 1 < in.size()) n |= uint32_t((unsigned char)in[i + 1]) << 8;
        if (i + 2 < in.size()) n |= uint32_t((unsigned char)in[i + 2]);
        for (size_t k = 0; k < 4; k++) out += k <= in.size() - i ? t[(n >> (18 - 6 * k)) & 63] : '=';
    }
    return out;
}

/// A header value, or the standard Base64 sentinel when plain ASCII cannot carry it.
inline std::string header_value(const std::string &v) {
    bool safe = !v.empty() && v.front() != ' ' && v.back() != ' ' && !(v.rfind("=?base64?", 0) == 0 && v.size() >= 2 && v.compare(v.size() - 2, 2, "?=") == 0);
    for (unsigned char c : v) safe = safe && c >= 0x20 && c <= 0x7e;
    return safe ? v : "=?base64?" + base64(v) + "?=";
}

inline std::string escape(const std::string &s) {
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += char(c);
        } else {
            char buf[4];
            std::snprintf(buf, sizeof buf, "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

inline std::string scalar(const Json &v) {
    if (v.is_string()) return v.str();
    return v.dump();
}

/// The RFC 6570 subset the catalogue uses: {name} and {?a,b}. A null optional is left out.
inline std::string expand_template(const std::string &tpl, const Json &vars) {
    std::string out;
    size_t i = 0;
    while (i < tpl.size()) {
        auto open = tpl.find('{', i);
        if (open == std::string::npos) {
            out += tpl.substr(i);
            break;
        }
        out += tpl.substr(i, open - i);
        auto close = tpl.find('}', open);
        if (close == std::string::npos) throw Error(tpl + " is not a URI template");
        std::string inner = tpl.substr(open + 1, close - open - 1);
        if (!inner.empty() && (inner[0] == '?' || inner[0] == '&')) {
            std::string pairs;
            std::istringstream names(inner.substr(1));
            std::string n;
            while (std::getline(names, n, ',')) {
                if (!vars[n].is_null()) pairs += (pairs.empty() ? "" : "&") + n + "=" + escape(scalar(vars[n]));
            }
            if (!pairs.empty()) out += inner[0] + pairs;
        } else {
            if (vars[inner].is_null()) throw Error("the URI template " + tpl + " needs " + inner);
            out += escape(scalar(vars[inner]));
        }
        i = close + 1;
    }
    return out;
}

/// The final response on a request-scoped event stream; notifications before it are ignored.
inline Json read_sse(const std::string &text, int64_t id) {
    std::vector<std::string> data;
    std::istringstream in(text + "\n\n");
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) {
            if (!data.empty()) {
                std::string joined;
                for (size_t k = 0; k < data.size(); k++) joined += (k ? "\n" : "") + data[k];
                try {
                    Json msg = Json::parse(joined);
                    if (msg["id"].num() == double(id) && (msg.has("result") || msg.has("error"))) return msg;
                } catch (const std::exception &) {
                    // not ours
                }
            }
            data.clear();
        } else if (line.rfind("data:", 0) == 0) {
            std::string d = line.substr(5);
            while (!d.empty() && d.front() == ' ') d.erase(0, 1);
            data.push_back(d);
        }
    }
    throw Error("the stream ended without a response");
}

}  // namespace detail

class Client;

/// A long operation: wait() answers its result once it completes (the person may act first).
class Job {
  public:
    Job(Client &c, std::string handle, Json result) : c_(c), handle_(std::move(handle)), result_(std::move(result)) {}
    const std::string &handle() const { return handle_; }
    Json status();
    void cancel();
    /// Poll until the job completes or fails; a timeout of 0 waits as long as it takes.
    Json wait(double timeout_s = 0, double poll_s = 0.5);

  private:
    Client &c_;
    std::string handle_;
    Json result_;
};

/// One BENTRIX endpoint. Extended by the generated Bentrix.
class Client {
  public:
    Client(std::string url, std::string token, std::string catalogue_version) : url_(std::move(url)), token_(std::move(token)), catalogue_version_(std::move(catalogue_version)) {}
    const std::string &url() const { return url_; }
    const std::string &catalogue_version() const { return catalogue_version_; }

    /// A tool by its wire name, its arguments a JSON object; null arguments are left out.
    Json call(const std::string &name, const Json &args = Json(Object{})) {
        Object params{{"name", Json(name)}, {"arguments", args.is_null() ? Json(Object{}) : args}};
        int timeout_s = TIMEOUT_S;
        if (args.has("timeout_ms") && !args["timeout_ms"].is_null()) timeout_s = std::max(TIMEOUT_S, int(args["timeout_ms"].num() / 1000) + 1 + LONG_POLL_MARGIN_S);
        Json res = rpc("tools/call", std::move(params), name, timeout_s);
        if (res["resultType"] == Json("input_required")) throw Error(name + " needs input this client cannot give; answer it in Bentrix's window");
        std::string text;
        if (res["content"].is_array()) {
            for (const auto &p : res["content"].arr()) {
                if (p["type"] == Json("text") && p["text"].is_string()) text += p["text"].str();
            }
        }
        if (res["isError"] == Json(true)) {
            const Json &k = res["_meta"]["bentrix/machine"];
            throw ToolError(name, text.empty() ? "the tool failed" : text, k.is_string() ? k.str() : "");
        }
        if (!res["structuredContent"].is_null()) return res["structuredContent"];
        try {
            return text.empty() ? Json(Object{}) : Json::parse(text);
        } catch (const std::exception &) {
            return Json(Object{});
        }
    }

    /// A long operation: a Job whether the server answered at once or with a handle.
    Job call_job(const std::string &name, const Json &args = Json(Object{})) {
        Json raw = call(name, args);
        if (raw.is_object() && raw.obj().size() == 1 && raw["job"].is_string()) return Job(*this, raw["job"].str(), Json());
        return Job(*this, "", raw);
    }

    Json read_resource(const std::string &tpl, const Json &vars = Json(Object{})) { return read_uri(detail::expand_template(tpl, vars)); }

    /// Every page of a cursor-paged resource: next is the URI of the following page.
    std::vector<Json> read_pages(const std::string &tpl, const Json &vars = Json(Object{})) {
        std::vector<Json> pages;
        std::string uri = detail::expand_template(tpl, vars);
        while (!uri.empty()) {
            pages.push_back(read_uri(uri));
            const Json &next = pages.back()["next"];
            uri = next.is_string() ? next.str() : "";
        }
        return pages;
    }

    /// One wait on a stream: {items, next}; give next back as after to read on. A page with `rewound`
    /// true means the workbench's clock went back behind `after`: what came after it comes again, from
    /// `next`. A call answered busy or expired is asked again, five times half a second apart (the
    /// strict audit of 2026-11-04, S4).
    Json wait_for(const std::string &tpl, const Json &vars, const std::string &after = "", int64_t timeout_ms = 30000) {
        Object args{{"uri", Json(detail::expand_template(tpl, vars))}, {"timeout_ms", Json(timeout_ms)}};
        if (!after.empty()) args["after"] = Json(after);
        const Json request(std::move(args));
        for (int attempt = 0;; ++attempt) {
            try {
                return call("wait_for", request);
            } catch (const ToolError &e) {
                if ((e.machine != "busy" && e.machine != "expired") || attempt >= 4) throw;
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        }
    }

    /// Refuse a server on another major line of the catalogue.
    std::string verify_catalogue() {
        std::string server = call("agent_whoami")["catalogue_version"].str();
        auto major = [](const std::string &v) {
            auto dot = v.find('.');
            if (dot == std::string::npos || dot == 0) throw Error(v + " is not a version major.minor.patch");
            return std::atol(v.substr(0, dot).c_str());
        };
        if (major(server) != major(catalogue_version_)) throw Error("this SDK was generated from catalogue " + catalogue_version_ + "; the server's is " + server);
        return server;
    }

  private:
    /// One request, sent again while Bentrix answers busy (BUSY): it ran nothing.
    Json rpc(const std::string &method, Object params, const std::string &name, int timeout_s = TIMEOUT_S) {
        for (int attempt = 0;; ++attempt) {
            try {
                return rpc_once(method, params, name, timeout_s);
            } catch (const RpcError &e) {
                if (e.code != BUSY || attempt >= BUSY_RETRIES) throw;
                int64_t ms = e.retry_after_ms > 0 ? e.retry_after_ms : 1000;
                std::this_thread::sleep_for(std::chrono::milliseconds(std::min(ms, BUSY_WAIT_MAX_MS)));
            }
        }
    }

    Json rpc_once(const std::string &method, Object params, const std::string &name, int timeout_s) {
        int64_t id = ++id_;
        params["_meta"] = Json(Object{
            {"io.modelcontextprotocol/protocolVersion", Json(PROTOCOL_VERSION)},
            {"io.modelcontextprotocol/clientInfo", Json(Object{{"name", Json(CLIENT_NAME)}, {"version", Json(CLIENT_VERSION)}})},
            {"io.modelcontextprotocol/clientCapabilities", Json(Object{})},
        });
        Json body(Object{{"jsonrpc", Json("2.0")}, {"id", Json(id)}, {"method", Json(method)}, {"params", Json(std::move(params))}});
        std::vector<std::pair<std::string, std::string>> headers{
            {"Content-Type", "application/json"},
            {"Accept", "application/json, text/event-stream"},
            {"MCP-Protocol-Version", PROTOCOL_VERSION},
            {"Mcp-Method", method},
            {"Authorization", "Bearer " + token_},
            {"Mcp-Name", detail::header_value(name)},
        };
        Response r = detail::post(url_, body.dump(), headers, timeout_s);
        Json msg;
        if (r.content_type.rfind("text/event-stream", 0) == 0) {
            msg = detail::read_sse(r.body, id);
        } else {
            try {
                msg = Json::parse(r.body);
            } catch (const std::exception &) {
                throw RpcError(r.status, "HTTP " + std::to_string(r.status) + " with no JSON-RPC body");
            }
        }
        if (!msg["error"].is_null()) {
            const Json &e = msg["error"];
            const Json &after = e["data"]["retryAfterMs"];
            throw RpcError(e["code"].is_null() ? r.status : int64_t(e["code"].num()), e["message"].is_string() ? e["message"].str() : "",
                           after.is_number() ? int64_t(after.num()) : 0);
        }
        return msg["result"];
    }

    Json read_uri(const std::string &uri) {
        Json res = rpc("resources/read", Object{{"uri", Json(uri)}}, uri);
        if (!res["contents"].is_array() || res["contents"].arr().empty() || !res["contents"][0]["text"].is_string()) throw Error(uri + " answered no text content");
        return Json::parse(res["contents"][0]["text"].str());
    }

    std::string url_, token_, catalogue_version_;
    int64_t id_ = 0;
};

inline Json Job::status() {
    if (handle_.empty()) return Json(Object{{"status", Json("completed")}});
    return c_.call("job_get", Json(Object{{"job", Json(handle_)}}));
}

inline void Job::cancel() {
    if (!handle_.empty()) c_.call("job_cancel", Json(Object{{"job", Json(handle_)}}));
}

inline Json Job::wait(double timeout_s, double poll_s) {
    if (handle_.empty()) return result_;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeout_s);
    for (;;) {
        Json st = status();
        std::string s = st["status"].is_string() ? st["status"].str() : "";
        if (s == "completed") return st["result"].is_null() ? Json(Object{}) : st["result"];
        if (s == "failed" || s == "cancelled") throw Error("job " + handle_ + " " + s + ": " + (st["message"].is_string() ? st["message"].str() : ""));
        if (timeout_s > 0 && std::chrono::steady_clock::now() >= deadline) throw Error("job " + handle_ + " is still " + s);
        std::this_thread::sleep_for(std::chrono::duration<double>(poll_s));
    }
}

// ---- finding the running application, and pairing with it ------------------------------------------

/// The folder BENTRIX writes its endpoint and secret into - user_dir(Data, "agents"), as the application has it.
inline std::string agents_dir() {
    const char *lad = std::getenv("LOCALAPPDATA");
    if (lad && *lad) return std::string(lad) + "/BENTRIX/agents";
    const char *home = std::getenv("HOME");
    if (home && *home) return std::string(home) + "/.local/share/bentrix/agents";
    return "bentrix-agents";
}

inline std::string read_file(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw Error("no readable " + path);
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}

inline std::string read_endpoint(const std::string &folder) {
    std::string file = folder + "/endpoint.json";
    Json doc;
    try {
        doc = Json::parse(read_file(file));
    } catch (const std::exception &) {
        throw NotRunning("Bentrix is not running: there is no readable endpoint.json in " + folder);
    }
    if (!doc["url"].is_string() || doc["url"].str().rfind("http://127.0.0.1:", 0) != 0 || doc["pid"].is_null()) throw NotRunning("Bentrix is not running: " + file + " is not an endpoint file");
#ifndef _WIN32
    auto pid = pid_t(doc["pid"].num());
    if (::kill(pid, 0) != 0 && errno != EPERM) throw NotRunning("Bentrix is not running: " + file + " is stale (process " + std::to_string(pid) + " has ended)");
#endif
    return doc["url"].str();
}

inline std::string read_secret(const std::string &folder) {
    std::string secret;
    try {
        secret = read_file(folder + "/local.secret");
    } catch (const std::exception &) {
        throw PairingRefused("there is no readable local.secret in " + folder + ": Bentrix writes it at every start");
    }
    while (!secret.empty() && std::isspace((unsigned char)secret.back())) secret.pop_back();
    while (!secret.empty() && std::isspace((unsigned char)secret.front())) secret.erase(0, 1);
    if (secret.empty()) throw PairingRefused("local.secret is empty");
    return secret;
}

/// The name this process pairs under: BENTRIX_AGENT_NAME, else the SDK's own.
inline std::string default_name() {
    const char *env = std::getenv("BENTRIX_AGENT_NAME");
    return env && *env ? std::string(env) : std::string(CLIENT_NAME);
}

/// Present the secret to url/pair and answer the token. The secret travels in this one body.
inline std::string pair(const std::string &url, const std::string &secret, const std::string &name) {
    Json body(Object{{"secret", Json(secret)}, {"client", Json(Object{{"name", Json(name)}})}});
    std::string base = url;
    while (!base.empty() && base.back() == '/') base.pop_back();
    Response r = detail::post(base + "/pair", body.dump(), {{"Content-Type", "application/json"}});
    Json doc;
    try {
        doc = Json::parse(r.body);
    } catch (const std::exception &) {
    }
    if (r.status < 200 || r.status > 299) {
        std::string code = doc["error"]["code"].is_string() ? doc["error"]["code"].str() : "";
        if (code == "wrong_secret") throw PairingRefused("Bentrix refused the secret in the secret file: it writes a new secret at every start");
        throw PairingRefused("Bentrix answered the pairing request with HTTP " + std::to_string(r.status) + " " + code + ": " + (doc["error"]["message"].is_string() ? doc["error"]["message"].str() : ""));
    }
    if (!doc["token"].is_string() || doc["token"].str().empty()) throw PairingRefused("Bentrix answered the pairing request with no token");
    return doc["token"].str();
}

/// Find the running application and get a token: {url, token}. Empty arguments take the defaults.
inline std::pair<std::string, std::string> discover(const std::string &agents_folder = "", const std::string &name = "") {
    std::string folder = agents_folder.empty() ? agents_dir() : agents_folder;
    std::string url = read_endpoint(folder);
    return {url, pair(url, read_secret(folder), name.empty() ? default_name() : name)};
}

}  // namespace bentrix::mcp
