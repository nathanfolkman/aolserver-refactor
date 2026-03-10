import React from "react";
import { StatCard } from "../components/StatCard.jsx";
import { Card, CardHeader, CardTitle, CardContent } from "../components/Card.jsx";
import { RingGauge } from "../components/MiniChart.jsx";
import { EChartsWrapper } from "../components/EChartsWrapper.jsx";

function avgMs(usec, count) {
  if (!count) return "—";
  return ((usec / count) / 1000).toFixed(2) + " ms";
}

export function JsEngine({ jsStats, history }) {
  if (!jsStats) return null;
  const s = jsStats;
  const hist = history || [];

  const totalLookups = (s.cacheHits ?? 0) + (s.cacheMisses ?? 0);
  const hitRate = totalLookups > 0 ? s.cacheHits / totalLookups : 0;

  return (
    <div className="space-y-6">
      <div className="grid grid-cols-2 md:grid-cols-4 gap-4">
        <StatCard label="Total Requests"    value={s.totalRequests}    />
        <StatCard label="Active Isolates"   value={s.activeIsolates}   />
        <StatCard label="Avg Exec Time"     value={avgMs(s.totalExecUsec, s.totalRequests)} />
        <StatCard label="Context Creations" value={s.contextCreations} sub={`Avg: ${avgMs(s.totalContextUsec, s.contextCreations)}`} />
      </div>

      <Card>
        <CardHeader><CardTitle>Script Cache</CardTitle></CardHeader>
        <CardContent>
          <div className="flex items-center gap-8">
            <RingGauge value={hitRate} size={72} warn={0.5} danger={0.25}
              label={`${Math.round(hitRate * 100)}%`} />
            <dl className="grid grid-cols-2 md:grid-cols-4 gap-6 text-sm flex-1">
              {[
                ["Hits",           s.cacheHits],
                ["Misses",         s.cacheMisses],
                ["Invalidations",  s.cacheInvalidations],
                ["Cached Scripts", (s.cachedScripts ?? 0) + (s.cachedAdpScripts ?? 0)],
              ].map(([label, val]) => (
                <div key={label}>
                  <dt className="text-muted-foreground text-xs">{label}</dt>
                  <dd className="text-2xl font-bold">{val ?? "—"}</dd>
                </div>
              ))}
            </dl>
          </div>
        </CardContent>
      </Card>

      <div className="grid grid-cols-2 md:grid-cols-4 gap-4">
        <StatCard label="Compile Errors"     value={s.compileErrors} />
        <StatCard label="Runtime Errors"     value={s.runtimeErrors} />
        <StatCard label="Avg Compile Time"   value={avgMs(s.totalCompileUsec, s.cacheMisses)} />
        <StatCard label="Cache Invalidations" value={s.cacheInvalidations} />
      </div>

      {hist.length > 1 && (
        <Card>
          <CardHeader><CardTitle>Request History</CardTitle></CardHeader>
          <CardContent>
            <EChartsWrapper
              style={{ width: "100%", height: 160 }}
              option={{
                animation: false,
                grid: { top: 8, bottom: 28, left: 40, right: 12 },
                tooltip: { trigger: "axis" },
                xAxis: { type: "category",
                  data: hist.map((h) => new Date(h.t).toLocaleTimeString()),
                  axisLabel: { fontSize: 10 } },
                yAxis: { type: "value", minInterval: 1, axisLabel: { fontSize: 10 } },
                series: [{
                  name: "Active",
                  type: "line",
                  smooth: true,
                  symbol: "none",
                  lineStyle: { color: "#8b5cf6" },
                  itemStyle: { color: "#8b5cf6" },
                  data: hist.map((h) => h.active ?? 0),
                }],
              }}
            />
          </CardContent>
        </Card>
      )}
    </div>
  );
}
