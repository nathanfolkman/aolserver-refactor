import React from "react";
import { DataTable } from "../components/DataTable.jsx";
import { Card, CardHeader, CardTitle, CardContent } from "../components/Card.jsx";
import { RingGauge } from "../components/MiniChart.jsx";

export function Caches({ caches }) {
  /* caches is an object: { cacheName: { entries, hits, misses, hitrate, flushed } } */
  const entries = caches && typeof caches === "object" && !Array.isArray(caches)
    ? Object.entries(caches)
    : [];

  if (entries.length === 0) {
    return <p className="text-muted-foreground text-sm">No named caches configured.</p>;
  }

  const rows = entries.map(([name, s]) => ({
    name,
    entries:  s.entries ?? 0,
    hits:     s.hits    ?? 0,
    misses:   s.misses  ?? 0,
    flushed:  s.flushed ?? 0,
    hitrate:  s.hitrate ?? 0,
    _hitrateRaw: typeof s.hitrate === "number" ? s.hitrate / 100 : 0,
  }));

  return (
    <div className="space-y-6">
      <Card>
        <CardHeader><CardTitle>Caches ({entries.length})</CardTitle></CardHeader>
        <CardContent>
          <DataTable
            columns={[
              { key: "name",    label: "Name" },
              { key: "entries", label: "Entries" },
              { key: "hits",    label: "Hits" },
              { key: "misses",  label: "Misses" },
              { key: "flushed", label: "Flushed" },
              { key: "_gauge",  label: "Hit Rate" },
            ]}
            rows={rows.map((r) => ({
              ...r,
              _gauge: (
                <div className="flex items-center gap-2">
                  <RingGauge value={r._hitrateRaw} size={32} warn={0.5} danger={0.2} label="" />
                  <span className="text-xs">{r.hitrate}%</span>
                </div>
              ),
            }))}
            emptyText="No caches."
          />
        </CardContent>
      </Card>
    </div>
  );
}
