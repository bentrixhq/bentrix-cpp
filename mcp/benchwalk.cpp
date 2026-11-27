// The C++ Bench's walk against a running Bentrix (bentrix --serve over a practice tape): the common tasks - a refusal
// read as an answer, a step, a buy with a stop and a target, a modify, a part closed, close_all, the account and the
// deals - through bentrix_bench.hpp, the layer generated from the Bench table. What it saw is printed as JSON for
// sdk/python/tests/test_bench.py, which holds it to the engine's own walk over the same tape.
//
//   c++ -std=c++17 -I<bentrix-ffi/include> sdk/cpp/mcp/benchwalk.cpp -L<dir> -lbentrix -o benchwalk   (Windows: -lws2_32)
//   LOCALAPPDATA=<the serve's> ./benchwalk                  (a running Bentrix)
//   BENTRIX_PRACTICE=<a practice folder> ./benchwalk        (the engine in this process)
#include <cmath>
#include <cstdio>

#include <cstdlib>
#include <memory>

#include "bentrix_bench.hpp"
#include "bentrix_engine_door.hpp"
#include "bentrix_mcp_generated.hpp"

using namespace bentrix::mcp;

static void check(bool held, const std::string &what) {
    if (!held) throw Error("FAILED: " + what);
}

static bool has(const Json &v, const std::string &needle) { return v.is_string() && v.str().find(needle) != std::string::npos; }

int main() {
    try {
        // One walk, either door: the engine in this process over a practice folder, or the running Bentrix.
        const char *practice = std::getenv("BENTRIX_PRACTICE");
        std::unique_ptr<bentrix::Local> local;
        std::unique_ptr<Bentrix> b;
        if (practice && *practice) {
            local = std::make_unique<bentrix::Local>(std::string(practice) + "/ws", std::string(practice) + "/store");
        } else {
            auto [url, token] = discover("", "cpp-bench");
            b = std::make_unique<Bentrix>(url, token);
        }
        Bench bench = local ? Bench::open(engine_door(*local)) : Bench::open(*b);
        auto markets = bench.markets();
        check(markets.size() == 1 && markets[0] == "XAUUSD", "the markets");
        Answer refused = bench.buy(0.001);
        check(!refused.ok() && refused.kind.is_string() && refused.kind.str() == "volume_out_of_range", "the refusal: " + refused.raw.dump());
        const std::string docs = refused.docs.is_string() ? refused.docs.str() : "";
        const std::string tail = "go/concept.volume/";
        check(refused.reason.is_string() && has(refused.remedy, "volume_min") && docs.size() >= tail.size() && docs.compare(docs.size() - tail.size(), tail.size(), tail) == 0, "the refusal's words: " + refused.raw.dump());
        Json first = bench.step(StepOptions().interval("H1"));
        check(!first.is_null() && first["time"].num() > 0 && first["close"].num() > 0, "the first step: " + first.dump());
        Json group = bench.group("walk");
        check(group.is_number() && group.num() == 1, "a group's number: " + group.dump());
        Json closed = bench.bars(BarsOptions().interval("H1").limit(5));
        check(closed.is_array() && closed.arr().size() == 1 && closed[0][0].num() == first["time"].num(), "the closed candles: " + closed.dump());
        Json q = bench.quote();
        check(!q.is_null() && q["ask"].num() > q["bid"].num(), "the quote: " + q.dump());
        double sl = std::nearbyint(q["ask"].num() - 30.0), tp = std::nearbyint(q["ask"].num() + 30.0);
        check(bench.buy(0.1, BuyOptions().sl(sl).tp(tp)).ok(), "the buy");
        bench.step(StepOptions().interval("H1"));
        Json positions = bench.positions();
        check(positions.arr().size() == 1, "the positions: " + positions.dump());
        Json p = positions[0];
        check(p["side"].str() == "buy" && p["volume"].num() == 0.1 && p["sl"].num() == sl && p["tp"].num() == tp, "the position: " + p.dump());
        check(bench.modify(p, ModifyOptions().sl(sl + 5.0)).ok(), "the modify");
        Json moved = bench.positions()[0];
        check(moved["sl"].num() == sl + 5.0 && moved["tp"].num() == tp, "only what was named changes: " + moved.dump());
        check(bench.close(moved, CloseOptions().lots(0.04)).ok(), "the part closed");
        bench.step(StepOptions().interval("H1"));
        Json rest = bench.positions()[0];
        check(rest["volume"].num() == 0.06, "the rest: " + rest.dump());
        check(bench.close_all().ok(), "close_all");
        bench.step(StepOptions().interval("H1"));
        check(bench.positions().arr().empty(), "a position stayed");
        Answer nothing = bench.close_all();
        check(nothing.ok() && nothing.raw["closed"].is_number() && nothing.raw["closed"].num() == 0, "close_all with nothing open: " + nothing.raw.dump());
        Json account = bench.account();
        check(account["equity"].num() == account["balance"].num() && account["balance"].num() != 10000.0, "the account: " + account.dump());
        Json deals = bench.deals();
        int ins = 0, outs = 0;
        Array seen_deals;
        for (const auto &d : deals.arr()) {
            if (d["entry"].str() == "in") ins++;
            if (d["entry"].str() == "out") outs++;
            seen_deals.push_back(Json(Array{d["entry"], d["side"], d["volume"]}));
        }
        check(ins == 1 && outs == 2, "the deals: " + deals.dump());
        // A value read off an answer that is not kept: the loop a person writes first. The answer is a temporary, and
        // what is read from it must not be a reference into it.
        size_t logged = 0;
        for (const auto &event : bench.events().arr()) logged += event["seq"].is_number() ? 1 : 0;
        check(logged >= 6 && bench.account()["currency"].str() == "USD", "the events of an answer not kept");
        Bench kept = bench.keep("kept");
        check(kept.positions().arr().empty(), "the kept workbench reads");
        // Past the days' end, advance stands at their last moment, by either door.
        Json end = bench.advance(Json(int64_t(4102444800000LL)));
        check(end.is_number() && end.num() > 0 && end.num() < 4102444800000.0, "the days' end: " + end.dump());
        if (b) {
            // A workbench resource by its address: the same book as by its three parts.
            std::vector<std::string> p;
            std::istringstream in(bench.uri.substr(std::string("bentrix://").size()));
            for (std::string s; std::getline(in, s, '/');) p.push_back(s);
            std::string by_parts = b->trade.book(TradeApi::BookArgs(p[0], p[1], p[3])).dump();
            check(b->trade.book(TradeApi::BookArgs(bench.uri)).dump() == by_parts, "the book by its address");
        }
        Json seen(Object{{"refused", Json(Array{Json(refused.outcome), refused.kind, refused.reason})}, {"first", first}, {"group", group}, {"bars", Json(Array{closed[0][0]})}, {"deals", Json(seen_deals)}, {"balance", account["balance"]}, {"end", end}});
        std::printf("%s\nPASS\n", seen.dump().c_str());
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
