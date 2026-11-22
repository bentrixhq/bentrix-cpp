// output_test.cpp - model outputs of every kind and the person's meaning: every kind builds a document; a
// value outside its declared range is refused in words before anything is sent; three examples are three
// meanings; a put never sends an arrival.
//
//   g++ -std=c++17 -I sdk/cpp/mcp sdk/cpp/mcp/output_test.cpp -o /tmp/output_test && /tmp/output_test
//
// Mutation (2026-11-11), red: in_range not comparing - a 1.3 for a 0..1 output was sent.
#include <cstdio>
#include <functional>
#include <set>

#include "bentrix_output.hpp"

using namespace bentrix::mcp;
using namespace bentrix::mcp::output;

static int failed = 0;
#define CHECK(c, what)                          \
    do {                                        \
        if (!(c)) {                             \
            std::printf("FAIL: %s\n", what);    \
            failed++;                           \
        }                                       \
    } while (0)

struct Rec {
    std::vector<std::string> names;
    std::vector<Json> args;
    Json call(const std::string &name, const Json &a) {
        names.push_back(name);
        args.push_back(a);
        return Json(Object{{"ok", true}});
    }
};

static bool refused(const std::function<void()> &f, const std::string &needle = "") {
    try {
        f();
        return false;
    } catch (const Refused &e) {
        return needle.empty() || std::string(e.what()).find(needle) != std::string::npos;
    }
}

int main() {
    Range unit = std::make_pair(0.0, 1.0);
    std::vector<Output> built = {
        Output::scalar("m1", "o1", "EURUSD", 1, unit, {0.7}),
        Output::classes("m1", "o1", "EURUSD", 1, {"up", "down"}, true, {{{"up", 0.6}, {"down", 0.4}}}),
        Output::quantiles("m1", "o1", "EURUSD", 1, {0.1, 0.9}, {{{"0.1", 1.0}, {"0.9", 1.2}}}),
        Output::rank("m1", "o1", "AAPL", 1, 4871, 0.62),
        Output::position("m1", "o1", "EURUSD", 1, "fraction", -0.5),
        Output::weights("m1", "o1", "EURUSD", 1, true, {{"EURUSD", 0.6}, {"GBPUSD", 0.4}}),
        Output::event("m1", "o1", "EURUSD", 1, 0.8, 3),
        Output::levels("m1", "o1", "EURUSD", 1, {{"stop", 1.09}, {"target", 1.12}}),
        Output::action("m1", "o1", "EURUSD", 1, "buy", 0.5),
        Output::text("m1", "o1", "EURUSD", 1, "the trend held"),
        Output::series("m1", "o1", "EURUSD", 1, {"a"}, {{{"a", 1.0}}}),
    };
    std::set<std::string> kinds{"path"};
    for (const auto &o : built) {
        o.check();
        CHECK(o.spec()["kind"].str() == o.kind, "a builder wrote another kind");
        kinds.insert(o.kind);
    }
    CHECK(KINDS.size() == 12 && kinds.size() == 12, "the builders do not cover the twelve kinds");

    Rec r;
    CHECK(refused([&] { Output::scalar("m1", "o1", "EURUSD", 1, unit, {1.3}).put(r, "bench"); }, "0 to 1; 1.3 is outside it"), "a 1.3 for a 0..1 output was sent");
    CHECK(r.names.empty(), "something was sent");
    CHECK(refused([&] { Output::rank("m1", "o1", "AAPL", 1, 10, 1.5).check(); }), "a rank of 1.5 was taken");

    Rec m;
    for (const auto &mm : {Meaning::of("buy", "sell", 0.6, 0.4), Meaning::of("buy", "nothing", 0.6), Meaning::of("sell", "nothing", 0.6)})
        set_meaning(m, "bench", "m1", "o1", mm, unit);
    CHECK(m.names[2] == "output_meaning_set" && m.args[2]["meaning"]["high_means"].str() == "sell", "the meaning was not sent as set");
    CHECK(refused([&] { set_meaning(m, "bench", "m1", "o1", Meaning::of("profit"), unit); }), "profit was taken as a meaning's word");
    CHECK(refused([&] { set_meaning(m, "bench", "m1", "o1", Meaning::of("buy", std::nullopt, 1.5), unit); }), "a threshold outside the range was taken");

    Rec p;
    Output::scalar("m1", "dir", "EURUSD", 1, unit, {0.7}).steps(4).put(p, "bench");
    put_outputs(p, "bench", {Output::event("m1", "e", "EURUSD", 1, 0.9)}, true);
    const Json &spec = p.args[0]["output"];
    CHECK(p.names[0] == "output_put" && spec["horizon"]["steps"].num() == 4, "output_put sent the wrong horizon");
    CHECK(!spec.has("put_ms"), "an arrival was sent");
    CHECK(p.names[1] == "output_put_many" && p.args[1]["all_or_nothing"] == Json(true), "output_put_many sent the wrong flag");

    std::printf(failed == 0 ? "output_test: all passed\n" : "output_test: %d failed\n", failed);
    return failed == 0 ? 0 : 1;
}
