/** Follows `next_cursor` until the list ends (or `maxPages`, a guard against runaway lists). */
export async function collect<T>(
  page: (cursor: string | undefined) => Promise<{ items: T[]; next_cursor?: string | null }>,
  maxPages = 10,
): Promise<T[]> {
  const out: T[] = [];
  let cursor: string | undefined;
  for (let i = 0; i < maxPages; i++) {
    const p = await page(cursor);
    out.push(...p.items);
    if (!p.next_cursor) break;
    cursor = p.next_cursor;
  }
  return out;
}
