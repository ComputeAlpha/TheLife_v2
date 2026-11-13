// thelife-admin: tenant and account bootstrap (backend_lld.md §7.3). There is no signup and no HTTP admin API;
// this CLI is the only way to create tenants and users. It connects as thelife_admin, which can only execute the
// admin.* SECURITY DEFINER functions.
//
//   THELIFE_ADMIN_DB_URL=postgresql://thelife_admin:...@host/thelife thelife-admin <command>
//
//   tenant create --name NAME                     prints the tenant id
//   tenant list
//   tenant agent-writes TENANT_ID on|off          per-tenant kill switch for agent writes
//   user create --tenant ID --email EMAIL --name NAME [--role user|tenant_admin] [--timezone TZ]
//                                                 prints a one-time password
//   user reset-password --email EMAIL             prints a new one-time password, revokes sessions
#include <drogon/orm/DbClient.h>
#include <drogon/orm/Exception.h>

#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "core/crypto.h"
#include "core/ids.h"

namespace {

int usage() {
  std::cerr << "usage:\n"
               "  thelife-admin tenant create --name NAME\n"
               "  thelife-admin tenant list\n"
               "  thelife-admin tenant agent-writes TENANT_ID on|off\n"
               "  thelife-admin user create --tenant ID --email EMAIL --name NAME [--role user|tenant_admin]"
               " [--timezone TZ]\n"
               "  thelife-admin user reset-password --email EMAIL\n"
               "environment: THELIFE_ADMIN_DB_URL\n";
  return 2;
}

std::map<std::string, std::string> parseFlags(const std::vector<std::string>& args, size_t from) {
  std::map<std::string, std::string> out;
  for (size_t i = from; i < args.size(); ++i) {
    if (args[i].rfind("--", 0) == 0 && i + 1 < args.size()) {
      out[args[i].substr(2)] = args[i + 1];
      ++i;
    } else {
      throw std::invalid_argument("unexpected argument: " + args[i]);
    }
  }
  return out;
}

std::string need(const std::map<std::string, std::string>& f, const std::string& k) {
  auto it = f.find(k);
  if (it == f.end() || it->second.empty()) throw std::invalid_argument("missing --" + k);
  return it->second;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args(argv + 1, argv + argc);
  if (args.size() < 2) return usage();
  const char* url = std::getenv("THELIFE_ADMIN_DB_URL");
  if (!url || !*url) {
    std::cerr << "THELIFE_ADMIN_DB_URL is not set\n";
    return 2;
  }
  try {
    auto db = drogon::orm::DbClient::newPgClient(url, 1);
    db->setTimeout(10.0);
    const auto& group = args[0];
    const auto& cmd = args[1];

    if (group == "tenant" && cmd == "create") {
      const auto f = parseFlags(args, 2);
      const auto r = db->execSqlSync("SELECT admin.create_tenant($1, $2) AS id", thelife::ids::newUuidV7(),
                                     need(f, "name"));
      std::cout << r[0]["id"].as<std::string>() << "\n";
    } else if (group == "tenant" && cmd == "list") {
      const auto r = db->execSqlSync("SELECT id, name, status, agent_writes_enabled, users FROM admin.list_tenants()");
      for (const auto& row : r) {
        std::cout << row["id"].as<std::string>() << "  " << row["name"].as<std::string>() << "  "
                  << row["status"].as<std::string>() << "  agent_writes="
                  << (row["agent_writes_enabled"].as<bool>() ? "on" : "off") << "  users="
                  << row["users"].as<std::string>() << "\n";
      }
    } else if (group == "tenant" && cmd == "agent-writes") {
      if (args.size() != 4 || (args[3] != "on" && args[3] != "off")) return usage();
      db->execSqlSync("SELECT admin.set_agent_writes($1, $2)", args[2], args[3] == "on");
      std::cout << "agent writes " << args[3] << "\n";
    } else if (group == "user" && cmd == "create") {
      const auto f = parseFlags(args, 2);
      const auto password = thelife::crypto::randomPassword();
      const auto role = f.count("role") ? f.at("role") : std::string("user");
      const auto tz = f.count("timezone") ? f.at("timezone") : std::string("UTC");
      const auto r = db->execSqlSync("SELECT admin.create_user($1, $2, $3, $4, $5, $6, $7) AS id",
                                     thelife::ids::newUuidV7(), need(f, "tenant"), need(f, "email"),
                                     need(f, "name"), thelife::crypto::hashPassword(password), role, tz);
      std::cout << "user id:  " << r[0]["id"].as<std::string>() << "\n"
                << "email:    " << need(f, "email") << "\n"
                << "password: " << password << "   (shown once; ask the user to change it)\n";
    } else if (group == "user" && cmd == "reset-password") {
      const auto f = parseFlags(args, 2);
      const auto password = thelife::crypto::randomPassword();
      db->execSqlSync("SELECT admin.reset_password($1, $2)", need(f, "email"),
                      thelife::crypto::hashPassword(password));
      std::cout << "password: " << password << "   (shown once; all sessions revoked)\n";
    } else {
      return usage();
    }
  } catch (const drogon::orm::DrogonDbException& e) {
    std::cerr << "database error: " << e.base().what() << "\n";
    return 1;
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
