# TheLife web UI

React + TypeScript single-page app (Stage 5, `docs/frontend_lld.md`). The backend serves the built app from
`frontend/dist` on its own origin (`THELIFE_WEB_ROOT`); `scripts/demo.sh up` builds it and sets that up.

Needs Node 22.

| Command | What it does |
|---|---|
| `npm run dev` | Vite on http://127.0.0.1:5173, proxying `/api` to `THELIFE_API_URL` (default `http://127.0.0.1:8080`) |
| `npm run build` | Type-check and build into `dist/` |
| `npm run gen:api` | Regenerate `src/api/schema.d.ts` after the contract changes (`npm run check:api` fails when stale) |
| `npm run lint` / `npm run typecheck` / `npm test` | ESLint, tsc, Vitest (with MSW) |
| `npm run e2e` | Playwright against the real backend on the test DB, desktop and phone sizes, with axe checks |

End to end: `eval "$(scripts/db.sh test)"`, build the backend (`cmake --build build/dev`), then
`npm run build && npm run e2e` in this directory. Global setup creates a throwaway tenant with `thelife-admin`.

Signing in sets an HttpOnly session cookie; scripts never see the token. Writes send `X-TheLife-CSRF: 1`
(the client does this). Dates are shown in the user's timezone from `/me`, never the browser's.
