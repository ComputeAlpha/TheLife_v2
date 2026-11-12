#!/usr/bin/env bash
# Contract tests: schemathesis checks every user-reachable Stage 1 operation of api/generated/openapi.yaml
# against a running backend (backend_lld.md §12). Use a throwaway tenant: it creates and deletes data.
#
#   THELIFE_CONTRACT_API=http://127.0.0.1:8080 THELIFE_CONTRACT_EMAIL=... THELIFE_CONTRACT_PASSWORD=... \
#     backend/tests/contract/run.sh [--strict] [--with-login] [extra schemathesis args]
#
# --strict      positive_data_acceptance with schemathesis defaults (review noise expected; see schemathesis.toml)
# --with-login  also fuzz POST /auth/login, as a last pass. Its failed attempts trip the per-IP login limit
#               (30 per 15 min), so further logins from this host get 429 for a while. CI passes it; locally,
#               leave it off unless you are testing login.
#
# SCHEMATHESIS and PYTHON override the executables (PYTHON needs PyYAML, which schemathesis installs).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
api="${THELIFE_CONTRACT_API:?set THELIFE_CONTRACT_API}"
: "${THELIFE_CONTRACT_EMAIL:?set THELIFE_CONTRACT_EMAIL}" "${THELIFE_CONTRACT_PASSWORD:?set THELIFE_CONTRACT_PASSWORD}"
st="${SCHEMATHESIS:-schemathesis}"
py="${PYTHON:-python3}"
contract="$root/api/generated/openapi.yaml"

config="$here/schemathesis.toml"
with_login=0
extra=()
for arg in "$@"; do
  case "$arg" in
    --strict)
      config="$(mktemp)"
      trap 'rm -f "$config"' EXIT
      sed '/^\[checks.positive_data_acceptance\]/,/^$/d' "$here/schemathesis.toml" > "$config" ;;
    --with-login) with_login=1 ;;
    *) extra+=("$arg") ;;
  esac
done

# Log in once; the token is passed as a header and filtered from schemathesis output.
token="$("$py" - "$api" <<'PY'
import json, os, sys, urllib.error, urllib.request
body = json.dumps({"email": os.environ["THELIFE_CONTRACT_EMAIL"],
                   "password": os.environ["THELIFE_CONTRACT_PASSWORD"]}).encode()
req = urllib.request.Request(sys.argv[1] + "/api/v1/auth/login", body, {"Content-Type": "application/json"})
try:
    print(json.load(urllib.request.urlopen(req, timeout=10))["token"])
except urllib.error.HTTPError as e:
    if e.code == 429:
        sys.exit(f"login is rate limited for {e.headers.get('Retry-After', '?')} s "
                 "(an earlier --with-login run used up the per-IP limit)")
    raise
PY
)"

# Operations a user cannot call at Stage 1 (collector endpoints, later stages), the two that would end the
# session under test, and login (fuzzed separately, see --with-login).
exclude="$("$py" - "$contract" <<'PY'
import re, sys, yaml
doc = yaml.safe_load(open(sys.argv[1]))
ops = {"auth.logout", "auth.change_password", "auth.login"}
for methods in doc["paths"].values():
    for o in methods.values():
        t = o["x-thelife-tool"]
        if t["stage"] > 1 or "user" not in t["principals"]:
            ops.add(o["operationId"])
print("^(" + "|".join(re.escape(o) for o in sorted(ops)) + ")$")
PY
)"

common=(--config-file "$config" run "$contract" --url "$api"
        --checks all --exclude-checks unsupported_method
        --phases examples,coverage,fuzzing --workers 4)

"$st" "${common[@]}" -H "Authorization: Bearer $token" --exclude-operation-id-regex "$exclude" \
  --max-examples "${THELIFE_CONTRACT_EXAMPLES:-50}" "${extra[@]}"

if [[ "$with_login" == 1 ]]; then
  "$st" "${common[@]}" --include-operation-id auth.login --max-examples 20 "${extra[@]}"
fi
