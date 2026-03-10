import React from "react";
import { StatCard } from "../components/StatCard.jsx";
import { Card, CardHeader, CardTitle, CardContent } from "../components/Card.jsx";
import { ProgressBar } from "../components/ProgressBar.jsx";
import { RingGauge } from "../components/MiniChart.jsx";
import { EChartsWrapper } from "../components/EChartsWrapper.jsx";

function fmtBytes(b) {
  if (b == null) return "—";
  if (b >= 1024 * 1024 * 1024) return (b / (1024 * 1024 * 1024)).toFixed(2) + " GB";
  if (b >= 1024 * 1024)        return (b / (1024 * 1024)).toFixed(2) + " MB";
  if (b >= 1024)               return (b / 1024).toFixed(1) + " KB";
  return b + " B";
}

export function Memory({ memory, memorySizeClasses, history }) {
  if (!memory) return null;

  if (!memory.available) {
    return (
      <p className="text-muted-foreground text-sm">
        tcmalloc stats not available (server not built with tcmalloc).
      </p>
    );
  }

  const allocated = memory["generic.current_allocated_bytes"];
  const heapSize  = memory["generic.heap_size"];
  const phFree    = memory["tcmalloc.pageheap_free_bytes"];
  const phUnmap   = memory["tcmalloc.pageheap_unmapped_bytes"];
  const tcMax     = memory["tcmalloc.max_total_thread_cache_bytes"];
  const tcCur     = memory["tcmalloc.current_total_thread_cache_bytes"];
  const ccFree    = memory["tcmalloc.central_cache_free_bytes"];
  const xfFree    = memory["tcmalloc.transfer_cache_free_bytes"];
  const thFree    = memory["tcmalloc.thread_cache_free_bytes"];

  const fragmented = heapSize != null && allocated != null
    ? heapSize - allocated : null;
  const heapRatio   = heapSize > 0 ? allocated / heapSize : 0;
  const tcRatio     = tcMax   > 0 ? tcCur    / tcMax    : 0;

  return (
    <div className="space-y-6">
      <div className="grid grid-cols-2 md:grid-cols-4 gap-4">
        <StatCard label="Allocated"      value={fmtBytes(allocated)}   sub="in use by app" />
        <StatCard label="Heap Size"      value={fmtBytes(heapSize)}    sub="total from OS" />
        <StatCard label="Page Heap Free" value={fmtBytes(phFree)}      sub="ready to reuse" />
        <StatCard label="Fragmentation"  value={fmtBytes(fragmented)}  sub="heap − allocated" />
      </div>

      {heapSize > 0 && (
        <Card>
          <CardHeader><CardTitle>Heap Utilization</CardTitle></CardHeader>
          <CardContent className="space-y-4">
            <div className="flex items-center gap-6">
              <RingGauge value={heapRatio} size={64} warn={0.7} danger={0.9}
                label={`${Math.round(heapRatio * 100)}%`} />
              <div className="flex-1 space-y-3">
                <div>
                  <div className="flex justify-between text-xs text-muted-foreground mb-1">
                    <span>Allocated / Heap</span>
                    <span>{fmtBytes(allocated)} / {fmtBytes(heapSize)}</span>
                  </div>
                  <ProgressBar value={allocated} max={heapSize} />
                </div>
                {tcMax > 0 && (
                  <div>
                    <div className="flex justify-between text-xs text-muted-foreground mb-1">
                      <span>Thread Cache Used / Max</span>
                      <span>{fmtBytes(tcCur)} / {fmtBytes(tcMax)}</span>
                    </div>
                    <ProgressBar value={tcCur} max={tcMax} />
                  </div>
                )}
              </div>
            </div>
          </CardContent>
        </Card>
      )}

      <Card>
        <CardHeader><CardTitle>tcmalloc Internal Caches</CardTitle></CardHeader>
        <CardContent>
          <dl className="grid grid-cols-2 md:grid-cols-3 gap-3 text-sm">
            {[
              ["Central Cache Free",   ccFree],
              ["Transfer Cache Free",  xfFree],
              ["Thread Cache Free",    thFree],
              ["Page Heap Unmapped",   phUnmap],
              ["Thread Cache (cur)",   tcCur],
              ["Thread Cache (max)",   tcMax],
            ].map(([label, val]) => (
              <div key={label}>
                <dt className="text-muted-foreground text-xs">{label}</dt>
                <dd className="font-mono font-medium">{fmtBytes(val)}</dd>
              </div>
            ))}
          </dl>
        </CardContent>
      </Card>

      {/* Per-size-class breakdown */}
      <Card>
        <CardHeader><CardTitle>Size Class Breakdown</CardTitle></CardHeader>
        <CardContent>
          {(!memorySizeClasses || memorySizeClasses.length === 0) ? (
            <p className="text-muted-foreground text-sm">
              Size class data unavailable. Note: tcmalloc does not expose per-thread
              allocation breakdown.
            </p>
          ) : (() => {
            const top = [...memorySizeClasses]
              .sort((a, b) => b.freeBytes - a.freeBytes)
              .slice(0, 20);
            return (
              <EChartsWrapper
                style={{ width: "100%", height: Math.max(200, top.length * 20) }}
                option={{
                  animation: false,
                  grid: { top: 8, bottom: 28, left: 56, right: 12 },
                  tooltip: {
                    trigger: "axis",
                    formatter: (params) => {
                      const p = params[0];
                      return `${p.name}: ${fmtBytes(p.value)}`;
                    },
                  },
                  xAxis: { type: "value", axisLabel: { fontSize: 10,
                    formatter: (v) => fmtBytes(v) } },
                  yAxis: { type: "category",
                    data: top.map((c) => `${c.size}B`),
                    axisLabel: { fontSize: 10 } },
                  series: [{
                    type: "bar",
                    data: top.map((c) => c.freeBytes),
                    itemStyle: { color: "#3b82f6" },
                  }],
                }}
              />
            );
          })()}
        </CardContent>
      </Card>

      {/* Memory over time */}
      {history && history.length > 1 && (
        <Card>
          <CardHeader><CardTitle>Memory Over Time</CardTitle></CardHeader>
          <CardContent>
            <EChartsWrapper
              style={{ width: "100%", height: 200 }}
              option={{
                animation: false,
                grid: { top: 24, bottom: 28, left: 60, right: 12 },
                tooltip: { trigger: "axis",
                  formatter: (params) => params.map(
                    (p) => `${p.seriesName}: ${fmtBytes(p.value)}`
                  ).join("<br/>") },
                legend: { top: 0, right: 0, textStyle: { fontSize: 11 } },
                xAxis: { type: "category",
                  data: history.map((h) => new Date(h.t).toLocaleTimeString()),
                  axisLabel: { fontSize: 10 } },
                yAxis: { type: "value", axisLabel: { fontSize: 10,
                  formatter: (v) => fmtBytes(v) } },
                series: [
                  { name: "Allocated", type: "line", smooth: true, symbol: "none",
                    lineStyle: { color: "#3b82f6" }, itemStyle: { color: "#3b82f6" },
                    data: history.map((h) => h.memAlloc) },
                ],
              }}
            />
          </CardContent>
        </Card>
      )}
    </div>
  );
}
