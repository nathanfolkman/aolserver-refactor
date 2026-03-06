/**
 * client-entry.jsx — browser rendering + live polling
 *
 * Renders the full React app client-side from embedded JSON data,
 * then polls stats-api.js every 5 seconds for live updates.
 */
import React from "react";
import { createRoot } from "react-dom/client";
import { App } from "./App.jsx";

const root = createRoot(document.getElementById("root"));
const initialData = window.__STATS_DATA__ || {};

root.render(<App data={initialData} />);

/* Live polling */
async function poll() {
  try {
    const res = await fetch("stats-api.js", { credentials: "same-origin" });
    if (!res.ok) return;
    const data = await res.json();
    root.render(<App data={data} />);
  } catch {
    /* Ignore transient fetch errors */
  }
}

setInterval(poll, 5000);
