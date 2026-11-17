import ReactMarkdown, { type Components } from "react-markdown";
import remarkGfm from "remark-gfm";

// Model output as markdown: raw HTML is dropped (never injected), links open outside and only for web URLs.
const components: Components = {
  a: ({ href, children }) => {
    const safe = href && /^https?:\/\//i.test(href) ? href : undefined;
    return safe ? (
      <a href={safe} target="_blank" rel="noopener noreferrer nofollow" className="text-accent underline decoration-accent/40 underline-offset-2 hover:decoration-accent">
        {children}
      </a>
    ) : (
      <span className="text-accent">{children}</span>
    );
  },
  p: ({ children }) => <p className="my-2 first:mt-0 last:mb-0">{children}</p>,
  ul: ({ children }) => <ul className="my-2 list-disc space-y-1 pl-5 marker:text-accent/70">{children}</ul>,
  ol: ({ children }) => <ol className="my-2 list-decimal space-y-1 pl-5 marker:text-text-3">{children}</ol>,
  h1: ({ children }) => <h3 className="mb-1 mt-3 text-base font-semibold">{children}</h3>,
  h2: ({ children }) => <h3 className="mb-1 mt-3 text-base font-semibold">{children}</h3>,
  h3: ({ children }) => <h4 className="mb-1 mt-3 text-[15px] font-semibold">{children}</h4>,
  strong: ({ children }) => <strong className="font-semibold text-text">{children}</strong>,
  code: ({ children, className }) =>
    className ? (
      <code className="block overflow-x-auto rounded-lg border border-line bg-surface-2 p-3 font-mono text-[12.5px]">{children}</code>
    ) : (
      <code className="rounded bg-surface-3 px-1 py-0.5 font-mono text-[12.5px]">{children}</code>
    ),
  pre: ({ children }) => <pre className="my-2">{children}</pre>,
  table: ({ children }) => <div className="my-2 overflow-x-auto"><table className="w-full border-collapse text-[13px]">{children}</table></div>,
  th: ({ children }) => <th className="border-b border-line-strong px-2 py-1 text-left font-medium text-text-2">{children}</th>,
  td: ({ children }) => <td className="border-b border-line px-2 py-1">{children}</td>,
  blockquote: ({ children }) => <blockquote className="my-2 border-l-2 border-accent/50 pl-3 text-text-2">{children}</blockquote>,
  img: () => null,
};

export function Markdown({ children }: { children: string }) {
  return (
    <div className="break-words text-[14.5px] leading-relaxed text-text">
      <ReactMarkdown remarkPlugins={[remarkGfm]} skipHtml components={components}>
        {children}
      </ReactMarkdown>
    </div>
  );
}
