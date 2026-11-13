#pragma once

#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>

#include <memory>
#include <optional>
#include <vector>
#include <string>
#include <type_traits>
#include <utility>

#include "auth/principal.h"

namespace thelife::db {

template <typename T>
struct isOptional : std::false_type {};
template <typename T>
struct isOptional<std::optional<T>> : std::true_type {};

// Text form of an optional for execParams (NULL when empty).
inline std::optional<std::string> nullable(const std::optional<int>& v) {
  return v ? std::optional<std::string>(std::to_string(*v)) : std::nullopt;
}

// The only way to touch tenant data (backend_lld.md §3). Opens a transaction and sets app.tenant_id,
// app.principal_kind and app.principal_id with SET LOCAL semantics, so RLS applies to every statement and the
// settings vanish at commit or rollback. Not committing (exception or early return) rolls back.
class TenantTx {
 public:
  static drogon::Task<TenantTx> begin(const auth::Principal& principal);
  // For trusted internal code that has a tenant but no principal (jobs, fixtures).
  static drogon::Task<TenantTx> beginSystem(const std::string& tenantId);

  TenantTx(TenantTx&&) noexcept = default;
  TenantTx& operator=(TenantTx&&) noexcept = default;
  TenantTx(const TenantTx&) = delete;
  TenantTx& operator=(const TenantTx&) = delete;
  ~TenantTx();

  // Arguments are copied into the coroutine frame (this Task is lazy, so references to the caller's temporaries
  // are not safe). Nullable values must go through execParams: a std::optional in this parameter pack reached
  // Postgres as '' instead of NULL under GCC, so it is rejected at compile time.
  template <typename... Args>
  drogon::Task<drogon::orm::Result> exec(std::string sql, Args... args) {
    static_assert((!isOptional<Args>::value && ...), "pass nullable values through execParams");
    co_return co_await tx_->execSqlCoro(sql, std::move(args)...);
  }

  // Dynamic parameter count (NULL for nullopt); used by the generic CRUD layer for optional filters.
  drogon::Task<drogon::orm::Result> execParams(std::string sql,
                                               std::vector<std::optional<std::string>> params);

  // Waits until the commit has completed; throws if it failed.
  drogon::Task<void> commit();
  void rollback();

  const std::string& tenantId() const { return tenantId_; }

 private:
  TenantTx(std::shared_ptr<drogon::orm::Transaction> tx, std::string tenantId)
      : tx_(std::move(tx)), tenantId_(std::move(tenantId)) {}
  static drogon::Task<TenantTx> open(const std::string& tenantId, const char* kind, const std::string& principalId);

  std::shared_ptr<drogon::orm::Transaction> tx_;
  std::string tenantId_;
};

}  // namespace thelife::db
