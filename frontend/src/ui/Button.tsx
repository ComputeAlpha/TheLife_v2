import { forwardRef, type ButtonHTMLAttributes, type ReactNode } from "react";
import { Loader2 } from "lucide-react";
import { cn } from "./cn";

type Variant = "primary" | "secondary" | "ghost" | "danger";
type Size = "sm" | "md" | "lg";

export interface ButtonProps extends ButtonHTMLAttributes<HTMLButtonElement> {
  variant?: Variant;
  size?: Size;
  loading?: boolean;
  icon?: ReactNode;
}

const variants: Record<Variant, string> = {
  primary:
    "bg-accent text-accent-ink font-semibold hover:bg-accent-strong shadow-[0_0_0_1px_rgb(34_211_238/.4),0_8px_24px_-8px_rgb(34_211_238/.6)] hover:shadow-[0_0_0_1px_rgb(103_232_249/.6),0_10px_32px_-8px_rgb(34_211_238/.8)]",
  secondary: "bg-surface-2 text-text border border-line hover:bg-surface-3 hover:border-line-strong",
  ghost: "text-text-2 hover:text-text hover:bg-surface-2",
  danger: "bg-alert/15 text-alert-text border border-alert/40 hover:bg-alert/25",
};
const sizes: Record<Size, string> = {
  sm: "h-8 px-3 text-[13px] gap-1.5",
  md: "h-10 px-4 text-sm gap-2",
  lg: "h-12 px-6 text-[15px] gap-2",
};

export const Button = forwardRef<HTMLButtonElement, ButtonProps>(function Button(
  { variant = "secondary", size = "md", loading, icon, className, children, disabled, type = "button", ...rest },
  ref,
) {
  return (
    <button
      ref={ref}
      type={type}
      disabled={disabled || loading}
      aria-busy={loading || undefined}
      className={cn(
        "inline-flex items-center justify-center rounded-[var(--radius-control)] font-medium whitespace-nowrap select-none",
        "transition-[background,box-shadow,color,border-color,transform] duration-150 active:scale-[.98]",
        "disabled:opacity-50 disabled:pointer-events-none",
        variants[variant],
        sizes[size],
        className,
      )}
      {...rest}
    >
      {loading ? <Loader2 className="size-4 animate-spin" aria-hidden /> : icon}
      {children}
    </button>
  );
});

export const IconButton = forwardRef<HTMLButtonElement, ButtonProps & { label: string }>(function IconButton(
  { label, className, size = "md", variant = "ghost", children, ...rest },
  ref,
) {
  return (
    <Button
      ref={ref}
      aria-label={label}
      title={label}
      variant={variant}
      size={size}
      className={cn(size === "sm" ? "w-8 px-0" : size === "lg" ? "w-12 px-0" : "w-10 px-0", className)}
      {...rest}
    >
      {children}
    </Button>
  );
});
