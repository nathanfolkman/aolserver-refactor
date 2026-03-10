import React, { useState } from "react";
import { Card, CardHeader, CardTitle, CardContent } from "../components/Card.jsx";
import { DataTable } from "../components/DataTable.jsx";

function fmtTime(epoch) {
  if (!epoch) return "—";
  return new Date(epoch * 1000).toLocaleString();
}

export function AdpCache({ adpStats }) {
  const rows = adpStats || [];

  if (rows.length === 0) {
    return (
      <Card>
        <CardHeader><CardTitle>ADP Page Cache</CardTitle></CardHeader>
        <CardContent>
          <p className="text-muted-foreground text-sm">No ADP pages cached.</p>
        </CardContent>
      </Card>
    );
  }

  return (
    <Card>
      <CardHeader><CardTitle>ADP Page Cache ({rows.length} entries)</CardTitle></CardHeader>
      <CardContent>
        <DataTable
          columns={[
            { key: "file",    label: "File" },
            { key: "evals",   label: "Evals" },
            { key: "refcnt",  label: "Refs" },
            { key: "size",    label: "Size (B)" },
            { key: "blocks",  label: "Blocks" },
            { key: "scripts", label: "Scripts" },
            { key: "mtime",   label: "Modified" },
          ]}
          rows={rows.map((e) => ({
            file:    e.file,
            evals:   e.evals,
            refcnt:  e.refcnt,
            size:    e.size,
            blocks:  e.blocks,
            scripts: e.scripts,
            mtime:   fmtTime(e.mtime),
          }))}
          emptyText="No ADP pages cached."
        />
      </CardContent>
    </Card>
  );
}
