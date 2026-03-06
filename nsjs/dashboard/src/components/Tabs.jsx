import React, { useState, useEffect } from "react";

function labelToHash(label) {
  return label.toLowerCase().replace(/\s+/g, "-");
}

function getInitialTab(tabs) {
  const hash = typeof window !== "undefined" ? window.location.hash.slice(1) : "";
  const idx = tabs.findIndex((t) => labelToHash(t.label) === hash);
  return idx >= 0 ? idx : 0;
}

export function Tabs({ tabs }) {
  const [active, setActive] = useState(() => getInitialTab(tabs));

  useEffect(() => {
    const onHashChange = () => setActive(getInitialTab(tabs));
    window.addEventListener("hashchange", onHashChange);
    return () => window.removeEventListener("hashchange", onHashChange);
  }, [tabs]);

  function select(i) {
    window.location.hash = labelToHash(tabs[i].label);
    setActive(i);
  }

  return (
    <div>
      <div className="border-b flex gap-1 overflow-x-auto">
        {tabs.map((t, i) => (
          <button
            key={i}
            onClick={() => select(i)}
            className={`px-4 py-2 text-sm font-medium whitespace-nowrap transition-colors border-b-2 -mb-px ${
              active === i
                ? "border-primary text-primary"
                : "border-transparent text-muted-foreground hover:text-foreground"
            }`}
          >
            {t.label}
          </button>
        ))}
      </div>
      <div className="pt-4">{tabs[active]?.content}</div>
    </div>
  );
}
