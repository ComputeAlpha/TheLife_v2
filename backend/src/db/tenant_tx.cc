#include "db/tenant_tx.h"

#include <stdexcept>

#include "db/database.h"

namespace thelife::db {

namespace {
struct CommitAwaiter : drogon::CallbackAwaiter<bool> {
  std::shared_ptr<drogon::orm::Transaction>& tx;
  explicit CommitAwaiter(std::shared_ptr<drogon::orm::Transaction>& t) : tx(t) {}
  void await_suspend(std::coroutine_handle<> h) {
    tx->setCommitCallback([this, h](bool ok) {
      setValue(ok);
      h.resume();
    });
    tx.reset();  // drogon commits when the last reference is released
  }
};
struct DynamicExecAwaiter : drogon::CallbackAwaiter<drogon::orm::Result> {
  explicit DynamicExecAwaiter(drogon::orm::internal::SqlBinder&& b) : binder(std::move(b)) {}
  void await_suspend(std::coroutine_handle<> h) {
    binder >> [h, this](const drogon::orm::Result& r) {
      setValue(r);
      h.resume();
    };
    // exception_ptr keeps the dynamic type (SqlError with its SQLSTATE) for error mapping.
    binder >> [h, this](const std::exception_ptr& e) {
      setException(e);
      h.resume();
    };
    binder.exec();
  }
  drogon::orm::internal::SqlBinder binder;
};
}  // namespace

drogon::Task<drogon::orm::Result> TenantTx::execParams(std::string sql,
                                                       std::vector<std::optional<std::string>> params) {
  if (!tx_) throw std::logic_error("TenantTx already finished");
  auto binder = *tx_ << sql;
  for (const auto& p : params) binder << p;
  co_return co_await DynamicExecAwaiter(std::move(binder));
}

drogon::Task<TenantTx> TenantTx::open(const std::string& tenantId, const char* kind, const std::string& principalId) {
  if (tenantId.empty()) throw std::logic_error("TenantTx without tenant");
  auto client = Database::instance().client();
  if (!client) throw std::runtime_error("database not configured");
  auto tx = co_await client->newTransactionCoro();
  co_await tx->execSqlCoro(
      "SELECT set_config('app.tenant_id', $1, true), set_config('app.principal_kind', $2, true), "
      "set_config('app.principal_id', $3, true), set_config('TimeZone', 'UTC', true)",
      tenantId, std::string(kind), principalId);
  co_return TenantTx(std::move(tx), tenantId);
}

drogon::Task<TenantTx> TenantTx::begin(const auth::Principal& p) {
  co_return co_await open(p.tenantId, auth::toString(p.kind), p.actorId());
}

drogon::Task<TenantTx> TenantTx::beginSystem(const std::string& tenantId) {
  co_return co_await open(tenantId, "system", "");
}

drogon::Task<void> TenantTx::commit() {
  if (!tx_) throw std::logic_error("TenantTx already finished");
  const bool ok = co_await CommitAwaiter(tx_);
  if (!ok) throw std::runtime_error("transaction commit failed");
}

void TenantTx::rollback() {
  if (tx_) {
    tx_->rollback();
    tx_.reset();
  }
}

TenantTx::~TenantTx() { rollback(); }

}  // namespace thelife::db
