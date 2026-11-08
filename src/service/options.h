#pragma once

// Command line and paths of cutefish-store-service. Defaults follow the
// XDG directories (ARCHITECTURE §2); every path can be given on the command
// line, which is how the D-Bus tests run the service in a sandbox.

#include <cutefish/store/result.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace cutefish::store::service {

struct ServiceOptions
{
    /// Holds catalog.sqlite and state.sqlite.
    std::filesystem::path dataDir;
    /// The sources to index while the backends do not provide them
    /// (sourcesfile.h).
    std::filesystem::path sourcesFile;
    /// cutefish-store-indexer.
    std::filesystem::path indexer;
    /// Inactivity before the service stops (P-010: 5 minutes).
    std::chrono::milliseconds idleTimeout = std::chrono::minutes(5);
    /// How long a finished transaction stays on the bus (ARCHITECTURE §5.3).
    std::chrono::milliseconds transactionRetention = std::chrono::minutes(5);
    /// Languages the texts are read in, most preferred first (session).
    std::vector<std::string> readLocales;
    /// Description languages given to the indexer (D-013).
    std::vector<std::string> indexLocales = {"fr"};
    std::string busName = "org.cutefish.Store";
};

/// Options:
///   --data-dir DIR        default $XDG_STATE_HOME/cutefish-store
///   --sources FILE        default $XDG_CONFIG_HOME/cutefish-store/sources.conf
///   --indexer PATH        default the installed indexer
///   --idle-timeout-ms N   default 300000
///   --retention-ms N      default 300000
///   --locale L            reading language, repeatable; default from the
///                         environment (LANGUAGE, LC_ALL, LC_MESSAGES, LANG)
/// InvalidArgument for anything else.
Result<ServiceOptions> parseArguments(const std::vector<std::string>& arguments);

/// The reading languages of the environment, most preferred first.
std::vector<std::string> environmentLocales();

} // namespace cutefish::store::service
