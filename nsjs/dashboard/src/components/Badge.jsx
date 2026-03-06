import React from "react";

const variants = {
  default:     "bg-primary text-primary-foreground",
  secondary:   "bg-secondary text-secondary-foreground",
  destructive: "bg-destructive text-destructive-foreground",
  outline:     "border border-border text-foreground",
};

export function Badge({ children, variant = "default", className = "" }) {
  return (
    <span
      className={`inline-flex items-center rounded-full px-2.5 py-0.5 text-xs font-semibold transition-colors ${variants[variant] || variants.default} ${className}`}
    >
      {children}
    </span>
  );
}
