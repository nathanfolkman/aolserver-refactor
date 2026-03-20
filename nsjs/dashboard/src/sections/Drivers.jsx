import React from "react";
import { Card, CardHeader, CardTitle, CardContent } from "../components/Card.jsx";

export function Drivers({ drivers, http2, http3 }) {
  const hasDriverList = Array.isArray(drivers) && drivers.length > 0;
  const hasHttp2 =
    http2 != null && typeof http2 === "object" && Object.keys(http2).length > 0;
  const hasHttp3 =
    http3 != null && typeof http3 === "object" && Object.keys(http3).length > 0;

  if (!hasDriverList && !hasHttp2 && !hasHttp3) {
    return (
      <p className="text-muted-foreground text-sm">No drivers or HTTP/2/3 stats reported.</p>
    );
  }

  return (
    <div className="space-y-4">
      {Array.isArray(drivers) && drivers.length > 0 ? (
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
      ) : drivers != null && typeof drivers === "object" && !Array.isArray(drivers) ? (
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
      ) : null}
      {hasHttp2 ? (
        <Card>
          <CardHeader><CardTitle>HTTP/2 (global)</CardTitle></CardHeader>
          <CardContent>
            <p className="text-xs text-muted-foreground mb-3">
              Process-wide counters for TLS ALPN <code className="font-mono">h2</code> (nghttp2).
            </p>
            <dl className="grid grid-cols-2 gap-2 text-sm">
              {Object.entries(http2).map(([k, v]) => (
                <div key={k} className="flex gap-2">
                  <dt className="font-medium text-muted-foreground">{k}</dt>
                  <dd className="font-mono">{String(v)}</dd>
                </div>
              ))}
            </dl>
          </CardContent>
        </Card>
      ) : null}
      {hasHttp3 ? (
        <Card>
          <CardHeader><CardTitle>HTTP/3 (global)</CardTitle></CardHeader>
          <CardContent>
            <p className="text-xs text-muted-foreground mb-3">
              Process-wide counters for QUIC + HTTP/3 (ngtcp2/nghttp3), when enabled in nsssl.
            </p>
            <dl className="grid grid-cols-2 gap-2 text-sm">
              {Object.entries(http3).map(([k, v]) => (
                <div key={k} className="flex gap-2">
                  <dt className="font-medium text-muted-foreground">{k}</dt>
                  <dd className="font-mono">{String(v)}</dd>
                </div>
              ))}
            </dl>
          </CardContent>
        </Card>
      ) : null}
    </div>
  );
}
