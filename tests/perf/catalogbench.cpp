// store-catalog-bench: latency of SqliteCatalog on a real catalogue.
//
//   store-catalog-bench CATALOG [--queries N] [--readers N] [--locale L]
//
// Query sets are drawn from the catalogue's own names with a fixed seed,
// like prototype 3: exact names, prefixes of 2 to 5 characters (typing),
// common words, 4-character substrings, and names with one substituted
// letter (typos). Each query is new to the catalogue, so the page cache of
// SqliteCatalog never answers in its place. Not a unit test: it needs a
// real catalogue (scripts/measure-catalog.sh).

#include <cutefish/store/sqlitecatalog.h>

#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace cutefish::store;
using Clock = std::chrono::steady_clock;

namespace {

double ms(Clock::time_point a, Clock::time_point b)
{
    return std::chrono::duration<double, std::milli>(b - a).count();
}

struct Stats
{
    double p50, p95, p99, max;
};

Stats stats(std::vector<double> v)
{
    std::sort(v.begin(), v.end());
    auto at = [&](double q) { return v[std::min(v.size() - 1, static_cast<std::size_t>(q * static_cast<double>(v.size() - 1) + 0.5))]; };
    return {at(0.5), at(0.95), at(0.99), v.back()};
}

std::vector<std::pair<std::string, std::string>> names(const char* path)
{
    std::vector<std::pair<std::string, std::string>> out;
    sqlite3* db = nullptr;
    const std::string uri = std::string("file:") + path + "?mode=ro";
    sqlite3_open_v2(uri.c_str(), &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, nullptr);
    sqlite3_stmt* s = nullptr;
    sqlite3_prepare_v2(db,
                       "SELECT a.app_id, min(t.value) FROM texts t JOIN offers o ON o.id = t.offer JOIN apps a ON a.id = o.app"
                       " WHERE t.field = 0 AND t.locale = 'C' GROUP BY a.id ORDER BY a.app_id",
                       -1, &s, nullptr);
    while (sqlite3_step(s) == SQLITE_ROW) {
        std::string name = reinterpret_cast<const char*>(sqlite3_column_text(s, 1));
        const bool ascii = std::all_of(name.begin(), name.end(), [](char c) { return c > 0x20 && c < 0x7F; });
        if (ascii && name.size() >= 5)
            out.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(s, 0)), std::move(name));
    }
    sqlite3_finalize(s);
    sqlite3_close(db);
    return out;
}

struct Query
{
    std::string text;
    std::string expectedId; // empty: no expectation
};

void run(const SqliteCatalog& c, const char* label, const std::vector<Query>& queries)
{
    std::vector<double> times;
    std::size_t nonEmpty = 0;
    std::size_t expectedFound = 0;
    std::size_t expectedTotal = 0;
    for (const Query& q : queries) {
        auto query = SearchQuery::create(q.text);
        if (!query)
            continue;
        const auto t0 = Clock::now();
        auto page = c.search(*query);
        times.push_back(ms(t0, Clock::now()));
        if (!page)
            continue;
        nonEmpty += page->total > 0;
        if (!q.expectedId.empty()) {
            ++expectedTotal;
            for (std::size_t i = 0; i < std::min<std::size_t>(10, page->items.size()); ++i)
                if (page->items[i].id.str() == q.expectedId)
                    ++expectedFound;
        }
    }
    const Stats s = stats(times);
    std::printf("search %-10s queries=%zu p50_ms=%.3f p95_ms=%.3f p99_ms=%.3f max_ms=%.3f nonempty=%zu "
                "expected_in_top10=%zu/%zu\n",
                label, times.size(), s.p50, s.p95, s.p99, s.max, nonEmpty, expectedFound, expectedTotal);
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: store-catalog-bench CATALOG [--queries N] [--readers N] [--locale L]\n");
        return 2;
    }
    const char* path = argv[1];
    std::size_t count = 500;
    int readers = 4;
    CatalogReadOptions options;
    for (int i = 2; i + 1 < argc; i += 2) {
        const std::string arg = argv[i];
        if (arg == "--queries")
            count = std::stoul(argv[i + 1]);
        else if (arg == "--readers")
            readers = std::stoi(argv[i + 1]);
        else if (arg == "--locale")
            options.locales.push_back(argv[i + 1]);
    }

    const auto t0 = Clock::now();
    auto opened = SqliteCatalog::open(path, options);
    const auto t1 = Clock::now();
    if (!opened) {
        std::fprintf(stderr, "%s\n", opened.error().message().c_str());
        return 1;
    }
    const SqliteCatalog& c = *opened;
    std::printf("open_ms=%.3f generation=%llu\n", ms(t0, t1), static_cast<unsigned long long>(c.generation()));

    auto all = names(path);
    std::mt19937 rng(20261107);
    std::shuffle(all.begin(), all.end(), rng);
    if (all.size() > count)
        all.resize(count);

    std::vector<Query> exact, prefix, substring, typo, words;
    std::uniform_int_distribution<int> letter('a', 'z');
    for (const auto& [id, name] : all) {
        exact.push_back({name, id});
        prefix.push_back({name.substr(0, 2 + rng() % 4), id});
        substring.push_back({name.substr(name.size() / 2 - 2, 4), id});
        std::string t = name;
        std::size_t pos = 1 + rng() % (t.size() - 1);
        while (pos < t.size() && !std::isalpha(static_cast<unsigned char>(t[pos])))
            ++pos;
        if (pos < t.size()) {
            char replacement = static_cast<char>(letter(rng));
            while (replacement == std::tolower(static_cast<unsigned char>(t[pos])))
                replacement = static_cast<char>(letter(rng));
            t[pos] = replacement;
        }
        typo.push_back({t, id});
    }
    for (const char* w : {"editor", "éditeur", "photo", "image", "music", "musique", "video", "vidéo", "game", "jeu",
                          "browser", "navigateur", "terminal", "text", "calculator", "paint", "email", "pdf", "chat",
                          "weather", "météo", "office", "password", "backup", "map", "podcast", "ebook", "code", "git",
                          "database", "virtual machine", "scanner", "printer"})
        words.push_back({w, {}});

    run(c, "exact", exact);
    run(c, "prefix", prefix);
    run(c, "words", words);
    run(c, "substring", substring);
    run(c, "typo", typo);

    // Turning pages: the same query again, next page (the ranked list of the
    // previous search is reused).
    {
        std::vector<double> pages;
        for (const Query& q : prefix) {
            auto first = SearchQuery::create(q.text);
            auto next = SearchQuery::create(q.text, {.offset = 60});
            (void) c.search(*first);
            const auto a = Clock::now();
            (void) c.search(*next);
            pages.push_back(ms(a, Clock::now()));
        }
        const Stats s = stats(pages);
        std::printf("next-page queries=%zu p50_ms=%.3f p95_ms=%.3f p99_ms=%.3f\n", pages.size(), s.p50, s.p95, s.p99);
    }

    // Application with all its offers, then the summaries of a page.
    std::vector<double> details;
    std::size_t offers = 0;
    for (const auto& [id, name] : all) {
        auto appId = AppId::parse(id);
        const auto a = Clock::now();
        auto app = c.application(*appId);
        details.push_back(ms(a, Clock::now()));
        if (app)
            offers += app->offers().size();
    }
    const Stats d = stats(details);
    std::printf("application apps=%zu offers=%zu p50_ms=%.3f p95_ms=%.3f p99_ms=%.3f max_ms=%.3f\n", details.size(),
                offers, d.p50, d.p95, d.p99, d.max);
    std::vector<AppId> page;
    for (std::size_t i = 0; i < std::min<std::size_t>(60, all.size()); ++i)
        page.push_back(*AppId::parse(all[i].first));
    std::vector<double> pageTimes;
    for (int i = 0; i < 50; ++i) {
        const auto a = Clock::now();
        auto r = c.summaries(page);
        pageTimes.push_back(ms(a, Clock::now()));
    }
    const Stats p = stats(pageTimes);
    std::printf("summaries ids=60 p50_ms=%.3f p95_ms=%.3f max_ms=%.3f\n", p.p50, p.p95, p.max);

    // Several readers, one connection each (ARCHITECTURE §8.4).
    std::atomic<int> errors{0};
    std::vector<std::vector<double>> perThread(static_cast<std::size_t>(readers));
    const auto r0 = Clock::now();
    std::vector<std::thread> threads;
    for (int t = 0; t < readers; ++t) {
        threads.emplace_back([&, t] {
            auto own = SqliteCatalog::open(path, options);
            if (!own) {
                ++errors;
                return;
            }
            for (std::size_t i = 0; i < prefix.size(); ++i) {
                const Query& q = prefix[(i + static_cast<std::size_t>(t) * 37) % prefix.size()];
                const auto a = Clock::now();
                if (!own->search(*SearchQuery::create(q.text)))
                    ++errors;
                perThread[static_cast<std::size_t>(t)].push_back(ms(a, Clock::now()));
            }
        });
    }
    for (std::thread& t : threads)
        t.join();
    std::vector<double> merged;
    for (auto& v : perThread)
        merged.insert(merged.end(), v.begin(), v.end());
    const Stats m = stats(merged);
    std::printf("readers=%d searches=%zu wall_ms=%.1f p50_ms=%.3f p95_ms=%.3f p99_ms=%.3f errors=%d\n", readers,
                merged.size(), ms(r0, Clock::now()), m.p50, m.p95, m.p99, errors.load());
    return errors ? 1 : 0;
}
