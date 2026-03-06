// Test the raw format by checking key names
var all = ns.cache.statsAll();
var keys = Object.keys(all);
var first = keys.length > 0 ? all[keys[0]] : {};
ns.conn.returnHtml(200, JSON.stringify({
  cacheKeys: keys,
  firstStatKeys: Object.keys(first),
  firstStatVals: first
}));
