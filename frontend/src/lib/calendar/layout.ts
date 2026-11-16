// Places timed entries of one day column side by side when they overlap (frontend_lld.md §6.3): entries are grouped
// into collision clusters; each entry takes the first free column of its cluster, and the cluster's column count
// sets the width of all its entries.
export interface Span<T> {
  item: T;
  start: number; // minutes from the column's midnight
  end: number;
}
export interface Placed<T> extends Span<T> {
  col: number;
  cols: number;
  /** Columns to the right that are free for the whole span, so the block can widen into them. */
  span: number;
}

export function layoutColumn<T>(spans: Span<T>[], minDuration = 20): Placed<T>[] {
  const items = spans
    .map((s) => ({ ...s, start: Math.max(0, s.start), end: Math.min(1440, Math.max(s.end, s.start + minDuration)) }))
    .filter((s) => s.end > 0 && s.start < 1440)
    .sort((a, b) => a.start - b.start || b.end - a.end);
  const out: Placed<T>[] = [];
  let cluster: Placed<T>[] = [];
  let colEnds: number[] = [];
  let clusterEnd = -1;

  const flush = () => {
    const cols = colEnds.length;
    for (const p of cluster) {
      p.cols = cols;
      let span = 1;
      while (p.col + span < cols && !cluster.some((q) => q.col === p.col + span && q.start < p.end && q.end > p.start)) span++;
      p.span = span;
    }
    out.push(...cluster);
    cluster = [];
    colEnds = [];
  };

  for (const s of items) {
    if (s.start >= clusterEnd && cluster.length) flush();
    let col = colEnds.findIndex((e) => e <= s.start);
    if (col === -1) {
      col = colEnds.length;
      colEnds.push(s.end);
    } else {
      colEnds[col] = s.end;
    }
    cluster.push({ ...s, col, cols: 1, span: 1 });
    clusterEnd = Math.max(clusterEnd, s.end);
  }
  if (cluster.length) flush();
  return out;
}
