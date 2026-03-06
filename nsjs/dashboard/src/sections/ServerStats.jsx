import React from "react";
import { StatCard } from "../components/StatCard.jsx";
import { ProgressBar } from "../components/ProgressBar.jsx";
import { DataTable } from "../components/DataTable.jsx";
import { Card, CardHeader, CardTitle, CardContent } from "../components/Card.jsx";
import { RingGauge } from "../components/MiniChart.jsx";

function fmtTime(epoch) {
  if (!epoch) return "—";
  return new Date(epoch * 1000).toLocaleString();
}

function fmtUsec(usec) {
  if (usec == null) return "—";
  return (usec / 1e6).toFixed(6) + "s";
}

export function ServerStats({ server, locks, threads, scheduled }) {
  if (!server) return null;
  const t = server.threads || {};
  const max     = t.max || 1;
  const current = t.current || 0;
  const idle    = t.idle || 0;
  const active  = current - idle;

  const urlstats  = server.urlstats  || [];
  const lockList  = locks     || [];
  const threadList= threads   || [];
  const schedList = scheduled || [];

  return (
    <div className="space-y-6">
      {/* Connection stats */}
      <div className="grid grid-cols-2 md:grid-cols-4 gap-4">
        <StatCard label="Active Conns"     value={server.active    ?? "—"} />
        <StatCard label="Queued Conns"     value={server.queued    ?? "—"} />
        <StatCard label="Waiting Conns"    value={server.waiting   ?? "—"} />
        <StatCard label="Keep-alive Conns" value={server.keepalive ?? "—"} />
      </div>

      {/* Thread pool */}
      <Card>
        <CardHeader><CardTitle>Thread Pool</CardTitle></CardHeader>
        <CardContent className="space-y-4">
          <div className="flex items-center gap-6">
            <RingGauge value={max > 0 ? current / max : 0} size={64} warn={0.7} danger={0.9}
              label={`${Math.round(max > 0 ? current / max * 100 : 0)}%`} />
            <div className="flex-1">
              <div className="grid grid-cols-3 md:grid-cols-5 gap-4 text-center text-sm mb-3">
                {[["Min", t.min], ["Max", t.max], ["Current", t.current],
                  ["Idle", t.idle], ["Stopping", t.stopping]].map(([label, val]) => (
                  <div key={label}>
                    <p className="text-muted-foreground text-xs">{label}</p>
                    <p className="text-xl font-bold">{val ?? "—"}</p>
                  </div>
                ))}
              </div>
              <div className="space-y-2">
                <div>
                  <div className="flex justify-between text-xs text-muted-foreground mb-1">
                    <span>Thread utilization</span><span>{current}/{max}</span>
                  </div>
                  <ProgressBar value={current} max={max} />
                </div>
                <div>
                  <div className="flex justify-between text-xs text-muted-foreground mb-1">
                    <span>Active (busy) threads</span><span>{active}/{max}</span>
                  </div>
                  <ProgressBar value={active} max={max} />
                </div>
              </div>
            </div>
          </div>
        </CardContent>
      </Card>

      {/* Total connections + pools */}
      <div className="grid grid-cols-1 md:grid-cols-2 gap-4">
        {typeof server.connections !== "undefined" && (
          <Card>
            <CardHeader><CardTitle>Total Connections</CardTitle></CardHeader>
            <CardContent>
              <p className="text-3xl font-bold">{server.connections}</p>
              <p className="text-sm text-muted-foreground mt-1">Since server start</p>
            </CardContent>
          </Card>
        )}
        {server.pools && server.pools.length > 0 && (
          <Card>
            <CardHeader><CardTitle>Connection Pools</CardTitle></CardHeader>
            <CardContent>
              <ul className="flex flex-wrap gap-2">
                {server.pools.map((p) => (
                  <li key={p} className="rounded-md border px-3 py-1 text-sm font-mono">{p}</li>
                ))}
              </ul>
            </CardContent>
          </Card>
        )}
      </div>

      {/* URL Stats */}
      {urlstats.length > 0 && (
        <Card>
          <CardHeader><CardTitle>URL Stats</CardTitle></CardHeader>
          <CardContent>
            <DataTable
              columns={[
                { key: "url",       label: "URL" },
                { key: "hits",      label: "Hits" },
                { key: "waitSec",   label: "Wait (s)" },
                { key: "openSec",   label: "Open (s)" },
                { key: "closedSec", label: "Closed (s)" },
              ]}
              rows={urlstats.map((u) => ({
                url:       u.url,
                hits:      u.hits,
                waitSec:   u.waitUsec   != null ? (u.waitUsec   / 1e6).toFixed(6) : "—",
                openSec:   u.openUsec   != null ? (u.openUsec   / 1e6).toFixed(6) : "—",
                closedSec: u.closedUsec != null ? (u.closedUsec / 1e6).toFixed(6) : "—",
              }))}
              emptyText="No URL stats."
            />
          </CardContent>
        </Card>
      )}

      {/* Running Threads */}
      {threadList.length > 0 && (
        <Card>
          <CardHeader><CardTitle>Running Threads ({threadList.length})</CardTitle></CardHeader>
          <CardContent>
            <DataTable
              columns={[
                { key: "name",   label: "Name" },
                { key: "tid",    label: "TID" },
                { key: "flags",  label: "Flags" },
                { key: "ctime",  label: "Created" },
                { key: "proc",   label: "Proc" },
              ]}
              rows={threadList.map((th) => ({
                name:  th.name,
                tid:   th.tid,
                flags: th.flags,
                ctime: fmtTime(th.ctime),
                proc:  th.proc,
              }))}
              emptyText="No threads."
            />
          </CardContent>
        </Card>
      )}

      {/* Thread Locks */}
      {lockList.length > 0 && (
        <Card>
          <CardHeader><CardTitle>Thread Locks ({lockList.length})</CardTitle></CardHeader>
          <CardContent>
            <DataTable
              columns={[
                { key: "name",       label: "Name" },
                { key: "owner",      label: "Owner" },
                { key: "nlock",      label: "Locks" },
                { key: "nbusy",      label: "Busy" },
                { key: "contention", label: "Contention" },
              ]}
              rows={lockList.map((lk) => {
                const ratio = lk.nlock > 0 ? lk.nbusy / lk.nlock : 0;
                return {
                  name:       lk.name,
                  owner:      lk.owner || "—",
                  nlock:      lk.nlock,
                  nbusy:      lk.nbusy,
                  contention: lk.nlock > 0 ? (ratio * 100).toFixed(1) + "%" : "—",
                  _ratio:     ratio,
                };
              })}
              rowClassName={(row) =>
                row._ratio > 0.25 ? "bg-red-50 dark:bg-red-950/30" :
                row._ratio > 0.05 ? "bg-amber-50 dark:bg-amber-950/30" : ""
              }
              emptyText="No locks."
            />
          </CardContent>
        </Card>
      )}

      {/* Scheduled Procedures */}
      {schedList.length > 0 && (
        <Card>
          <CardHeader><CardTitle>Scheduled Procedures ({schedList.length})</CardTitle></CardHeader>
          <CardContent>
            <DataTable
              columns={[
                { key: "proc",      label: "Proc" },
                { key: "id",        label: "ID" },
                { key: "interval",  label: "Interval (s)" },
                { key: "nextqueue", label: "Next Run" },
                { key: "lastend",   label: "Last End" },
              ]}
              rows={schedList.map((s) => ({
                proc:      s.proc,
                id:        s.id,
                interval:  s.interval,
                nextqueue: fmtTime(s.nextqueue),
                lastend:   fmtTime(s.lastend),
              }))}
              emptyText="No scheduled procedures."
            />
          </CardContent>
        </Card>
      )}
    </div>
  );
}
