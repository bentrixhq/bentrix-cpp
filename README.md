# Bentrix SDK for C++

The C++ client of [Bentrix](https://bentrix.net) — charts, a replay laboratory and trading through your own MetaTrader — for a program or an agent.

It finds the Bentrix running on this computer through the agents folder Bentrix writes at every start, pairs with it under the name you give — the name you see in ☰ › Settings › **Agents**, where the pairing's permissions are set — and calls every tool of Bentrix's capability catalogue over MCP. Nothing is typed: no address, no key.

The client is generated from the catalogue of the Bentrix release it came with, and checks when it connects that the running Bentrix serves the same major version.

## What you need

A C++17 compiler, and Bentrix open on the same computer.

## Install

The client is header-only: `mcp/bentrix_mcp.hpp` and `mcp/bentrix_mcp_generated.hpp`. Include the generated one and compile:

```bash
c++ -std=c++17 -I bentrix-cpp/mcp my_agent.cpp -o my_agent        # on Windows add -lws2_32
```

## Connect

```cpp
#include "bentrix_mcp_generated.hpp"
using namespace bentrix::mcp;

int main() {
    auto b = Bentrix::connect("my-agent");
    Json state = b.workspace.get_state(WorkspaceApi::GetStateArgs());
    std::string bench = state["laboratories"][0]["benches"][0]["uri"].str();
}
```

## The engine in your own process

`bentrix.hpp` and the C example in `c/` (the in-process engine) use Bentrix's engine library, which is not part of this repository.

## Documentation

Every method, its arguments, the permission it needs and what it answers: **https://bentrix.net/en/docs/sdk/cpp/**

## Licence

MIT — see [LICENSE](LICENSE). The licence covers this source code; Bentrix itself is not covered by it.
