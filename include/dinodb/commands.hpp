#pragma once

#include <string>

#include "dinodb/resp.hpp"
#include "dinodb/store.hpp"

namespace dinodb {

class Server;  // fwd decl, some commands (SAVE) need server-level config

// Executes one parsed request against the store and returns a fully
// RESP-encoded reply (including trailing \r\n).
std::string dispatch(Store& store, Server& server, const resp::Request& req);

}  // namespace dinodb
