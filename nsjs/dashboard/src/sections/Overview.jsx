import React from "react";
import { StatCard } from "../components/StatCard.jsx";
import { Card, CardHeader, CardTitle, CardContent } from "../components/Card.jsx";

function formatUptime(seconds) {
  const d = Math.floor(seconds / 86400);
  const h = Math.floor((seconds % 86400) / 3600);
  const m = Math.floor((seconds % 3600) / 60);
  const s = seconds % 60;
  const parts = [];
  if (d > 0) parts.push(`${d}d`);
  if (h > 0) parts.push(`${h}h`);
  if (m > 0) parts.push(`${m}m`);
  parts.push(`${s}s`);
  return parts.join(" ");
}

function formatDate(epoch) {
  if (!epoch) return "—";
  return new Date(epoch * 1000).toLocaleString();
}

export function Overview({ info }) {
  if (!info) return null;
  return (
    <div className="space-y-6">
      <div className="grid grid-cols-2 md:grid-cols-4 gap-4">
        <StatCard label="Server" value={info.serverName || "—"} />
        <StatCard label="Version" value={info.version || "—"} sub={info.buildDate} />
        <StatCard label="Uptime" value={formatUptime(info.uptime || 0)} sub={`Boot: ${formatDate(info.boottime)}`} />
        <StatCard label="PID" value={info.pid || "—"} sub={info.platform} />
      </div>
      <Card>
        <CardHeader><CardTitle>Server Info</CardTitle></CardHeader>
        <CardContent>
          <dl className="grid grid-cols-1 md:grid-cols-2 gap-2 text-sm">
            {[
              ["Hostname",  info.hostname],
              ["Address",   info.address],
              ["Page Root", info.pageroot],
              ["Config",    info.config],
              ["Log",       info.log],
              ["Platform",  info.platform],
              ["Build Date",info.buildDate],
            ].map(([k, v]) => v ? (
              <div key={k} className="flex gap-2">
                <dt className="font-medium text-muted-foreground min-w-24">{k}</dt>
                <dd className="font-mono break-all">{v}</dd>
              </div>
            ) : null)}
          </dl>
        </CardContent>
      </Card>
    </div>
  );
}
