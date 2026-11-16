import { render } from "@testing-library/react";
import { QueryClient, QueryClientProvider } from "@tanstack/react-query";
import { MemoryRouter, Route, Routes } from "react-router";
import type { ReactNode } from "react";

export function renderApp(ui: ReactNode, { path = "/", routes }: { path?: string; routes?: ReactNode } = {}) {
  const qc = new QueryClient({ defaultOptions: { queries: { retry: false }, mutations: { retry: false } } });
  const utils = render(
    <QueryClientProvider client={qc}>
      <MemoryRouter initialEntries={[path]}>
        {routes ? <Routes>{routes}</Routes> : ui}
      </MemoryRouter>
    </QueryClientProvider>,
  );
  return { ...utils, qc };
}

export { Route };
