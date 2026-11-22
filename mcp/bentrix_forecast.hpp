// bentrix_forecast.hpp - a model's forecast on the chart, built by hand: the forecast_spec document step by
// step - a time form (an interval) or an element form (a chart built by price: renko, kagi, line_break,
// point_figure, range_bars - no interval, its rule in form.elements) - put through forecast_put; and an
// evaluation's sheet waited on: forecast_eval_make's answer is the run at once, or an ask the person answers in
// Bentrix's window, watched on the bentrix://forecasts/eval event. A forecast never names its arrival: Bentrix
// stamps put_ms.
//
// A caller is anything with `Json call(const std::string &, const Json &)`: bentrix::mcp::Client, or a test's.
#pragma once

#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "bentrix_mcp.hpp"

namespace bentrix {
namespace mcp {
namespace forecast {

inline const std::string EVAL_EVENT = "bentrix://forecasts/eval";

/// One bar of a forecast; everything but close left out when empty.
struct Step {
    double close = 0;
    std::optional<double> open, high, low, volume;
    std::vector<std::tuple<double, double, std::optional<double>>> bands;  // (lo, hi, p)
    std::vector<std::pair<std::string, double>> quantiles;                 // by level, "0.05"
    std::vector<double> samples;
    /// When the step is expected - on every step or none; what places an element form on a chart by time.
    std::optional<int64_t> expected_ms;

    Json value() const {
        Object s{{"close", close}};
        if (open) s["open"] = *open;
        if (high) s["high"] = *high;
        if (low) s["low"] = *low;
        if (volume) s["volume"] = *volume;
        if (expected_ms) s["expected_ms"] = static_cast<long long>(*expected_ms);
        if (!bands.empty()) {
            Array a;
            for (const auto &[lo, hi, p] : bands) {
                Object b{{"lo", lo}, {"hi", hi}};
                if (p) b["p"] = *p;
                a.push_back(Json(b));
            }
            s["bands"] = Json(a);
        }
        if (!quantiles.empty()) {
            Object q;
            for (const auto &[k, v] : quantiles) q[k] = v;
            s["quantiles"] = Json(q);
        }
        if (!samples.empty()) s["samples"] = Json::list(samples);
        return Json(s);
    }
};

/// The forecast_spec document. anchor_ms is the open of the last closed bar (or the moment of the last closed
/// element) it is made at; step 1 is the one after it.
struct Forecast {
    std::string model, symbol, chart_type = "candlestick";
    std::optional<std::string> timeframe;  // a time form's interval; empty for an element form
    std::optional<Json> elements;          // an element form's rule, where it is not the chart's own
    int64_t anchor_ms = 0;
    int epoch = 0;
    std::optional<std::string> note;
    std::optional<Json> look;
    std::vector<Step> steps;

    /// A forecast of timeframe's bars, drawn as candles.
    static Forecast of_time(std::string model, std::string symbol, std::string timeframe, int64_t anchor_ms) {
        Forecast f;
        f.model = std::move(model);
        f.symbol = std::move(symbol);
        f.timeframe = std::move(timeframe);
        f.anchor_ms = anchor_ms;
        return f;
    }

    /// A forecast of a chart built by price: no interval, elements its rule (none for the chart's own).
    static Forecast of_elements(std::string model, std::string symbol, std::string chart_type, int64_t anchor_ms, std::optional<Json> elements = std::nullopt) {
        Forecast f;
        f.model = std::move(model);
        f.symbol = std::move(symbol);
        f.chart_type = std::move(chart_type);
        f.anchor_ms = anchor_ms;
        f.elements = std::move(elements);
        return f;
    }

    Forecast &step(Step s) {
        steps.push_back(std::move(s));
        return *this;
    }

    Json spec() const {
        Object form{{"chart_type", chart_type}};
        if (timeframe) form["timeframe"] = *timeframe;
        if (elements) form["elements"] = *elements;
        Array st;
        for (const auto &s : steps) st.push_back(s.value());
        Object d{{"schema_version", 1}, {"model", model}, {"symbol", symbol}, {"form", Json(form)},
                 {"anchor", Json(Object{{"time_ms", static_cast<long long>(anchor_ms)}})}, {"steps", Json(st)}, {"epoch", epoch}};
        if (note) d["note"] = *note;
        if (look) d["look"] = *look;
        return Json(d);
    }

    /// forecast_put on the workbench bench: its arrival, its state, where it stands on each chart.
    template <class C>
    Json put(C &c, const std::string &bench) const {
        return c.call("forecast_put", Json(Object{{"bench", bench}, {"forecast", spec()}}));
    }
};

/// forecast_put_many: several markets and models at one instant.
template <class C>
Json put_many(C &c, const std::string &bench, const std::vector<Forecast> &fs, bool all_or_nothing = false) {
    Array specs;
    for (const auto &f : fs) specs.push_back(f.spec());
    return c.call("forecast_put_many", Json(Object{{"bench", bench}, {"forecasts", Json(specs)}, {"all_or_nothing", all_or_nothing}}));
}

/// The person's No on the evaluation's sheet. Not a failure: nothing was made or fetched.
struct DeclinedByThePerson : Error {
    std::string ask;
    explicit DeclinedByThePerson(const std::string &a) : Error("the person declined " + a), ask(a) {}
};

/// A laboratory not made over the span, or a run that did not start on it, with Bentrix's why.
struct EvalFailed : Error {
    std::string ask, why;
    EvalFailed(const std::string &a, const std::string &w) : Error(w), ask(a), why(w) {}
};

/// The run to drive, from forecast_eval_make's answer: its eval at once, or - over days the workbench's laboratory
/// does not hold - the person's answer on the sheet, watched on EVAL_EVENT: started answers the run's id; declined
/// throws DeclinedByThePerson; failed throws EvalFailed. No tool answers the sheet. idle_limit waits in a row with no
/// move of this ask end the watch with an Error (never, when 0).
template <class C>
int64_t eval_started(C &c, const Json &made, int64_t timeout_ms = 60000, int idle_limit = 0) {
    if (made["eval"].is_number()) return static_cast<int64_t>(made["eval"].num());
    const std::string ask = made["ask"].is_string() ? made["ask"].str() : "";
    std::string uri = made["watch"].is_string() ? made["watch"].str() : EVAL_EVENT;
    std::string cursor = "0";
    if (auto i = uri.find("after="); i != std::string::npos) cursor = uri.substr(i + 6, uri.find('&', i) == std::string::npos ? std::string::npos : uri.find('&', i) - i - 6);
    uri = uri.substr(0, std::min(uri.find('{'), uri.find('?')));
    for (int idle = 0; idle_limit == 0 || idle < idle_limit;) {
        Json page = c.call("wait_for", Json(Object{{"uri", uri}, {"after", cursor}, {"timeout_ms", static_cast<long long>(timeout_ms)}}));
        if (page["next"].is_string()) cursor = page["next"].str();
        bool moved = false;
        if (page["items"].is_array()) {
            for (const auto &item : page["items"].arr()) {
                if (!item["ask"].is_string() || item["ask"].str() != ask) continue;
                moved = true;
                const std::string state = item["state"].is_string() ? item["state"].str() : "";
                if (state == "started") return static_cast<int64_t>(item["eval"].num());
                if (state == "declined") throw DeclinedByThePerson(ask);
                if (state == "failed") throw EvalFailed(ask, item["why"].is_string() ? item["why"].str() : "the evaluation did not start");
            }
        }
        idle = moved ? 0 : idle + 1;
    }
    throw Error(ask + ": no answer on the sheet");
}

}  // namespace forecast
}  // namespace mcp
}  // namespace bentrix
