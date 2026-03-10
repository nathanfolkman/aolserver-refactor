import React, { useEffect, useRef } from "react";
import { Card, CardHeader, CardTitle, CardContent } from "../components/Card.jsx";

export function Log({ logTail }) {
  const preRef = useRef(null);

  useEffect(() => {
    if (preRef.current) {
      preRef.current.scrollTop = preRef.current.scrollHeight;
    }
  }, [logTail]);

  return (
    <Card>
      <CardHeader>
        <CardTitle>Server Log (last 8 KB)</CardTitle>
      </CardHeader>
      <CardContent>
        <pre
          ref={preRef}
          className="text-xs font-mono bg-muted rounded p-3 overflow-auto max-h-[600px] whitespace-pre-wrap break-all"
        >
          {logTail || "(empty)"}
        </pre>
      </CardContent>
    </Card>
  );
}
