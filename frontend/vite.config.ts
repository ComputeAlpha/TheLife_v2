import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";
import tailwindcss from "@tailwindcss/vite";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";

const pkg = JSON.parse(readFileSync(new URL("./package.json", import.meta.url), "utf8")) as { version: string };

// Same origin in development too: the dev server proxies the API (frontend_lld.md §2.1), streams included.
const api = process.env.THELIFE_API_URL ?? "http://127.0.0.1:8080";

export default defineConfig({
  plugins: [react(), tailwindcss()],
  define: { __APP_VERSION__: JSON.stringify(pkg.version), __BUILT_AT__: JSON.stringify(new Date().toISOString()) },
  resolve: { alias: { "@": fileURLToPath(new URL("./src", import.meta.url)) } },
  server: {
    host: "127.0.0.1",
    port: 5173,
    strictPort: true,
    proxy: {
      "/api": { target: api, changeOrigin: false },
      "/healthz": { target: api, changeOrigin: false },
      "/readyz": { target: api, changeOrigin: false },
    },
  },
  // No data: URIs: the CSP allows fonts, scripts and styles from the origin only.
  build: { target: "es2022", sourcemap: true, chunkSizeWarningLimit: 900, assetsInlineLimit: 0 },
});
