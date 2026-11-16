/** The TheLife mark: an orbit arc around a core, in cyan. */
export function Logo({ className }: { className?: string }) {
  return (
    <svg viewBox="0 0 32 32" className={className} aria-hidden>
      <defs>
        <linearGradient id="tl-arc" x1="0" y1="0" x2="1" y2="1">
          <stop offset="0" stopColor="#67e8f9" />
          <stop offset="1" stopColor="#0891b2" />
        </linearGradient>
      </defs>
      <circle cx="16" cy="16" r="11" fill="none" stroke="var(--color-line-strong)" strokeWidth="2.5" />
      <circle cx="16" cy="16" r="11" fill="none" stroke="url(#tl-arc)" strokeWidth="2.5" strokeLinecap="round" strokeDasharray="50 100" transform="rotate(-90 16 16)" />
      <circle cx="16" cy="16" r="3.5" fill="#22d3ee" />
    </svg>
  );
}
