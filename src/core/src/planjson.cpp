#include "planjson.h"

#include "jsontext.h"
#include "sqlite.h"

#include <limits>

namespace cutefish::store::detail {

namespace {

void appendString(std::string& out, std::string_view text)
{
    detail::appendJsonString(out, text);
}

void appendKey(std::string& out, std::string_view key)
{
    appendString(out, key);
    out += ':';
}

void appendVersion(std::string& out, const std::optional<Version>& v)
{
    if (v)
        appendString(out, v->str());
    else
        out += "null";
}

void appendList(std::string& out, std::span<const std::string> list)
{
    out += '[';
    for (std::size_t i = 0; i < list.size(); ++i) {
        if (i)
            out += ',';
        appendString(out, list[i]);
    }
    out += ']';
}

const char* boolean(bool b) { return b ? "true" : "false"; }

Error invalid(std::string_view what) { return Error::invalidArgument("stored plan: " + std::string(what)); }

Result<std::vector<std::string>> readList(Database& db, std::string_view json, std::string_view what)
{
    auto stmt = Statement::prepare(db, "SELECT type, value FROM json_each(?1) ORDER BY CAST(key AS INTEGER)");
    if (!stmt)
        return std::move(stmt).error();
    stmt->bind(1, json);
    std::vector<std::string> out;
    int rc;
    while ((rc = stmt->step()) == SQLITE_ROW) {
        if (stmt->columnText(0) != "text")
            return invalid(std::string(what) + " must hold strings");
        out.emplace_back(stmt->columnText(1));
    }
    if (rc != SQLITE_DONE)
        return invalid(std::string(what) + " is not an array");
    return out;
}

Result<std::optional<Version>> readVersion(const Statement& s, int typeColumn, int valueColumn, BackendKind backend)
{
    const std::string_view type = s.columnText(typeColumn);
    if (type == "null" || type.empty())
        return std::optional<Version>();
    if (type != "text")
        return invalid("a version must be a string");
    const std::string_view text = s.columnText(valueColumn);
    auto v = backend == BackendKind::PackageKit ? Version::debian(text) : Version::upstream(text);
    if (!v)
        return std::move(v).error();
    return std::optional<Version>(std::move(v).value());
}

} // namespace

std::string planToJson(const Plan& plan)
{
    std::string out;
    out.reserve(256 + plan.items().size() * 256);
    out += "{";
    appendKey(out, "v");
    out += std::to_string(kPlanJsonVersion);
    out += ',';
    appendKey(out, "kind");
    appendString(out, toString(plan.kind()));
    out += ',';
    appendKey(out, "backend");
    appendString(out, toString(plan.backend()));
    out += ',';
    appendKey(out, "scope");
    appendString(out, toString(plan.scope()));
    out += ',';
    appendKey(out, "installedSizeChange");
    out += std::to_string(plan.installedSizeChange());
    out += ',';
    appendKey(out, "items");
    out += '[';
    bool first = true;
    for (const PlanItem& item : plan.items()) {
        if (!first)
            out += ',';
        first = false;
        out += '{';
        appendKey(out, "operation");
        appendString(out, toString(item.operation()));
        out += ',';
        appendKey(out, "kind");
        appendString(out, toString(item.kind()));
        out += ',';
        appendKey(out, "requested");
        out += boolean(item.requested());
        out += ',';
        appendKey(out, "package");
        appendString(out, item.package());
        out += ',';
        appendKey(out, "fromVersion");
        appendVersion(out, item.fromVersion());
        out += ',';
        appendKey(out, "toVersion");
        appendVersion(out, item.toVersion());
        out += ',';
        appendKey(out, "origin");
        appendString(out, item.origin());
        out += ',';
        appendKey(out, "revision");
        appendString(out, item.revision());
        out += ',';
        appendKey(out, "downloadSize");
        // Sizes stay far below 2^63; JSON integers in SQLite are signed.
        out += std::to_string(item.downloadSize());
        out += ',';
        appendKey(out, "permissions");
        appendList(out, item.permissions());
        out += ',';
        appendKey(out, "oldPermissions");
        appendList(out, item.oldPermissions());
        out += ',';
        appendKey(out, "addsSource");
        out += boolean(item.addsSource());
        out += ',';
        appendKey(out, "rebaseFrom");
        appendString(out, item.rebaseFrom());
        out += ',';
        appendKey(out, "trusted");
        out += boolean(item.trusted());
        out += '}';
    }
    out += "]}";
    return out;
}

Result<Plan> planFromJson(Database& db, std::string_view json)
{
    auto head = Statement::prepare(db, "SELECT json_valid(?1), json_extract(?1, '$.v'), json_extract(?1, '$.kind'),"
                                       " json_extract(?1, '$.backend'), json_extract(?1, '$.scope'),"
                                       " json_type(?1, '$.installedSizeChange'), json_extract(?1, '$.installedSizeChange'),"
                                       " json_type(?1, '$.items')");
    if (!head)
        return std::move(head).error();
    head->bind(1, json);
    if (head->step() != SQLITE_ROW || head->columnInt(0) != 1)
        return invalid("not JSON");
    if (head->columnIsNull(1) || head->columnInt(1) != kPlanJsonVersion)
        return invalid("unknown format version");
    const auto kind = parseTransactionKind(head->columnText(2));
    const auto backend = parseBackendKind(head->columnText(3));
    const auto scope = parseScope(head->columnText(4));
    if (!kind || !backend || !scope)
        return invalid("bad kind, backend or scope");
    if (head->columnText(5) != "integer")
        return invalid("installedSizeChange must be an integer");
    PlanSpec spec{.kind = *kind, .backend = *backend, .scope = *scope, .items = {},
                  .installedSizeChange = head->columnInt(6)};
    if (head->columnText(7) != "array")
        return invalid("items must be an array");

    auto items = Statement::prepare(
            db, "SELECT json_extract(value, '$.operation'), json_extract(value, '$.kind'),"
                " json_type(value, '$.requested'), json_extract(value, '$.package'),"
                " json_type(value, '$.fromVersion'), json_extract(value, '$.fromVersion'),"
                " json_type(value, '$.toVersion'), json_extract(value, '$.toVersion'),"
                " json_extract(value, '$.origin'), json_extract(value, '$.revision'),"
                " json_type(value, '$.downloadSize'), json_extract(value, '$.downloadSize'),"
                " json_extract(value, '$.permissions'), json_extract(value, '$.oldPermissions'),"
                " json_type(value, '$.addsSource'), json_extract(value, '$.rebaseFrom'),"
                " json_type(value, '$.trusted'),"
                " json_type(value, '$.origin'), json_type(value, '$.revision'), json_type(value, '$.rebaseFrom'),"
                " json_type(value, '$.package'), json_type(value, '$.permissions'), json_type(value, '$.oldPermissions')"
                " FROM json_each(?1, '$.items') ORDER BY CAST(key AS INTEGER)");
    if (!items)
        return std::move(items).error();
    items->bind(1, json);
    auto boolOf = [&](int column) -> std::optional<bool> {
        const std::string_view t = items->columnText(column);
        if (t == "true")
            return true;
        if (t == "false")
            return false;
        return std::nullopt;
    };
    int rc;
    while ((rc = items->step()) == SQLITE_ROW) {
        const auto operation = parsePlanOperation(items->columnText(0));
        const auto itemKind = parsePlanItemKind(items->columnText(1));
        const auto requested = boolOf(2);
        const auto addsSource = boolOf(14);
        const auto trusted = boolOf(16);
        if (!operation || !itemKind || !requested || !addsSource || !trusted)
            return invalid("bad item operation, kind or flag");
        for (const int c : {17, 18, 19, 20})
            if (items->columnText(c) != "text")
                return invalid("item strings are missing");
        if (items->columnText(21) != "array" || items->columnText(22) != "array")
            return invalid("item permissions must be arrays");
        if (items->columnText(10) != "integer" || items->columnInt(11) < 0)
            return invalid("downloadSize must be a non-negative integer");

        PlanItemSpec item{.operation = *operation,
                          .kind = *itemKind,
                          .requested = *requested,
                          .package = std::string(items->columnText(3))};
        auto from = readVersion(*items, 4, 5, spec.backend);
        if (!from)
            return std::move(from).error();
        auto to = readVersion(*items, 6, 7, spec.backend);
        if (!to)
            return std::move(to).error();
        item.fromVersion = std::move(from).value();
        item.toVersion = std::move(to).value();
        item.origin = std::string(items->columnText(8));
        item.revision = std::string(items->columnText(9));
        item.downloadSize = static_cast<std::uint64_t>(items->columnInt(11));
        const std::string permissions(items->columnText(12));
        const std::string oldPermissions(items->columnText(13));
        item.addsSource = *addsSource;
        item.rebaseFrom = std::string(items->columnText(15));
        item.trusted = *trusted;

        auto p = readList(db, permissions, "permissions");
        if (!p)
            return std::move(p).error();
        auto o = readList(db, oldPermissions, "oldPermissions");
        if (!o)
            return std::move(o).error();
        item.permissions = std::move(p).value();
        item.oldPermissions = std::move(o).value();
        spec.items.push_back(std::move(item));
    }
    if (rc != SQLITE_DONE)
        return db.error("stored plan items", ErrorCode::InvalidArgument);
    return Plan::create(std::move(spec));
}

} // namespace cutefish::store::detail
