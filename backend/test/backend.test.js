import assert from "node:assert/strict";
import { afterEach, test } from "node:test";
import { createServer, configFromEnv } from "../server.js";

const servers = [];

afterEach(async () => {
  while (servers.length) await new Promise((resolve) => servers.pop().close(resolve));
});

async function start(config = {}) {
  const server = createServer(config);
  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  servers.push(server);
  return `http://127.0.0.1:${server.address().port}`;
}


test("configFromEnv returns frozen config with defaults", () => {
  const config = configFromEnv({ PORT: "9000", HOST: "0.0.0.0" });
  assert.equal(config.port, 9000);
  assert.equal(config.host, "0.0.0.0");
  assert.equal(Object.isFrozen(config), true);
});

test("configFromEnv rejects invalid PORT", () => {
  assert.throws(() => configFromEnv({ PORT: "99999" }), /PORT/);
  assert.throws(() => configFromEnv({ PORT: "abc" }), /PORT/);
});

test("GET /healthz returns ok", async () => {
  const base = await start();
  const res = await fetch(`${base}/healthz`);
  assert.equal(res.status, 200);
  const body = await res.json();
  assert.equal(body.ok, true);
});

test("unknown route returns 404", async () => {
  const base = await start();
  const res = await fetch(`${base}/v1/query`, { method: "POST" });
  assert.equal(res.status, 404);
  const body = await res.json();
  assert.equal(body.error.code, "NOT_FOUND");
});

test("GET /youtube/stream without query returns 400", async () => {
  const base = await start();
  const res = await fetch(`${base}/youtube/stream`);
  assert.equal(res.status, 400);
  const body = await res.json();
  assert.equal(body.error.code, "INVALID_QUERY");
});

test("GET /youtube/stream with blank query returns 400", async () => {
  const base = await start();
  const res = await fetch(`${base}/youtube/stream?q=   `);
  assert.equal(res.status, 400);
  const body = await res.json();
  assert.equal(body.error.code, "INVALID_QUERY");
});
