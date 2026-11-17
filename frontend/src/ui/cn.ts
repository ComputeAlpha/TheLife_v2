import { clsx, type ClassValue } from "clsx";
import { twMerge } from "tailwind-merge";

/**
 * Joins class names; on Tailwind conflicts the later class wins (a `className` passed to a component overrides
 * its defaults). Plain clsx left that to stylesheet order, which squeezed icons and selects (see §11, 5e).
 */
export function cn(...inputs: ClassValue[]): string {
  return twMerge(clsx(inputs));
}
