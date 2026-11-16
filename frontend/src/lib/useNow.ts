import { useEffect, useState } from "react";

/** The current time, re-rendered every `everyMs` (aligned to the boundary, so minute clocks flip on time). */
export function useNow(everyMs = 60_000): number {
  const [now, setNow] = useState(() => Date.now());
  useEffect(() => {
    let timer: ReturnType<typeof setTimeout>;
    const tick = () => {
      const t = Date.now();
      setNow(t);
      timer = setTimeout(tick, everyMs - (t % everyMs) + 5);
    };
    timer = setTimeout(tick, everyMs - (Date.now() % everyMs) + 5);
    return () => clearTimeout(timer);
  }, [everyMs]);
  return now;
}
