#!/usr/bin/env node
/**
 * build.mjs — esbuild script for AOLserver stats dashboard
 *
 * Produces:
 *   dist/server-bundle.js  — IIFE with globalThis.__dashRender(data) for V8/SSR
 *   dist/client-bundle.js  — IIFE for browser hydration
 *   dist/styles.css        — Tailwind pre-built CSS
 */

import * as esbuild from "esbuild";
import { execSync } from "child_process";
import { mkdirSync } from "fs";

mkdirSync("dist", { recursive: true });

const define = {
  "process.env.NODE_ENV": '"production"',
};

/*
 * TextEncoder / TextDecoder polyfill for V8 embedder (no DOM globals).
 * Must run as a banner — BEFORE React initialises — because esbuild hoists
 * import statements above any module-level code in the entry file.
 */
const v8Polyfill = `
if(typeof globalThis.TextEncoder==="undefined"){globalThis.TextEncoder=class TextEncoder{encode(s){const b=[];for(let i=0;i<s.length;i++){let c=s.charCodeAt(i);if(c<0x80)b.push(c);else if(c<0x800)b.push(0xc0|(c>>6),0x80|(c&0x3f));else b.push(0xe0|(c>>12),0x80|((c>>6)&0x3f),0x80|(c&0x3f));}return new Uint8Array(b);}}}
if(typeof globalThis.TextDecoder==="undefined"){globalThis.TextDecoder=class TextDecoder{decode(buf){const a=buf instanceof Uint8Array?buf:new Uint8Array(buf);let s="";for(let i=0;i<a.length;i++)s+=String.fromCharCode(a[i]);return s;}}}
`;

/* ---------- Server bundle (SSR in V8) ---------- */
await esbuild.build({
  entryPoints: ["src/server-entry.jsx"],
  bundle: true,
  minify: true,
  platform: "browser",
  format: "iife",
  globalName: "__dashBundle",
  define,
  banner: { js: v8Polyfill },
  /* After the IIFE closes, assign the render function to globalThis */
  footer: {
    js: "if (typeof __dashBundle !== 'undefined') { globalThis.__dashRender = __dashBundle.render; }",
  },
  outfile: "dist/server-bundle.js",
});
console.log("✓ dist/server-bundle.js");

/* ---------- Client bundle (browser hydration) ---------- */
await esbuild.build({
  entryPoints: ["src/client-entry.jsx"],
  bundle: true,
  minify: true,
  platform: "browser",
  format: "iife",
  define,
  outfile: "dist/client-bundle.js",
});
console.log("✓ dist/client-bundle.js");

/* ---------- Tailwind CSS ---------- */
try {
  execSync(
    "npx tailwindcss -i src/styles.css -o dist/styles.css --minify",
    { stdio: "inherit" }
  );
  console.log("✓ dist/styles.css");
} catch (e) {
  console.error("Tailwind CSS build failed:", e.message);
  process.exit(1);
}
