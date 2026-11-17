// Pure helpers for the Activity review page (frontend_lld.md §6.5).
import type { Area, Detection } from "@/api/types";
import { detectionSource, type DetectionSource } from "@/api/types";

export const COUNT_CONFIDENCE = 0.4;

/** Counted toward the day: confirmed, or detected with confidence ≥ 0.4 (never disputed or superseded). */
export function isCounted(d: Detection): boolean {
  if (d.status === "confirmed") return true;
  return (d.status ?? "detected") === "detected" && (d.confidence ?? 1) >= COUNT_CONFIDENCE;
}

/**
 * Counted minutes per area within [fromMs, toMs). Sources overlap (vision and process can describe the same
 * minutes), so each area's intervals are merged before summing, as the demo UI's `area_minutes` does.
 */
export function areaMinutes(items: Detection[], fromMs: number, toMs: number): Partial<Record<Area, number>> {
  const spans = new Map<Area, [number, number][]>();
  for (const d of items) {
    if (!isCounted(d)) continue;
    const s = Math.max(Date.parse(d.started_at), fromMs);
    const e = Math.min(Date.parse(d.ended_at), toMs);
    if (e <= s) continue;
    (spans.get(d.area) ?? spans.set(d.area, []).get(d.area)!).push([s, e]);
  }
  const out: Partial<Record<Area, number>> = {};
  for (const [area, iv] of spans) {
    iv.sort((a, b) => a[0] - b[0]);
    let total = 0;
    let [cs, ce] = iv[0]!;
    for (const [s, e] of iv.slice(1)) {
      if (s <= ce) ce = Math.max(ce, e);
      else {
        total += ce - cs;
        [cs, ce] = [s, e];
      }
    }
    total += ce - cs;
    out[area] = Math.round(total / 60_000);
  }
  return out;
}

export type ReviewStatus = "detected" | "confirmed" | "disputed" | "superseded";

export interface ReviewFilters {
  sources: Set<DetectionSource>;
  statuses: Set<ReviewStatus>;
  areas: Set<Area>;
  showCorrections: boolean;
}

export const NO_FILTERS: ReviewFilters = { sources: new Set(), statuses: new Set(), areas: new Set(), showCorrections: false };

/** An empty filter set means "all". Superseded rows only appear with "Show corrections". */
export function applyFilters(items: Detection[], f: ReviewFilters): Detection[] {
  return items.filter((d) => {
    const st = (d.status ?? "detected") as ReviewStatus;
    if (st === "superseded" && !f.showCorrections) return false;
    if (f.sources.size && !f.sources.has(detectionSource(d))) return false;
    if (f.statuses.size && !f.statuses.has(st)) return false;
    if (f.areas.size && !f.areas.has(d.area)) return false;
    return true;
  });
}

/**
 * The hours the timeline shows: from an hour before the first item (the day start when there are none) to the
 * later of the last item and (for today) the current hour, never shorter than 8 hours. Whole hours, within 0..24.
 */
export function visibleHours(
  spans: { start: number; end: number }[], // minutes into the day
  dayStartHour: number,
  nowMinute: number | null,
): [number, number] {
  // With items, start an hour before the first one; an empty day starts at the day start.
  let lo = spans.length ? Math.min(...spans.map((s) => Math.floor(s.start / 60) - 1)) : dayStartHour;
  let hi = spans.length ? Math.max(...spans.map((s) => Math.ceil(s.end / 60))) : dayStartHour + 10;
  if (nowMinute != null) hi = Math.max(hi, Math.ceil(nowMinute / 60) + 1);
  lo = Math.max(0, lo);
  hi = Math.min(24, hi);
  if (hi - lo < 8) {
    if (lo + 8 <= 24) hi = lo + 8;
    else lo = Math.max(0, hi - 8);
  }
  return [lo, hi];
}

/** Ids that a bulk "Confirm" would change: only unreviewed ones. */
export function confirmable(items: Detection[], selected: Set<string>): string[] {
  return items.filter((d) => selected.has(d.id) && (d.status ?? "detected") === "detected").map((d) => d.id);
}
