import { forwardRef, useId, type InputHTMLAttributes, type ReactNode, type SelectHTMLAttributes, type TextareaHTMLAttributes } from "react";
import { cn } from "./cn";

const control =
  "w-full rounded-[var(--radius-control)] bg-surface-2 border border-line text-text placeholder:text-text-3 " +
  "transition-[border-color,box-shadow] duration-150 hover:border-line-strong " +
  "focus:outline-none focus:border-accent focus:shadow-[0_0_0_3px_rgb(34_211_238/.18)] " +
  "aria-[invalid=true]:border-alert aria-[invalid=true]:focus:shadow-[0_0_0_3px_rgb(219_39_119/.2)] disabled:opacity-60";

export const Input = forwardRef<HTMLInputElement, InputHTMLAttributes<HTMLInputElement>>(function Input(
  { className, ...rest },
  ref,
) {
  return <input ref={ref} className={cn(control, "h-10 px-3 text-sm", className)} {...rest} />;
});

export const Textarea = forwardRef<HTMLTextAreaElement, TextareaHTMLAttributes<HTMLTextAreaElement>>(
  function Textarea({ className, ...rest }, ref) {
    return <textarea ref={ref} className={cn(control, "px-3 py-2 text-sm min-h-24", className)} {...rest} />;
  },
);

export const NativeSelect = forwardRef<HTMLSelectElement, SelectHTMLAttributes<HTMLSelectElement>>(
  function NativeSelect({ className, children, ...rest }, ref) {
    return (
      <select ref={ref} className={cn(control, "h-10 px-3 text-sm appearance-none bg-no-repeat pr-9", className)} {...rest}>
        {children}
      </select>
    );
  },
);

/** Label, control, hint and error, wired together for screen readers. */
export function Field({
  label,
  hint,
  error,
  children,
  className,
}: {
  label: ReactNode;
  hint?: ReactNode;
  error?: string;
  className?: string;
  children: (props: { id: string; "aria-invalid"?: boolean; "aria-describedby"?: string }) => ReactNode;
}) {
  const id = useId();
  const describedBy = error ? `${id}-err` : hint ? `${id}-hint` : undefined;
  return (
    <div className={cn("flex flex-col gap-1.5", className)}>
      <label htmlFor={id} className="text-[13px] font-medium text-text-2">
        {label}
      </label>
      {children({ id, "aria-invalid": error ? true : undefined, "aria-describedby": describedBy })}
      {error ? (
        <p id={`${id}-err`} className="text-[12.5px] text-alert-text" role="alert">
          {error}
        </p>
      ) : hint ? (
        <p id={`${id}-hint`} className="text-[12.5px] text-text-3">
          {hint}
        </p>
      ) : null}
    </div>
  );
}
