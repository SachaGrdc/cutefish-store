#pragma once

// Plans as JSON in state.sqlite. Internal to store-core: written here, read
// back with SQLite's JSON functions, then validated again by Plan::create.
// No JSON library is added to the core.

#include <cutefish/store/plan.h>
#include <cutefish/store/result.h>

#include <string>
#include <string_view>

namespace cutefish::store::detail {

class Database;

/// Format version written in "v". Readers refuse any other value. Optional
/// keys added later (the plan's `downloadSize`, D-029) keep the version:
/// records without them still read the same.
inline constexpr int kPlanJsonVersion = 1;

std::string planToJson(const Plan& plan);
/// Parses with the given connection (SQLite JSON functions). Any missing
/// field, wrong type or invalid plan is an InvalidArgument error.
Result<Plan> planFromJson(Database& db, std::string_view json);

} // namespace cutefish::store::detail
