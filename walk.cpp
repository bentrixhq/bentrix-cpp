// The C++ binding's walk on the CI tape: the engine (a second session refused, thirty hours advanced, a
// market order filled, the book read, a refusal in its words) and the Gym episode every binding runs,
// printed as JSON for sdk/python/tests/test_crosslang.py.
//
//   c++ -std=c++17 -I crates/bentrix-ffi/include sdk/cpp/walk.cpp -L <dir> -lbentrix -o walk
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "bentrix.hpp"

using bentrix::Error;
using bentrix::Json;
using bentrix::Local;

static void check(bool held, const std::string &what) {
    if (!held) throw Error("FAILED: " + what);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: walk <empty directory>\n");
        return 2;
    }
    try {
        std::printf("bentrix %s\n", Local::version().c_str());
        auto [ws, store] = Local::make_tape(argv[1]);
        Local local(ws, store);
        bool refused = false;
        try {
            Local again(ws, store);
        } catch (const Error &e) {
            refused = true;
            std::printf("a second session refused: %s\n", e.what());
        }
        check(refused, "a second session on the held workspace was not refused");
        std::string bench = local.call("get_state")["laboratories"][0]["benches"][0]["uri"].str();
        local.arm(bench);
        auto from = int64_t(local.call("replay_step", "{\"bench\":\"" + bench + "\",\"unit\":\"tick\",\"n\":1}")["cursor_ms"].num());
        auto reached = local.advance(bench, from + 30LL * 3600000);
        check(reached >= from + 30LL * 3600000, "the workbench did not advance");
        local.call("order_market", "{\"bench\":\"" + bench + "\",\"symbol\":\"XAUUSD\",\"side\":\"buy\",\"volume\":0.01}");
        reached = local.advance(bench, reached + 60000);
        std::string home, owner, name;
        {
            std::string rest = bench.substr(std::string("bentrix://").size());
            auto a = rest.find('/'), b = rest.find('/', a + 1), c = rest.find('/', b + 1);
            home = rest.substr(0, a), owner = rest.substr(a + 1, b - a - 1), name = rest.substr(c + 1);
        }
        Json book = local.read("trade_book", "{\"home\":\"" + home + "\",\"owner\":\"" + owner + "\",\"bench\":\"" + name + "\"}");
        check(book["positions"].arr().size() == 1 && book["positions"][0]["side"].str() == "buy", "the order did not fill");
        // A value read off an answer that is not kept - the loop a person writes first: the answer is a temporary, and
        // what is read from it must not be a reference into it.
        size_t held = 0;
        for (const auto &p : local.read("trade_book", "{\"home\":\"" + home + "\",\"owner\":\"" + owner + "\",\"bench\":\"" + name + "\"}")["positions"].arr()) held += p["ticket"].num() > 0 ? 1 : 0;
        check(held == 1, "the positions of an answer not kept");
        std::string said;
        try {
            local.call("no_such_tool");
        } catch (const Error &e) {
            said = e.what();
        }
        check(said.find("no_such_tool") != std::string::npos, "a refusal did not say its tool");
        check(Local::interval_ms("H1") == 3600000, "H1 is not an hour");

        std::vector<double> rewards;
        std::vector<std::string> hashes;
        long long start_ms = 0;
        {
            bentrix::TradingEnv env(local, bench, "\"interval\":\"H1\",\"window\":8,\"episode\":\"window\",\"episode_bars\":12");
            Json started = env.reset(5);
            check(started["obs"]["market"].arr().size() == 8, "the observation is not shaped as declared");
            start_ms = (long long)started["info"]["start_ms"].num();
            hashes.push_back(started["info"]["ledger_hash"].str());
            for (int a : {1, 1, 2, 0, 2, 2, 1, 0, 1, 2, 0, 1}) {
                Json s = env.step(std::to_string(a));
                rewards.push_back(s["reward"].num());
                hashes.push_back(s["info"]["ledger_hash"].str());
                if (s["terminated"].boolean() || s["truncated"].boolean()) break;
            }
            // The next episode of the same seed: a reset with no seed, as every binding has it. It starts elsewhere.
            Json next = env.reset();
            check((long long)next["info"]["start_ms"].num() != start_ms, "a reset with no seed started the first episode again");
        }
        check(std::set<std::string>(hashes.begin(), hashes.end()).size() > 1, "the episode traded nothing");
        // Every number written whole: a reward at 17 significant digits reads back as the same double.
        std::printf("{\"start_ms\": %lld, \"rewards\": [", start_ms);
        for (size_t i = 0; i < rewards.size(); i++) std::printf("%s%.17g", i ? ", " : "", rewards[i]);
        std::printf("], \"hashes\": [");
        for (size_t i = 0; i < hashes.size(); i++) std::printf("%s\"%s\"", i ? ", " : "", hashes[i].c_str());
        std::printf("]}\nPASS\n");
        return 0;
    } catch (const Error &e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
