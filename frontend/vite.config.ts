import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";
import tailwindcss from "@tailwindcss/vite";
import { fileURLToPath } from "node:url";

// Same origin in development too: the dev server proxies the API (frontend_lld.md §2.1), streams included.
const api = process.env.THELIFE_API_URL ?? "http://127.0.0.1:8080";

export default defineConfig({
  plugins: [react(), tailwindcss()],
  resolve: { alias: { "@": fileURLToPath(new URL("./src", import.meta.url)) } },
  server: {
    host: "127.0.0.1",
    port: 5173,
    strictPort: true,
    proxy: { "/api": { target: api, changeOrigin: false } },
  },
  // No data: URIs: the CSP allows fonts, scripts and styles from the origin only.
  build: { target: "es2022", sourcemap: true, chunkSizeWarningLimit: 900, assetsInlineLimit: 0 },
});
