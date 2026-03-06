import React from "react";
import { Card, CardHeader, CardTitle, CardContent } from "../components/Card.jsx";

export function Drivers({ drivers }) {
  if (!drivers || drivers.length === 0) {
    return (
      <p className="text-muted-foreground text-sm">No drivers reported.</p>
    );
  }

  return (
    <div className="space-y-4">
      {Array.isArray(drivers) ? (
        <Card>
          <CardHeader><CardTitle>Network Drivers</CardTitle></CardHeader>
          <CardContent>
            <ul className="flex flex-wrap gap-2">
              {drivers.map((d) => (
                <li key={d} className="rounded-md border px-3 py-1 text-sm font-mono">
                  {d}
                </li>
              ))}
            </ul>
          </CardContent>
        </Card>
      ) : (
        <Card>
          <CardHeader><CardTitle>Driver Info</CardTitle></CardHeader>
          <CardContent>
            <dl className="grid grid-cols-2 gap-2 text-sm">
              {Object.entries(drivers).map(([k, v]) => (
                <div key={k} className="flex gap-2">
                  <dt className="font-medium text-muted-foreground">{k}</dt>
                  <dd className="font-mono">{String(v)}</dd>
                </div>
              ))}
            </dl>
          </CardContent>
        </Card>
      )}
    </div>
  );
}
