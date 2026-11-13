#include "crud/resource.h"

#include <mutex>
#include <regex>
#include <set>
#include <stdexcept>

#include "core/crypto.h"
#include "core/errors.h"
#include "core/ids.h"
#include "core/json.h"
#include "db/field_crypt.h"

namespace thelife::crud {

namespace {

using drogon::Task;
using tools::ToolCall;

const std::regex kIdent("^[a-z_][a-z0-9_]*$");

std::string quoted(const std::string& ident) {
  if (!std::regex_match(ident, kIdent)) throw std::logic_error("bad identifier " + ident);
  return "\"" + ident + "\"";
}

struct Shape {
  std::vector<std::string> projection;  // output properties
  std::set<std::string> bodyFields;     // writable fields
  Json::Value createDefaults{Json::objectValue};
};

// Column name → SQL type, read once per table from the catalog.
class Columns {
 public:
  Task<const std::map<std::string, std::string>*> get(db::TenantTx& tx, const std::string& table) {
    {
      std::lock_guard lock(mu_);
      if (auto it = cache_.find(table); it != cache_.end()) co_return &it->second;
    }
    const auto rows = co_await tx.exec(
        "SELECT a.attname AS name, format_type(a.atttypid, a.atttypmod) AS type FROM pg_attribute a "
        "WHERE a.attrelid = $1::regclass AND a.attnum > 0 AND NOT a.attisdropped",
        table);
    std::map<std::string, std::string> cols;
    for (const auto& r : rows) cols[r["name"].as<std::string>()] = r["type"].as<std::string>();
    std::lock_guard lock(mu_);
    co_return &cache_.emplace(table, std::move(cols)).first->second;
  }

 private:
  std::mutex mu_;
  std::map<std::string, std::map<std::string, std::string>> cache_;
};

Columns& columns() {
  static Columns c;
  return c;
}

std::string paramText(const Json::Value& v) {
  if (v.isString()) return v.asString();
  return json::compact(v);
}

std::string encodeCursor(const std::string& id) {
  return crypto::base64Url(reinterpret_cast<const unsigned char*>(id.data()), id.size());
}

std::string decodeCursor(const std::string& cursor) {
  // base64url of a UUID string; anything else is rejected.
  static const std::string kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string out;
  int val = 0, bits = -8;
  for (char c : cursor) {
    const auto pos = kAlphabet.find(c);
    if (pos == std::string::npos) throw ApiError::validation({{"cursor", "is not a valid cursor"}});
    val = (val << 6) + static_cast<int>(pos);
    bits += 6;
    if (bits >= 0) {
      out.push_back(static_cast<char>((val >> bits) & 0xFF));
      bits -= 8;
    }
  }
  if (!ids::isUuid(out)) throw ApiError::validation({{"cursor", "is not a valid cursor"}});
  return out;
}

Task<Json::Value> fetchOne(db::TenantTx& tx, const std::string& table, const std::string& id) {
  const auto rows = co_await tx.exec(
      "SELECT to_jsonb(r)::text AS j FROM " + quoted(table) + " r WHERE r.id = $1::uuid AND r.deleted_at IS NULL", id);
  if (rows.empty()) co_return Json::Value();
  auto row = json::parse(rows[0]["j"].as<std::string>());
  co_await fieldcrypt::openRow(tx, table, row);
  co_return row;
}

// After an UPDATE matched nothing: 404 if the row is gone, 412 if only the version differed.
Task<void> explainMiss(db::TenantTx& tx, const std::string& table, const std::string& id) {
  const auto rows = co_await tx.exec(
      "SELECT version FROM " + quoted(table) + " WHERE id = $1::uuid AND deleted_at IS NULL", id);
  if (rows.empty()) throw ApiError::notFound();
  throw ApiError::preconditionFailed("If-Match does not match the current version (" +
                                     std::to_string(rows[0]["version"].as<int>()) + ").");
}

Shape shapeFor(const tools::ToolRegistry& reg, const std::string& prefix) {
  Shape s;
  const auto& tools = reg.manifest().tools;
  auto find = [&](const std::string& op) -> const tools::ToolDef* {
    auto it = tools.find(prefix + "." + op);
    return it == tools.end() ? nullptr : &it->second;
  };
  const tools::ToolDef* get = find("get");
  const tools::ToolDef* create = find("create");
  const Json::Value* out = get ? &get->outputSchema : create ? &create->outputSchema : nullptr;
  if (!out) throw std::logic_error(prefix + ": neither get nor create in the manifest");
  for (const auto& k : (*out)["properties"].getMemberNames()) s.projection.push_back(k);
  if (create && create->rest) {
    for (const auto& f : create->rest->bodyArgs) {
      s.bodyFields.insert(f);
      const auto& prop = create->inputSchema["properties"][f];
      if (prop.isMember("default")) s.createDefaults[f] = prop["default"];
    }
  }
  if (auto* upd = find("update"); upd && upd->rest) {
    for (const auto& f : upd->rest->bodyArgs) s.bodyFields.insert(f);
  }
  return s;
}

void checkColumns(const std::map<std::string, std::string>& cols, const Json::Value& doc, const std::string& table) {
  for (const auto& k : doc.getMemberNames()) {
    if (!cols.count(k)) throw std::logic_error("contract field " + k + " has no column in " + table);
  }
}

}  // namespace

Json::Value toApi(const Json::Value& row, const std::vector<std::string>& projection) {
  Json::Value out(Json::objectValue);
  for (const auto& p : projection) {
    if (p == "created_by") {
      out[p]["kind"] = row["created_by_kind"];
      out[p]["id"] = row["created_by_id"];
    } else if (row.isMember(p)) {
      out[p] = row[p];
    }
  }
  return out;
}

std::vector<std::string> projectionFor(const tools::ToolRegistry& reg, const std::string& prefix) {
  return shapeFor(reg, prefix).projection;
}

tools::TargetResolver targetResolver(const std::string& table) {
  const auto q = quoted(table);
  return [q](db::TenantTx& tx, const std::string& id) -> Task<std::optional<tools::TargetProvenance>> {
    const auto rows = co_await tx.exec(
        "SELECT created_by_kind::text AS k, origin::text AS o, version, created_at > now() - interval '24 hours' AS recent "
        "FROM " + q + " WHERE id = $1::uuid AND deleted_at IS NULL",
        id);
    if (rows.empty()) co_return std::nullopt;
    co_return tools::TargetProvenance{rows[0]["k"].as<std::string>(), rows[0]["o"].as<std::string>(),
                                      rows[0]["version"].as<int>(), rows[0]["recent"].as<bool>()};
  };
}

Task<Json::Value> fetchApi(db::TenantTx& tx, const std::string& table, const std::string& id,
                           const std::vector<std::string>& projection) {
  const auto row = co_await fetchOne(tx, table, id);
  if (row.isNull()) co_return Json::Value();
  co_return toApi(row, projection);
}

namespace {
std::vector<std::pair<std::string, std::string>>& bound() {
  static std::vector<std::pair<std::string, std::string>> v;
  return v;
}
}  // namespace

const std::vector<std::pair<std::string, std::string>>& boundResources() { return bound(); }

void bindCrud(tools::ToolRegistry& reg, ResourceBinding b) {
  bound().emplace_back(b.prefix, b.table);
  auto shape = std::make_shared<Shape>(shapeFor(reg, b.prefix));
  auto binding = std::make_shared<ResourceBinding>(std::move(b));
  const auto& tools = reg.manifest().tools;
  auto has = [&](const std::string& op) { return tools.count(binding->prefix + "." + op) > 0; };
  const std::string table = binding->table;

  const tools::TargetResolver resolver = targetResolver(table);

  if (has("get")) {
    reg.bind(binding->prefix + ".get", [shape, table](ToolCall& c) -> Task<Json::Value> {
      const auto row = co_await fetchOne(c.tx, table, c.args["id"].asString());
      if (row.isNull()) throw ApiError::notFound();
      co_return toApi(row, shape->projection);
    }, resolver);
  }

  if (has("list")) {
    reg.bind(binding->prefix + ".list", [shape, binding, table](ToolCall& c) -> Task<Json::Value> {
      const auto* cols = co_await columns().get(c.tx, table);
      std::string sql = "SELECT to_jsonb(r)::text AS j FROM " + quoted(table) + " r WHERE true";
      std::vector<std::optional<std::string>> params;
      auto next = [&]() { return "$" + std::to_string(params.size()); };
      const auto& a = c.args;
      if (!a.get("include_deleted", false).asBool()) sql += " AND r.deleted_at IS NULL";
      if (a.isMember("updated_since")) {
        params.push_back(a["updated_since"].asString());
        sql += " AND r.updated_at > " + next() + "::timestamptz";
      }
      if (a.isMember("cursor")) {
        params.push_back(decodeCursor(a["cursor"].asString()));
        sql += " AND r.id > " + next() + "::uuid";
      }
      static const std::set<std::string> kCommon{"limit", "cursor", "updated_since", "include_deleted"};
      for (const auto& name : a.getMemberNames()) {
        if (kCommon.count(name)) continue;
        params.push_back(paramText(a[name]));
        if (auto it = binding->filters.find(name); it != binding->filters.end()) {
          auto frag = it->second;
          for (auto pos = frag.find("$?"); pos != std::string::npos; pos = frag.find("$?", pos)) {
            frag.replace(pos, 2, next());
          }
          sql += " AND (" + frag + ")";
        } else if (auto col = cols->find(name); col != cols->end()) {
          sql += " AND r." + quoted(name) + " = " + next() + "::" + col->second;
        } else {
          throw std::logic_error(binding->prefix + ": filter " + name + " has no column or SQL mapping");
        }
      }
      const int limit = a.get("limit", 50).asInt();
      sql += " ORDER BY r.id LIMIT " + std::to_string(limit + 1);
      const auto rows = co_await c.tx.execParams(sql, params);

      Json::Value out;
      out["items"] = Json::Value(Json::arrayValue);
      std::string lastId;
      for (size_t i = 0; i < rows.size() && i < static_cast<size_t>(limit); ++i) {
        auto row = json::parse(rows[i]["j"].as<std::string>());
        co_await fieldcrypt::openRow(c.tx, table, row);
        lastId = row["id"].asString();
        out["items"].append(toApi(row, shape->projection));
      }
      out["next_cursor"] = rows.size() > static_cast<size_t>(limit) ? Json::Value(encodeCursor(lastId)) : Json::Value();
      co_return out;
    });
  }

  if (has("create")) {
    reg.bind(binding->prefix + ".create", [shape, binding, table](ToolCall& c) -> Task<Json::Value> {
      Json::Value fields = shape->createDefaults;
      for (const auto& f : shape->bodyFields) {
        if (c.args.isMember(f)) fields[f] = c.args[f];
      }
      if (binding->validate) binding->validate(fields, true);
      Json::Value doc = fields;
      doc["id"] = ids::newUuidV7();
      doc["tenant_id"] = c.principal.tenantId;
      doc["created_by_kind"] = auth::createdByKind(c.principal.kind);
      doc["created_by_id"] = c.principal.actorId().empty() ? Json::Value() : Json::Value(c.principal.actorId());
      doc["origin"] = c.origin;
      if (c.sourceMessageId) doc["source_message_id"] = *c.sourceMessageId;
      const auto* cols = co_await columns().get(c.tx, table);
      checkColumns(*cols, doc, table);
      co_await fieldcrypt::sealFields(c.tx, table, doc);
      std::string list;
      for (const auto& k : doc.getMemberNames()) list += (list.empty() ? "" : ", ") + quoted(k);
      const auto rows = co_await c.tx.exec("INSERT INTO " + quoted(table) + " AS r (" + list + ") SELECT " + list +
                                               " FROM jsonb_populate_record(NULL::" + quoted(table) +
                                               ", $1::jsonb) RETURNING to_jsonb(r)::text AS j",
                                           json::compact(doc));
      auto row = json::parse(rows[0]["j"].as<std::string>());
      co_await fieldcrypt::openRow(c.tx, table, row);
      co_return toApi(row, shape->projection);
    });
  }

  if (has("update")) {
    reg.bind(binding->prefix + ".update", [shape, binding, table](ToolCall& c) -> Task<Json::Value> {
      Json::Value patch(Json::objectValue);
      for (const auto& f : shape->bodyFields) {
        if (c.args.isMember(f)) patch[f] = c.args[f];
      }
      if (patch.empty()) throw ApiError::validation({{"", "at least one field must be provided"}});
      if (binding->validate) binding->validate(patch, false);
      const auto* cols = co_await columns().get(c.tx, table);
      checkColumns(*cols, patch, table);
      co_await fieldcrypt::sealFields(c.tx, table, patch);
      std::string list;
      for (const auto& k : patch.getMemberNames()) list += (list.empty() ? "" : ", ") + quoted(k);
      const auto id = c.args["id"].asString();
      const std::vector<std::optional<std::string>> params{json::compact(patch), id, db::nullable(c.expectedVersion)};
      const auto rows = co_await c.tx.execParams(
          "UPDATE " + quoted(table) + " AS r SET (" + list + ") = (SELECT " + list + " FROM jsonb_populate_record(NULL::" +
              quoted(table) + ", $1::jsonb)) WHERE r.id = $2::uuid AND r.deleted_at IS NULL "
              "AND ($3::int IS NULL OR r.version = $3::int) RETURNING to_jsonb(r)::text AS j",
          params);
      if (rows.empty()) co_await explainMiss(c.tx, table, id);
      // Cross-field rules that depend on unchanged columns are enforced by table CHECK constraints.
      auto row = json::parse(rows[0]["j"].as<std::string>());
      co_await fieldcrypt::openRow(c.tx, table, row);
      co_return toApi(row, shape->projection);
    }, resolver);
  }

  if (has("delete")) {
    reg.bind(binding->prefix + ".delete", [table](ToolCall& c) -> Task<Json::Value> {
      const auto id = c.args["id"].asString();
      const std::vector<std::optional<std::string>> params{id, db::nullable(c.expectedVersion)};
      const auto rows = co_await c.tx.execParams(
          "UPDATE " + quoted(table) + " SET deleted_at = now() WHERE id = $1::uuid AND deleted_at IS NULL "
          "AND ($2::int IS NULL OR version = $2::int) RETURNING id",
          params);
      if (rows.empty()) co_await explainMiss(c.tx, table, id);
      co_return Json::Value();
    }, resolver);
  }
}

}  // namespace thelife::crud
