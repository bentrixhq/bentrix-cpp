// bentrix.hpp - Bentrix for C++17: the engine in your own process through its C ABI (bentrix.h), with
// RAII, exceptions in Bentrix's own words, a small dependency-free JSON value, and the Gym as a client
// of Bentrix's own environment (env_make, env_reset, env_step) - the one every language reaches.
//
//   #include "bentrix.hpp"     // with crates/bentrix-ffi/include on the include path
//   link: -lbentrix
#pragma once

#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "bentrix.h"

namespace bentrix {

/// A refusal or a failure, in Bentrix's own words.
struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

inline std::string take(char *p) {
    if (!p) throw Error(bx_last_error());
    std::string s(p);
    bx_free(p);
    return s;
}

inline void done(int status) {
    if (status != 0) throw Error(bx_last_error());
}

// ---- a small JSON value -----------------------------------------------------------------------------

struct Json;
using Array = std::vector<Json>;
using Object = std::map<std::string, Json>;

/// A JSON value: null, bool, number (double), string, array or object.
struct Json {
    std::variant<std::nullptr_t, bool, double, std::string, Array, Object> v = nullptr;

    bool is_null() const { return std::holds_alternative<std::nullptr_t>(v); }
    double num() const { return std::get<double>(v); }
    bool boolean() const { return std::get<bool>(v); }
    // A value read off a temporary is the value, never a reference into it: `for (const auto &p :
    // local.call(...)["positions"].arr())` reads an answer nobody kept, so on a kept Json these hand a reference and
    // on a temporary what was there, moved out - nothing dangles.
    const std::string &str() const & { return std::get<std::string>(v); }
    std::string str() && { return std::move(std::get<std::string>(v)); }
    const Array &arr() const & { return std::get<Array>(v); }
    Array arr() && { return std::move(std::get<Array>(v)); }
    const Object &obj() const & { return std::get<Object>(v); }
    Object obj() && { return std::move(std::get<Object>(v)); }
    const Json &operator[](const std::string &k) const & {
        static const Json none;
        auto &o = obj();
        auto it = o.find(k);
        return it == o.end() ? none : it->second;
    }
    Json operator[](const std::string &k) && { return static_cast<const Json &>(*this)[k]; }
    const Json &operator[](size_t i) const & { return arr().at(i); }
    Json operator[](size_t i) && { return static_cast<const Json &>(*this)[i]; }

    static Json parse(const std::string &text) {
        size_t i = 0;
        Json out = value(text, i);
        space(text, i);
        if (i != text.size()) throw Error("JSON: trailing text");
        return out;
    }

  private:
    static void space(const std::string &s, size_t &i) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\n' || s[i] == '\r' || s[i] == '\t')) i++;
    }
    static std::string string_at(const std::string &s, size_t &i) {
        std::string out;
        i++;  // the opening quote
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
                    if (cp < 0x80) out += char(cp);
                    else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
                    else if (cp < 0x10000) { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
                    else { out += char(0xF0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 0x3F)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
                    break;
                }
                default: out += e;
            }
        }
        i++;  // the closing quote
        return out;
    }
    static Json value(const std::string &s, size_t &i) {
        space(s, i);
        if (i >= s.size()) throw Error("JSON: unexpected end");
        Json j;
        char c = s[i];
        if (c == '{') {
            Object o;
            i++;
            space(s, i);
            if (s[i] == '}') { i++; j.v = o; return j; }
            for (;;) {
                space(s, i);
                std::string k = string_at(s, i);
                space(s, i);
                i++;  // ':'
                o[k] = value(s, i);
                space(s, i);
                if (s[i] == ',') { i++; continue; }
                i++;  // '}'
                break;
            }
            j.v = std::move(o);
        } else if (c == '[') {
            Array a;
            i++;
            space(s, i);
            if (s[i] == ']') { i++; j.v = a; return j; }
            for (;;) {
                a.push_back(value(s, i));
                space(s, i);
                if (s[i] == ',') { i++; continue; }
                i++;  // ']'
                break;
            }
            j.v = std::move(a);
        } else if (c == '"') {
            j.v = string_at(s, i);
        } else if (s.compare(i, 4, "true") == 0) {
            i += 4;
            j.v = true;
        } else if (s.compare(i, 5, "false") == 0) {
            i += 5;
            j.v = false;
        } else if (s.compare(i, 4, "null") == 0) {
            i += 4;
        } else {
            size_t used = 0;
            j.v = std::stod(s.substr(i, 40), &used);
            i += used;
        }
        return j;
    }
};

// ---- the engine -------------------------------------------------------------------------------------

/// An embedded Bentrix over a workspace and its recordings; closed when it goes out of scope.
class Local {
  public:
    /// Open `workspace` (yours when empty) and its recordings (yours when empty).
    explicit Local(const std::string &workspace = "", const std::string &store = "")
        : s_((refuse_another_interface(), bx_open(workspace.empty() ? nullptr : workspace.c_str(), store.empty() ? nullptr : store.c_str(), "C++")), bx_close) {
        if (!s_) throw Error(bx_last_error());
    }

    /// **The engine's interface this package speaks** (`bx_abi`): a library
    /// of another one is refused before `bx_open`, in a sentence naming both. A library from before `bx_abi`
    /// (0.1.382) does not link with this header at all.
    static constexpr std::uint32_t abi = BX_ABI;
    static void refuse_another_interface() {
        const std::uint32_t have = bx_abi();
        if (have == abi) return;
        throw Error("Bentrix's engine library speaks interface " + std::to_string(have) + " (Bentrix " + bx_version() +
                    ") and this package speaks " + std::to_string(abi) + ": " + (have < abi ? "update Bentrix" : "update this package") + ".");
    }

    /// One tool by name, its arguments as JSON text (empty: none); its answer.
    Json call(const std::string &tool, const std::string &args_json = "") { return Json::parse(call_text(tool, args_json)); }
    /// `call`'s answer as the text the engine wrote: what a client reads with its own JSON (the workbench object's
    /// engine door, `mcp/bentrix_engine_door.hpp`).
    std::string call_text(const std::string &tool, const std::string &args_json = "") {
        return take(bx_call(s_.get(), tool.c_str(), args_json.empty() ? nullptr : args_json.c_str()));
    }
    /// One resource by its catalogue name, its template's variables as JSON text.
    Json read(const std::string &resource, const std::string &vars_json = "") {
        return Json::parse(take(bx_read(s_.get(), resource.c_str(), vars_json.empty() ? nullptr : vars_json.c_str())));
    }
    void arm(const std::string &bench, size_t max_frames = 20000) { done(bx_arm(s_.get(), bench.c_str(), max_frames)); }
    /// Advance a workbench to `to_ms` on its clock, as Play would; where it stands.
    int64_t advance(const std::string &bench, int64_t to_ms) {
        int64_t reached = 0;
        done(bx_advance(s_.get(), bench.c_str(), to_ms, &reached));
        return reached;
    }
    void settle(size_t max_frames = 1000) { done(bx_settle(s_.get(), max_frames)); }

    static std::string version() { return bx_version(); }
    static int64_t interval_ms(const std::string &label) { return bx_interval_ms(label.c_str()); }
    /// The CI tape in `directory`: {workspace, store}.
    static std::pair<std::string, std::string> make_tape(const std::string &directory) {
        char *ws = nullptr, *store = nullptr;
        done(bx_make_tape(directory.c_str(), &ws, &store));
        return {take(ws), take(store)};
    }

  private:
    std::unique_ptr<BxSession, void (*)(BxSession *)> s_;
};

/// The Gym (Gymnasium's shape): reset(seed) -> {obs, info}; step(action) -> {obs, reward, terminated,
/// truncated, info}, each as the environment answered it.
class TradingEnv {
  public:
    /// `options_json`: env_make's arguments beside the bench, e.g. "\"window\":8,\"episode_bars\":12".
    TradingEnv(Local &local, const std::string &bench, const std::string &options_json = "") : local_(local) {
        std::string args = "{\"bench\":\"" + bench + "\"" + (options_json.empty() ? "" : "," + options_json) + "}";
        Json made = local_.call("env_make", args);
        id_ = made["env"].str();
    }
    ~TradingEnv() {
        try {
            local_.call("env_close", "{\"env\":\"" + id_ + "\"}");
        } catch (...) {
        }
    }
    /// A new episode from `seed`: the draws start over.
    Json reset(long long seed) { return local_.call("env_reset", "{\"env\":\"" + id_ + "\",\"seed\":" + std::to_string(seed) + "}"); }
    /// The next episode of the last seed.
    Json reset() { return local_.call("env_reset", "{\"env\":\"" + id_ + "\"}"); }
    /// `action_json`: 0, 1 or 2; a number; or an object.
    Json step(const std::string &action_json) { return local_.call("env_step", "{\"env\":\"" + id_ + "\",\"action\":" + action_json + "}"); }
    const std::string &id() const { return id_; }

  private:
    Local &local_;
    std::string id_;
};

}  // namespace bentrix
