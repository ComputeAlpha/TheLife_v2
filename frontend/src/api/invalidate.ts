import { useQueryClient } from "@tanstack/react-query";

/** Returns a function that invalidates the given query families. */
export function useInvalidate(...keys: string[]) {
  const qc = useQueryClient();
  return () => {
    for (const k of keys) qc.invalidateQueries({ queryKey: [k] });
  };
}
