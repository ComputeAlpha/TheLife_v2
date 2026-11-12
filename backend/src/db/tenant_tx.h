#pragma once

#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>

#include <memory>
#include <optional>
#include <vector>
#include <string>
#include <utility>

#include "auth/principal.h"

namespace thelife::db {

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

  template <typename... Args>
  drogon::Task<drogon::orm::Result> exec(const std::string& sql, Args&&... args) {
    co_return co_await tx_->execSqlCoro(sql, std::forward<Args>(args)...);
  }

  // Dynamic parameter count (NULL for nullopt); used by the generic CRUD layer for optional filters.
  drogon::Task<drogon::orm::Result> execParams(const std::string& sql,
                                               const std::vector<std::optional<std::string>>& params);

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
