// The C++ MCP client's walk against a running BENTRIX (bentrix --serve over the CI tape): it finds the
// application and pairs with nothing typed, reads its state through the generated surface, reads a
// resource by its template, and runs the same Gym episode every binding runs - through the agents'
// tools, over the wire - printed as JSON for sdk/python/tests/test_crosslang.py.
//
//   c++ -std=c++17 sdk/cpp/mcp/walk.cpp -o walk     (Windows: -lws2_32)
//   LOCALAPPDATA=<the serve's> ./walk
#include <cstdio>
#include <set>

#include "bentrix_mcp_generated.hpp"

using namespace bentrix::mcp;

static void check(bool held, const std::string &what) {
    if (!held) throw Error("FAILED: " + what);
}

int main() {
    try {
        auto b = Bentrix::connect("cpp-walk");
        Json state = b.workspace.get_state(WorkspaceApi::GetStateArgs());
        std::string bench = state["laboratories"][0]["benches"][0]["uri"].str();
        std::vector<std::string> p;
        {
            std::string rest = bench.substr(std::string("bentrix://").size()), part;
            std::istringstream in(rest);
            while (std::getline(in, part, '/')) p.push_back(part);
        }
        Json book = b.trade.book(TradeApi::BookArgs(p[0], p[1], p[3]));
        check(book["positions"].is_array(), "the book did not read: " + book.dump());
        std::string said;
        try {
            b.call("no_such_tool");
        } catch (const Error &e) {
            said = e.what();
        }
        check(said.find("no_such_tool") != std::string::npos, "a refusal did not say its tool: " + said);

        Json made = b.strategy.env_make(StrategyApi::EnvMakeArgs(bench).interval("H1").window(8).episode("window").episode_bars(12));
        std::string env = made["env"].str();
        Json start = b.strategy.env_reset(StrategyApi::EnvResetArgs(env).seed(5));
        check(start["obs"]["market"].arr().size() == 8, "the observation is not shaped as declared");
        Array rewards;
        Array hashes{start["info"]["ledger_hash"]};
        for (int a : {1, 1, 2, 0, 2, 2, 1, 0, 1, 2, 0, 1}) {
            Json s = b.strategy.env_step(StrategyApi::EnvStepArgs(env, Json(a)));
            rewards.push_back(Json(s["reward"].num()));
            hashes.push_back(s["info"]["ledger_hash"]);
            if (s["terminated"].boolean() || s["truncated"].boolean()) break;
        }
        b.strategy.env_close(StrategyApi::EnvCloseArgs(env));
        // A tool's own refusal (isError), not a request the wire refused.
        std::string refusal;
        try {
            b.strategy.env_step(StrategyApi::EnvStepArgs(env, Json(0)));
        } catch (const ToolError &e) {
            refusal = e.what();
        }
        check(refusal.find("No environment") != std::string::npos, "a closed environment's step was not refused in its words: " + refusal);
        std::set<std::string> distinct;
        for (const auto &h : hashes) distinct.insert(h.str());
        check(distinct.size() > 1, "the episode traded nothing");
        Json episode(Object{{"start_ms", Json(int64_t(start["info"]["start_ms"].num()))}, {"rewards", Json(rewards)}, {"hashes", Json(hashes)}});
        // A reward is written in the shortest of 15 or 17 digits that reads back as the same double.
        std::printf("%s\nPASS\n", episode.dump().c_str());
        return 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
