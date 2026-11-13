#include "modules/common.h"

#include "core/errors.h"
#include "core/recurrence.h"

namespace thelife::modules {

drogon::Task<UserClock> userClock(db::TenantTx& tx, const std::string& userId) {
  const auto rows = co_await tx.exec(
      "SELECT u.timezone, u.day_start_hour::int AS dsh, "
      "  to_char(((now() AT TIME ZONE u.timezone) - make_interval(hours => u.day_start_hour))::date, "
      "          'YYYY-MM-DD') AS today "
      "FROM users u WHERE u.id = $1::uuid",
      userId);
  if (rows.empty()) throw ApiError::unauthorized();
  co_return UserClock{rows[0]["timezone"].as<std::string>(), rows[0]["dsh"].as<int>(),
                      rows[0]["today"].as<std::string>()};
}

drogon::Task<std::vector<std::pair<std::string, std::string>>> toInstants(
    db::TenantTx& tx, const std::string& timezone,
    const std::vector<std::pair<recurrence::LocalDateTime, recurrence::LocalDateTime>>& local) {
  std::vector<std::pair<std::string, std::string>> out;
  if (local.empty()) co_return out;
  std::string a = "{", b = "{";
  for (size_t i = 0; i < local.size(); ++i) {
    a += (i ? ",\"" : "\"") + local[i].first.str() + "\"";
    b += (i ? ",\"" : "\"") + local[i].second.str() + "\"";
  }
  a += "}";
  b += "}";
  const auto rows = co_await tx.exec(
      "SELECT to_jsonb(s.a AT TIME ZONE $3) #>> '{}' AS a, to_jsonb(s.b AT TIME ZONE $3) #>> '{}' AS b "
      "FROM unnest($1::timestamp[], $2::timestamp[]) WITH ORDINALITY AS s(a, b, n) ORDER BY s.n",
      a, b, timezone);
  out.reserve(rows.size());
  for (const auto& r : rows) out.emplace_back(r["a"].as<std::string>(), r["b"].as<std::string>());
  co_return out;
}

void requireValidRrule(const Json::Value& fields, const std::string& name) {
  if (!fields.isMember(name) || fields[name].isNull()) return;
  if (auto err = recurrence::validate(fields[name].asString())) {
    throw ApiError::validation({{"/" + name, *err}});
  }
}

std::map<std::string, std::string> intervalFilters(const std::string& startCol, const std::string& endCol) {
  return {{"from", "r." + endCol + " >= $?::timestamptz"}, {"to", "r." + startCol + " < $?::timestamptz"}};
}

}  // namespace thelife::modules
