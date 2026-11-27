// The engine in this process as a workbench object's door: the same Bench over Bentrix's engine as over a running
// Bentrix. It needs the engine's own header and library (sdk/cpp/bentrix.hpp, the C ABI's bentrix.h, -lbentrix).
//
//   bentrix::Local local(workspace, store);
//   auto bench = bentrix::mcp::Bench::open(bentrix::mcp::engine_door(local));
#pragma once

#include "../bentrix.hpp"
#include "bentrix_bench.hpp"

namespace bentrix::mcp {

/// `local`'s tools as a door: each answer read with the client's JSON, as the connection reads its own.
inline Door engine_door(bentrix::Local &local) {
    return [&local](const std::string &name, const Json &args) { return Json::parse(local.call_text(name, args.dump())); };
}

}  // namespace bentrix::mcp
