#pragma once

#include <string>

#include "dinodb/store.hpp"

namespace dinodb::persistence {

// DinoDB's own simple length-prefixed binary snapshot format (unrelated to
// the Redis RDB format). See src/persistence.cpp for the layout.
bool save(Store& store, const std::string& path);
bool load(Store& store, const std::string& path);

}  // namespace dinodb::persistence
