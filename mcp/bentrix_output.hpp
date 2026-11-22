// bentrix_output.hpp - a model's output of any kind on the chart: the output_spec document built by hand for
// the eleven kinds beside a path (a path is bentrix_forecast.hpp's) - scalar, classes, quantiles, rank, position,
// weights, event, levels, action, text, series - checked as Bentrix checks it, a value outside its declared range
// refused in words and never clamped, and put through output_put; and the person's meaning (output_meaning) set
// through output_meaning_set, never in a model's file. An output never names its arrival: Bentrix stamps put_ms.
//
// A caller is anything with `Json call(const std::string &, const Json &)`: bentrix::mcp::Client, or a test's.
#pragma once

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "bentrix_mcp.hpp"

namespace bentrix {
namespace mcp {
namespace output {

/// The twelve kinds, as the document writes them.
inline const std::vector<std::string> KINDS = {"path", "scalar", "classes", "quantiles", "rank", "position", "weights", "event", "levels", "action", "text", "series"};
/// The meaning's closed words.
inline const std::vector<std::string> MEANING_WORDS = {"buy", "sell", "nothing", "up", "down", "flat", "in", "out", "close"};
/// An action's intents.
inline const std::vector<std::string> INTENTS = {"buy", "sell", "close_buys", "close_sells", "nothing"};

/// A refusal in Bentrix's words, raised before anything is sent.
struct Refused : Error {
    explicit Refused(const std::string &m) : Error(m) {}
};

using Range = std::optional<std::pair<double, double>>;
using Named = std::map<std::string, double>;

namespace detail {
inline bool one_of(const std::vector<std::string> &xs, const std::string &x) { return std::find(xs.begin(), xs.end(), x) != xs.end(); }
inline std::string fmt(double v) { return Json::number(v); }
inline bool label(const std::string &s) {
    if (s.empty() || s.size() > 32) return false;
    for (char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}
inline size_t chars(const std::string &s) {
    size_t n = 0;
    for (unsigned char c : s) n += (c & 0xC0) != 0x80;
    return n;
}
inline void in_range(const std::string &field, const Json &n, Range range) {
    if (!n.is_number() || !std::isfinite(n.num())) throw Refused(field + " " + n.dump() + " is not a number");
    double v = n.num();
    if (range && (v < range->first || v > range->second))
        throw Refused(field + " the output answers " + fmt(range->first) + " to " + fmt(range->second) + "; " + fmt(v) + " is outside it");
}
inline Json named(const Named &m) {
    Object o;
    for (const auto &[k, v] : m) o[k] = v;
    return Json(o);
}
}  // namespace detail

/// The output_meaning document: what a value means to the person.
struct Meaning {
    std::string reads = "direction";
    std::optional<std::string> high_means, low_means;
    std::optional<double> act_above, act_below;
    double hysteresis = 0;
    std::map<std::string, std::string> labels;

    /// high above act_above, low below act_below.
    static Meaning of(const std::string &high, std::optional<std::string> low = std::nullopt, std::optional<double> above = std::nullopt, std::optional<double> below = std::nullopt) {
        Meaning m;
        m.high_means = high;
        m.low_means = std::move(low);
        m.act_above = above;
        m.act_below = below;
        return m;
    }

    /// Refuses a word that is not the product's, a threshold outside range, a label empty or over 24.
    const Meaning &check(Range range) const {
        for (const auto *w : {&high_means, &low_means})
            if (*w && !detail::one_of(MEANING_WORDS, **w)) throw Refused("\"" + **w + "\" is not one of the meaning's words");
        if (range)
            for (const auto *t : {&act_above, &act_below})
                if (*t && (**t < range->first || **t > range->second))
                    throw Refused("the output answers " + detail::fmt(range->first) + " to " + detail::fmt(range->second) + "; " + detail::fmt(**t) + " is outside it");
        for (const auto &[k, l] : labels)
            if (!detail::one_of(MEANING_WORDS, k) || l.empty() || detail::chars(l) > 24)
                throw Refused("the label \"" + l + "\" for " + k + " is not a meaning word's, or is empty or longer than 24");
        return *this;
    }

    /// The document.
    Json doc() const {
        Object d{{"reads", reads}, {"hysteresis", hysteresis}};
        if (high_means) d["high_means"] = *high_means;
        if (low_means) d["low_means"] = *low_means;
        if (act_above) d["act_above"] = *act_above;
        if (act_below) d["act_below"] = *act_below;
        if (!labels.empty()) {
            Object l;
            for (const auto &[k, v] : labels) l[k] = v;
            d["labels"] = Json(l);
        }
        return Json(d);
    }
};

/// One output's document: its kind, its values as the document writes them, and the kind's own fields.
struct Output {
    std::string kind, model, name, symbol;
    int64_t anchor_ms = 0;
    Array values;
    /// range, units, horizon, names, levels, universe, markets, long_only, exclusive, softmax, persistent, gross, note.
    Object fields;
    int epoch = 0;
    std::optional<Meaning> suggested_meaning;

    static Output make(std::string kind, std::string model, std::string name, std::string symbol, int64_t anchor_ms) {
        Output o;
        o.kind = std::move(kind);
        o.model = std::move(model);
        o.name = std::move(name);
        o.symbol = std::move(symbol);
        o.anchor_ms = anchor_ms;
        return o;
    }
    Output &with(const std::string &k, Json v) {
        fields[k] = std::move(v);
        return *this;
    }
    Output &value(Object v) {
        values.push_back(Json(std::move(v)));
        return *this;
    }

    /// A number per step; range its declared bounds, if any.
    static Output scalar(const std::string &model, const std::string &name, const std::string &symbol, int64_t at, Range range, const std::vector<double> &vs) {
        Output o = make("scalar", model, name, symbol, at);
        for (double v : vs) o.value({{"value", v}});
        if (range) o.with("range", Json(Array{Json(range->first), Json(range->second)}));
        return o;
    }
    /// Probabilities over named classes, one map per step.
    static Output classes(const std::string &model, const std::string &name, const std::string &symbol, int64_t at, const std::vector<std::string> &names, bool exclusive, const std::vector<Named> &steps) {
        Output o = make("classes", model, name, symbol, at);
        o.with("names", Json::list(names)).with("exclusive", exclusive);
        for (const auto &p : steps) o.value({{"p", detail::named(p)}});
        return o;
    }
    /// Quantiles at levels, one map per step keyed as the document writes a level ("0.1").
    static Output quantiles(const std::string &model, const std::string &name, const std::string &symbol, int64_t at, const std::vector<double> &levels, const std::vector<Named> &steps) {
        Output o = make("quantiles", model, name, symbol, at);
        o.with("levels", Json::list(levels));
        for (const auto &q : steps) o.value({{"q", detail::named(q)}});
        return o;
    }
    /// A rank in 0..1 among universe markets.
    static Output rank(const std::string &model, const std::string &name, const std::string &symbol, int64_t at, int universe, double v) {
        Output o = make("rank", model, name, symbol, at);
        o.with("universe", universe).value({{"value", v}});
        return o;
    }
    /// A target position in units (fraction, lots, ...).
    static Output position(const std::string &model, const std::string &name, const std::string &symbol, int64_t at, const std::string &units, double target) {
        Output o = make("position", model, name, symbol, at);
        o.with("units", units).value({{"target", target}});
        return o;
    }
    /// Weights across markets.
    static Output weights(const std::string &model, const std::string &name, const std::string &symbol, int64_t at, bool long_only, const Named &w) {
        Output o = make("weights", model, name, symbol, at);
        std::vector<std::string> markets;
        for (const auto &kv : w) markets.push_back(kv.first);
        o.with("long_only", long_only).with("markets", Json::list(markets)).value({{"w", detail::named(w)}});
        return o;
    }
    /// An event's score in 0..1, expected within within_steps (0 for none).
    static Output event(const std::string &model, const std::string &name, const std::string &symbol, int64_t at, double score, int within_steps = 0) {
        Output o = make("event", model, name, symbol, at);
        Object v{{"score", score}};
        if (within_steps > 0) v["within_steps"] = within_steps;
        o.value(v);
        return o;
    }
    /// Named price levels (stop, target, ...).
    static Output levels(const std::string &model, const std::string &name, const std::string &symbol, int64_t at, const Named &levels) {
        Output o = make("levels", model, name, symbol, at);
        o.value({{"levels", detail::named(levels)}});
        return o;
    }
    /// An intent, with a size where it has one.
    static Output action(const std::string &model, const std::string &name, const std::string &symbol, int64_t at, const std::string &intent, std::optional<double> size = std::nullopt) {
        Output o = make("action", model, name, symbol, at);
        Object v{{"intent", intent}};
        if (size) v["size"] = *size;
        o.value(v);
        return o;
    }
    /// A sentence.
    static Output text(const std::string &model, const std::string &name, const std::string &symbol, int64_t at, const std::string &t) {
        Output o = make("text", model, name, symbol, at);
        o.value({{"text", t}});
        return o;
    }
    /// Named lines, one map per step.
    static Output series(const std::string &model, const std::string &name, const std::string &symbol, int64_t at, const std::vector<std::string> &names, const std::vector<Named> &steps) {
        Output o = make("series", model, name, symbol, at);
        o.with("names", Json::list(names));
        for (const auto &l : steps) o.value({{"lines", detail::named(l)}});
        return o;
    }

    /// The horizon: bars of the anchor's interval.
    Output &steps(int n) { return with("horizon", Json(Object{{"steps", n}})); }
    /// The horizon in business days.
    Output &business_days(int n) { return with("horizon", Json(Object{{"business_days", n}})); }
    /// Held until "levels" or "next_output".
    Output &until(const std::string &what) { return with("horizon", Json(Object{{"until", what}})); }

    Range range() const {
        auto it = fields.find("range");
        if (it == fields.end() || !it->second.is_array() || it->second.arr().size() != 2) return std::nullopt;
        return std::make_pair(it->second[0].num(), it->second[1].num());
    }

    /// Refuses what Bentrix would refuse, in its words.
    const Output &check() const {
        if (!detail::label(model) || !detail::label(name)) throw Refused("the model and the output are [a-z0-9_]{1,32}");
        if (!detail::one_of(KINDS, kind)) throw Refused(kind + " is not one of the twelve kinds");
        if (values.empty()) throw Refused("values holds no value");
        Range r = range();
        std::vector<std::string> names;
        if (auto it = fields.find("names"); it != fields.end() && it->second.is_array())
            for (const auto &n : it->second.arr()) names.push_back(n.str());
        auto sm = fields.find("softmax");
        bool softmax = sm != fields.end() && sm->second == Json(true);
        for (size_t i = 0; i < values.size(); i++) {
            const Json &v = values[i];
            std::string f = "values[" + std::to_string(i) + "]";
            if (kind == "scalar") detail::in_range(f + ".value", v["value"], r);
            else if (kind == "rank") detail::in_range(f + ".value", v["value"], r ? r : Range(std::make_pair(0.0, 1.0)));
            else if (kind == "event") detail::in_range(f + ".score", v["score"], std::make_pair(0.0, 1.0));
            else if (kind == "position") detail::in_range(f + ".target", v["target"], r);
            else if (kind == "classes") {
                if (names.empty()) throw Refused("names a classes output names its classes");
                for (const auto &[k, p] : v["p"].obj()) {
                    if (!detail::one_of(names, k)) throw Refused(f + ".p " + k + " is not one of the classes");
                    if (!softmax) detail::in_range(f + ".p." + k, p, std::make_pair(0.0, 1.0));
                }
            } else if (kind == "series" && names.empty()) throw Refused("names a series output names its lines");
            else if (kind == "action") {
                std::string intent = v["intent"].is_string() ? v["intent"].str() : "";
                if (!detail::one_of(INTENTS, intent)) throw Refused(f + ".intent " + intent + " is not one of buy, sell, close_buys, close_sells, nothing");
            } else if (kind == "text" && v["text"].is_string() && detail::chars(v["text"].str()) > 2000)
                throw Refused(f + ".text is longer than 2000 characters");
        }
        if (suggested_meaning) suggested_meaning->check(r);
        return *this;
    }

    /// The output_spec document; never an arrival.
    Json spec() const {
        Object d{{"schema_version", 1}, {"model", model}, {"output", name}, {"symbol", symbol}, {"kind", kind},
                 {"anchor", Json(Object{{"time_ms", static_cast<long long>(anchor_ms)}})}, {"values", Json(values)}};
        for (const auto &[k, v] : fields) d[k] = v;
        if (epoch > 0) d["epoch"] = epoch;
        if (suggested_meaning) d["suggested_meaning"] = suggested_meaning->doc();
        return Json(d);
    }

    /// output_put on the workbench bench, after check.
    template <class C>
    Json put(C &c, const std::string &bench) const {
        check();
        return c.call("output_put", Json(Object{{"bench", bench}, {"output", spec()}}));
    }
};

/// output_put_many: several outputs at one instant.
template <class C>
Json put_outputs(C &c, const std::string &bench, const std::vector<Output> &outputs, bool all_or_nothing = false) {
    Array specs;
    for (const auto &o : outputs) specs.push_back(o.check().spec());
    return c.call("output_put_many", Json(Object{{"bench", bench}, {"outputs", Json(specs)}, {"all_or_nothing", all_or_nothing}}));
}

/// output_meaning_set: the person's meaning of model's output on the workbench bench.
template <class C>
Json set_meaning(C &c, const std::string &bench, const std::string &model, const std::string &out, const Meaning &m, Range range) {
    m.check(range);
    return c.call("output_meaning_set", Json(Object{{"bench", bench}, {"model", model}, {"output", out}, {"meaning", m.doc()}}));
}

}  // namespace output
}  // namespace mcp
}  // namespace bentrix
