import React from "react";

/**
 * RingGauge — small SVG donut chart showing a ratio (0–1).
 * Colors: blue (ok), amber (warn), red (danger).
 */
export function RingGauge({ value = 0, size = 44, warn = 0.6, danger = 0.85, label }) {
  const r     = (size - 6) / 2;
  const circ  = 2 * Math.PI * r;
  const fill  = Math.max(0, Math.min(1, value));
  const dash  = fill * circ;
  const cx    = size / 2;

  const color = fill >= danger ? "#ef4444"
              : fill >= warn   ? "#f59e0b"
              : "#3b82f6";

  return (
    <svg width={size} height={size} viewBox={`0 0 ${size} ${size}`} className="shrink-0">
      {/* background ring */}
      <circle cx={cx} cy={cx} r={r} fill="none"
        stroke="currentColor" strokeWidth={5}
        className="text-muted opacity-20" />
      {/* value arc */}
      <circle cx={cx} cy={cx} r={r} fill="none"
        stroke={color} strokeWidth={5}
        strokeDasharray={`${dash} ${circ}`}
        strokeLinecap="round"
        transform={`rotate(-90 ${cx} ${cx})`} />
      {label !== undefined && (
        <text x={cx} y={cx + 1} textAnchor="middle" dominantBaseline="middle"
          fontSize={size * 0.22} fill={color} fontWeight="600">
          {label}
        </text>
      )}
    </svg>
  );
}

/**
 * SparkBars — tiny inline bar chart from an array of numbers.
 */
export function SparkBars({ values = [], width = 60, height = 24, color = "#3b82f6" }) {
  if (!values.length) return null;
  const max  = Math.max(...values, 1);
  const bw   = Math.max(1, Math.floor(width / values.length) - 1);
  const bars = values.map((v, i) => {
    const bh = Math.max(1, Math.round((v / max) * height));
    return (
      <rect key={i}
        x={i * (bw + 1)} y={height - bh}
        width={bw} height={bh}
        fill={color} rx={1} />
    );
  });
  return (
    <svg width={width} height={height} viewBox={`0 0 ${width} ${height}`} className="shrink-0">
      {bars}
    </svg>
  );
}

/**
 * MiniBar — single horizontal bar showing value/max.
 */
export function MiniBar({ value = 0, max = 1, warn = 0.6, danger = 0.85, width = 64, height = 6 }) {
  const fill  = max > 0 ? Math.min(1, value / max) : 0;
  const color = fill >= danger ? "#ef4444"
              : fill >= warn   ? "#f59e0b"
              : "#3b82f6";
  return (
    <svg width={width} height={height} viewBox={`0 0 ${width} ${height}`} className="shrink-0">
      <rect x={0} y={0} width={width} height={height} fill="currentColor"
        className="text-muted opacity-20" rx={3} />
      <rect x={0} y={0} width={fill * width} height={height} fill={color} rx={3} />
    </svg>
  );
}
