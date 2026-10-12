#pragma once

#include <string>

#include "dinodb/store.hpp"

namespace dinodb::persistence {

// DinoDB's own simple length-prefixed binary snapshot format (unrelated to
// the Redis RDB format). See src/persistence.cpp for the layout.
bool save(Store& store, const std::string& path);

enum class LoadResult { Ok, Missing, Corrupt };

// All-or-nothing: on Corrupt the store is left untouched, so the caller can
// refuse to start instead of later autosaving over the damaged file.
LoadResult load(Store& store, const std::string& path);

}  // namespace dinodb::persistence
