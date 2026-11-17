import { motion } from "motion/react";
import { cn } from "./cn";

/** A thin animated progress bar, value 0..1. */
export function Bar({ value, color, className, label }: { value: number; color: string; className?: string; label?: string }) {
  const v = Math.max(0, Math.min(1, Number.isFinite(value) ? value : 0));
  return (
    <div
      className={cn("h-1.5 overflow-hidden rounded-full bg-surface-3", className)}
      {...(label ? { role: "meter", "aria-label": label, "aria-valuemin": 0, "aria-valuemax": 100, "aria-valuenow": Math.round(v * 100) } : {})}
    >
      <motion.div
        className="h-full rounded-full"
        style={{ background: `linear-gradient(90deg, color-mix(in oklab, ${color} 55%, transparent), ${color})`, boxShadow: `0 0 10px -2px ${color}` }}
        initial={{ width: 0 }}
        animate={{ width: `${v * 100}%` }}
        transition={{ duration: 0.9, ease: [0.22, 1, 0.36, 1] }}
      />
    </div>
  );
}
