import React, { useState } from "react";
import { createRoot } from "react-dom/client";
import { App } from "./App.jsx";

const root = createRoot(document.getElementById("root"));
const initialData = window.__STATS_DATA__ || {};

let currentHistory = [];

function renderWithData(data, hist) {
  root.render(<App data={data} history={hist} />);
}

renderWithData(initialData, currentHistory);

async function poll() {
  try {
    const res = await fetch("stats-api.js", { credentials: "same-origin" });
    if (!res.ok) return;
    const data = await res.json();
    currentHistory = [...currentHistory, {
      t:          Date.now(),
      active:     data.server?.active   ?? 0,
      queued:     data.server?.queued   ?? 0,
      threads:    data.server?.threads?.current ?? 0,
      threadsMax: data.server?.threads?.max     ?? 0,
      memAlloc:   data.memory?.["generic.current_allocated_bytes"] ?? 0,
    }].slice(-60);
    renderWithData(data, currentHistory);
  } catch {
    /* Ignore transient fetch errors */
  }
}

setInterval(poll, 5000);
