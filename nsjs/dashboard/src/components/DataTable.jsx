import React, { useState } from "react";

export function DataTable({ columns, rows, emptyText = "No data.", rowClassName }) {
  const [sortCol, setSortCol] = useState(null);
  const [sortAsc, setSortAsc] = useState(true);

  function handleSort(col) {
    if (sortCol === col) setSortAsc(!sortAsc);
    else { setSortCol(col); setSortAsc(true); }
  }

  const sorted = sortCol
    ? [...rows].sort((a, b) => {
        const av = a[sortCol]; const bv = b[sortCol];
        if (av == null) return 1; if (bv == null) return -1;
        const cmp = typeof av === "number" ? av - bv : String(av).localeCompare(String(bv));
        return sortAsc ? cmp : -cmp;
      })
    : rows;

  return (
    <div className="overflow-x-auto rounded-lg border">
      <table className="w-full text-sm">
        <thead className="bg-muted text-muted-foreground">
          <tr>
            {columns.map((c) => (
              <th
                key={c.key}
                className="px-4 py-2 text-left font-medium cursor-pointer select-none hover:bg-accent"
                onClick={() => handleSort(c.key)}
              >
                {c.label}
                {sortCol === c.key ? (sortAsc ? " ↑" : " ↓") : ""}
              </th>
            ))}
          </tr>
        </thead>
        <tbody>
          {sorted.length === 0 ? (
            <tr>
              <td colSpan={columns.length} className="px-4 py-6 text-center text-muted-foreground">
                {emptyText}
              </td>
            </tr>
          ) : (
            sorted.map((row, i) => (
              <tr key={i} className={`border-t hover:bg-muted/50 ${rowClassName ? rowClassName(row) : ""}`}>
                {columns.map((c) => (
                  <td key={c.key} className="px-4 py-2">
                    {row[c.key] ?? "—"}
                  </td>
                ))}
              </tr>
            ))
          )}
        </tbody>
      </table>
    </div>
  );
}
