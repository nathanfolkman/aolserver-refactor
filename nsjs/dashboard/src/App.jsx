import React from "react";
import { Tabs } from "./components/Tabs.jsx";
import { Overview } from "./sections/Overview.jsx";
import { JsEngine } from "./sections/JsEngine.jsx";
import { ServerStats } from "./sections/ServerStats.jsx";
import { Memory } from "./sections/Memory.jsx";
import { Caches } from "./sections/Caches.jsx";
import { Drivers } from "./sections/Drivers.jsx";
import { AdpCache } from "./sections/AdpCache.jsx";
import { Log } from "./sections/Log.jsx";

export function App({ data, history }) {
  const {
    info, jsStats, memory, server, drivers, caches, locks, threads, scheduled,
    adpStats, logTail, memorySizeClasses,
  } = data || {};
  const hist = history || [];

  const tabs = [
    {
      label: "Overview",
      content: <Overview info={info} />,
    },
    {
      label: "JS Engine",
      content: <JsEngine jsStats={jsStats} history={hist} />,
    },
    {
      label: "Server",
      content: <ServerStats server={server} locks={locks} threads={threads}
                            scheduled={scheduled} history={hist} />,
    },
    {
      label: "Memory",
      content: <Memory memory={memory} memorySizeClasses={memorySizeClasses} history={hist} />,
    },
    {
      label: "Caches",
      content: <Caches caches={caches} />,
    },
    {
      label: "Drivers",
      content: <Drivers drivers={drivers} />,
    },
    {
      label: "ADP Cache",
      content: <AdpCache adpStats={adpStats} />,
    },
    {
      label: "Log",
      content: <Log logTail={logTail} />,
    },
  ];

  return (
    <div className="h-screen flex flex-col bg-background overflow-hidden">
      <header className="border-b bg-card px-6 py-4 flex items-center justify-between shrink-0">
        <div>
          <h1 className="text-xl font-bold">AOLserver Stats</h1>
          <p className="text-sm text-muted-foreground">
            {info?.serverName || "server"} · Live dashboard
          </p>
        </div>
        <div className="text-xs text-muted-foreground">
          Auto-refresh every 5s
        </div>
      </header>
      <main className="flex-1 overflow-y-auto px-6 py-6">
        <div className="max-w-7xl mx-auto">
          <Tabs tabs={tabs} />
        </div>
      </main>
    </div>
  );
}
