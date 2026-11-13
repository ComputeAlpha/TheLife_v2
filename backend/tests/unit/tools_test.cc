#include <gtest/gtest.h>

#include "core/json.h"
#include "tools/manifest.h"
#include "tools/policy.h"
#include "tools/schema_validator.h"

using namespace thelife;
using namespace thelife::tools;

TEST(Manifest, EmbeddedManifestLoadsAndEverySchemaCompiles) {
  const auto& m = embeddedManifest();
  EXPECT_FALSE(m.contractVersion.empty());
  EXPECT_GT(m.tools.size(), 90u);
  for (const auto& [name, t] : m.tools) {
    EXPECT_NO_THROW(SchemaValidator{t.inputSchema}) << name;
    EXPECT_NO_THROW(SchemaValidator{t.outputSchema}) << name;
    if (t.rest) {
      EXPECT_TRUE(t.rest->path.starts_with("/")) << name;
    }
  }
  const auto& upd = m.tools.at("goals.update");
  EXPECT_EQ(upd.effect, Effect::Update);
  ASSERT_TRUE(upd.rest);
  EXPECT_TRUE(upd.rest->ifMatch);
  EXPECT_EQ(upd.targetResource, "Goal");
}

TEST(SchemaValidator, ReportsPointersTypesEnumsFormatsAndUnknownProperties) {
  const auto& create = embeddedManifest().tools.at("goals.create");
  SchemaValidator v(create.inputSchema);
  EXPECT_TRUE(v.validate(json::parse(R"({"title":"x","area":"skill","horizon":"year"})")).empty());

  auto errs = v.validate(json::parse(R"({"title":"","area":"nap","horizon":"year","parent_id":"nope","bogus":1})"));
  std::set<std::string> paths;
  for (const auto& e : errs) paths.insert(e.path);
  EXPECT_TRUE(paths.count("/title"));
  EXPECT_TRUE(paths.count("/area"));
  EXPECT_TRUE(paths.count("/parent_id"));  // uuid format (our extra check)
  EXPECT_TRUE(paths.count("/bogus"));
  EXPECT_FALSE(paths.count(""));

  auto missing = v.validate(json::parse(R"({"title":"x"})"));
  EXPECT_FALSE(missing.empty());
  EXPECT_FALSE(v.validate(json::parse(R"({"title":"x","area":"skill","horizon":"year","due_on":"2026-13-40"})")).empty());
  EXPECT_TRUE(v.validate(json::parse(R"({"title":"x","area":"skill","horizon":"year","parent_id":null})")).empty());
}

namespace {
ToolDef toolWith(Effect e) {
  ToolDef t;
  t.name = "x.y";
  t.effect = e;
  return t;
}
auth::Principal as(auth::PrincipalKind k) {
  auth::Principal p;
  p.kind = k;
  return p;
}
}  // namespace

TEST(Policy, PermissionTableOfToolRegistrySpec) {
  using K = auth::PrincipalKind;
  const auto read = toolWith(Effect::Read), create = toolWith(Effect::Create), update = toolWith(Effect::Update),
             del = toolWith(Effect::Delete);
  const auto user = as(K::User), agent = as(K::Agent), key = as(K::ApiKey);

  for (const auto* t : {&read, &create, &update, &del}) {
    EXPECT_EQ(decide({user, *t, std::nullopt}), Decision::Allow);
    EXPECT_EQ(decide({key, *t, std::nullopt}), Decision::Allow);
  }
  // MCP clients: reads allowed, writes become proposals
  EXPECT_EQ(decide({key, read, std::nullopt, true, true}), Decision::Allow);
  EXPECT_EQ(decide({key, create, std::nullopt, true, true}), Decision::Propose);
  EXPECT_EQ(decide({key, del, std::nullopt, true, true}), Decision::Propose);

  // agent
  EXPECT_EQ(decide({agent, read, std::nullopt}), Decision::Allow);
  EXPECT_EQ(decide({agent, create, std::nullopt}), Decision::Allow);
  EXPECT_EQ(decide({agent, update, TargetProvenance{"agent", "direct"}}), Decision::Allow);
  EXPECT_EQ(decide({agent, update, TargetProvenance{"agent", "inferred"}}), Decision::Allow);
  EXPECT_EQ(decide({agent, update, TargetProvenance{"agent", "user_stated"}}), Decision::RequireApproval);
  EXPECT_EQ(decide({agent, update, TargetProvenance{"user", "direct"}}), Decision::RequireApproval);
  EXPECT_EQ(decide({agent, update, std::nullopt}), Decision::RequireApproval);
  EXPECT_EQ(decide({agent, del, TargetProvenance{"agent", "direct"}}), Decision::RequireApproval);

  // kill switch: agent reads still work, every write is denied
  EXPECT_EQ(decide({agent, read, std::nullopt, false}), Decision::Allow);
  EXPECT_EQ(decide({agent, create, std::nullopt, false}), Decision::Deny);
  EXPECT_EQ(decide({agent, update, TargetProvenance{"agent", "direct"}, false}), Decision::Deny);
}

TEST(Policy, NamedPoliciesAndUndo) {
  using K = auth::PrincipalKind;
  const auto agent = as(K::Agent);
  auto progress = toolWith(Effect::Update);
  progress.policy = "user_reported_progress";
  const TargetProvenance userTask{"user", "direct", 3, false};
  PolicyInput in{agent, progress, userTask};
  EXPECT_EQ(decide(in), Decision::RequireApproval);  // not from a user message
  in.userTurn = true;
  EXPECT_EQ(decide(in), Decision::Allow);
  in.agentWritesEnabled = false;
  EXPECT_EQ(decide(in), Decision::Deny);  // the kill switch still wins

  auto forget = toolWith(Effect::Delete);
  forget.policy = "user_stated_fact";
  EXPECT_EQ(decide({agent, forget, TargetProvenance{"agent", "user_stated", 1, true}}), Decision::RequireApproval);
  EXPECT_EQ(decide({agent, forget, TargetProvenance{"agent", "inferred", 1, false}}), Decision::Allow);

  const auto del = toolWith(Effect::Delete);
  EXPECT_EQ(decide({agent, del, TargetProvenance{"agent", "user_stated", 1, true}}), Decision::Allow);   // undo
  EXPECT_EQ(decide({agent, del, TargetProvenance{"agent", "user_stated", 1, false}}), Decision::RequireApproval);
  EXPECT_EQ(decide({agent, del, TargetProvenance{"user", "direct", 1, true}}), Decision::RequireApproval);
}

TEST(Policy, WebContentInTheTurnHoldsEveryAgentWrite) {
  using K = auth::PrincipalKind;
  const auto agent = as(K::Agent);
  const auto create = toolWith(Effect::Create), read = toolWith(Effect::Read);
  auto progress = toolWith(Effect::Update);
  progress.policy = "user_reported_progress";
  PolicyInput in{agent, create, std::nullopt};
  in.untrustedContext = true;
  in.userTurn = true;
  EXPECT_EQ(decide(in), Decision::RequireApproval);
  EXPECT_EQ(decide({agent, read, std::nullopt, true, false, true, true}), Decision::Allow);  // reads still run
  PolicyInput p{agent, progress, TargetProvenance{"user", "direct", 1, false}};
  p.userTurn = true;
  p.untrustedContext = true;
  EXPECT_EQ(decide(p), Decision::RequireApproval);  // even the named policy
  p.agentWritesEnabled = false;
  EXPECT_EQ(decide(p), Decision::Deny);  // the kill switch still wins
  const auto user = as(K::User);
  PolicyInput u{user, create, std::nullopt};
  u.untrustedContext = true;
  EXPECT_EQ(decide(u), Decision::Allow);  // only agent writes are affected
}
