/**
 * server-entry.jsx — SSR entry point for V8/ReactDOM.renderToString
 *
 * Exported as globalThis.__dashRender(data) by build.mjs.
 * Called from stats.jsadp to produce the initial HTML.
 *
 * Note: ReactDOM.renderToString is synchronous — no await needed.
 */

import React from "react";
import { renderToString } from "react-dom/server";
import { App } from "./App.jsx";

export function render(data) {
  return renderToString(<App data={data} />);
}
