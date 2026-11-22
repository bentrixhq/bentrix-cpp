// forecast_test.cpp - a forecast's builder and an evaluation's sheet.
//
//   g++ -std=c++17 -I sdk/cpp/mcp sdk/cpp/mcp/forecast_test.cpp -o /tmp/forecast_test && /tmp/forecast_test
//
// Mutations (2026-11-08), each red: spec writing the timeframe whatever it is (the element form named an interval);
// declined read as a failure (a decline was EvalFailed).
#include <cstdio>
#include <deque>

#include "bentrix_forecast.hpp"

using namespace bentrix::mcp;
using namespace bentrix::mcp::forecast;

static int failed = 0;
#define CHECK(c, what)                          \
    do {                                        \
        if (!(c)) {                             \
            std::printf("FAIL: %s\n", what);    \
            failed++;                           \
        }                                       \
    } while (0)

struct Sheet {
    std::deque<Array> pages;
    std::vector<std::string> asked;
    Json call(const std::string &name, const Json &args) {
        if (name != "wait_for" || args["uri"].str() != EVAL_EVENT) throw Error("not the eval event");
        std::string after = args["after"].str();
        asked.push_back(after);
        Array items;
        if (!pages.empty()) {
            items = pages.front();
            pages.pop_front();
        }
        return Json(Object{{"items", Json(items)}, {"next", std::to_string(std::stoi(after) + int(items.size()))}});
    }
};

static Json moved(const std::string &ask, const std::string &state, Object more = {}) {
    more["ask"] = ask;
    more["state"] = state;
    return Json(more);
}

int main() {
    Step s;
    s.close = 1.1005;
    s.expected_ms = 1700000600000LL;
    auto f = Forecast::of_elements("bricks", "EURUSD", "renko", 1700000000000LL, Json(Object{{"renko_box", 0.0005}}));
    f.step(s);
    Json d = f.spec();
    CHECK(!d["form"].has("timeframe") && d["form"]["chart_type"].str() == "renko" && d["form"]["elements"]["renko_box"].num() == 0.0005, "the element form named an interval");
    CHECK(d["steps"][0]["expected_ms"].num() == 1700000600000.0, "a step lost its expected_ms");
    Step one;
    one.close = 1;
    CHECK(Forecast::of_time("m", "EURUSD", "H1", 0).step(one).spec()["form"]["timeframe"].str() == "H1", "the time form named no interval");

    Sheet none;
    CHECK(eval_started(none, Json(Object{{"eval", 7}}), 0, 1) == 7, "a run at once");
    Json ask(Object{{"ask", "bentrix://ask/2"}, {"watch", "bentrix://forecasts/eval?after=0"}});
    Sheet sheet;
    sheet.pages = {Array{moved("bentrix://ask/1", "declined"), moved("bentrix://ask/2", "asked")}, Array{moved("bentrix://ask/2", "made_laboratory")},
                   Array{moved("bentrix://ask/2", "started", Object{{"eval", 11}})}};
    CHECK(eval_started(sheet, ask, 0, 3) == 11, "the run the person started");
    CHECK((sheet.asked == std::vector<std::string>{"0", "2", "3"}), "the cursor");
    Sheet declined;
    declined.pages = {Array{moved("bentrix://ask/2", "declined")}};
    try {
        eval_started(declined, ask, 0, 3);
        CHECK(false, "a decline answered a run");
    } catch (const DeclinedByThePerson &) {
    } catch (const Error &) {
        CHECK(false, "a decline was EvalFailed");
    }
    Sheet bad;
    bad.pages = {Array{moved("bentrix://ask/2", "failed", Object{{"why", "the laboratory was not made"}})}};
    try {
        eval_started(bad, ask, 0, 3);
        CHECK(false, "a failure answered a run");
    } catch (const EvalFailed &e) {
        CHECK(e.why == "the laboratory was not made", "a failure lost its why");
    }
    Sheet quiet;
    try {
        eval_started(quiet, ask, 0, 2);
        CHECK(false, "a watch with no answer did not end");
    } catch (const DeclinedByThePerson &) {
        CHECK(false, "silence was a decline");
    } catch (const Error &) {
    }
    std::printf(failed ? "FAILED %d\n" : "PASS\n", failed);
    return failed ? 1 : 0;
}
