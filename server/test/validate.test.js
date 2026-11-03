import test from "node:test";
import assert from "node:assert/strict";
import { badge, cleanReport, cleanCounts, issueBody, issueTitle, issueWorthy } from "../src/validate.js";

const good = () => ({
  schema: 1, kind: "crash", id: "crash-6A21833D-20261101-163719-34888", signature: "mxbikes.exe+0x11d753",
  install_id: "0123456789abcdef0123456789abcdef", plugin: { name: "mxbfix", version: "0.1.6" }, game: { build: "0x6a21833d" },
  context: { server: "Test Server", track: "Test Track" },
  modules: [{ name: "a.dll", path: "C:\\Users\\someone\\a.dll", base: "0x1", size: "0x2", timestamp: "0x3" }],
  dump_file: "x.dmp", rider_name: "someone", exception: { name: "ACCESS_VIOLATION" },
});

test("keeps what debugging needs, including the server name", () => {
  const { report } = cleanReport(good());
  assert.equal(report.server, "Test Server");
  assert.equal(report.track, "Test Track");
  assert.equal(report.body.exception.name, "ACCESS_VIOLATION");
});

test("removes paths, dumps and anything not on the list, even if a sender adds them", () => {
  const text = JSON.stringify(cleanReport(good()).report.body);
  for (const bad of ["Users", "someone", "x.dmp", "rider_name", "dump_file", "path"]) assert.ok(!text.includes(bad), bad);
});

for (const [field, value] of [["schema", 2], ["kind", "x"], ["signature", "@someone hello"], ["signature", "a+0x1\n# spam"],
  ["install_id", "short"], ["id", "a b"]]) {
  test(`rejects ${field}=${JSON.stringify(value)}`, () => {
    const r = good();
    r[field] = value;
    assert.ok(cleanReport(r).error);
  });
}

test("rejects bad versions and non-objects", () => {
  const r = good();
  r.plugin.version = "1.0 <b>";
  assert.ok(cleanReport(r).error);
  assert.ok(cleanReport([]).error);
  assert.ok(cleanReport(null).error);
});

test("unclean exits use the previous session's server", () => {
  const r = good();
  r.kind = "unclean_exit";
  r.signature = "unclean-exit";
  delete r.context;
  r.previous_session = { server: "S2", track: "T2" };
  assert.equal(cleanReport(r).report.server, "S2");
});

test("counts: only known fixes, sane numbers", () => {
  assert.deepEqual(cleanCounts({ schema: 1, counts: { "gate-vertex-pointer": 3, "ground-nan-guard": 0, other: 9 } }).counts, { "gate-vertex-pointer": 3 });
  assert.ok(cleanCounts({ schema: 1, counts: { "gate-vertex-pointer": -1 } }).error);
  assert.ok(cleanCounts({ schema: 1, counts: { "gate-vertex-pointer": 101 } }).error);
  assert.ok(cleanCounts({ schema: 1, counts: { "gate-vertex-pointer": 1.5 } }).error);
});

test("badge is what shields.io expects", () => {
  assert.deepEqual(badge(14), { schemaVersion: 1, label: "crashes prevented", message: "14", color: "39ff14" });
});

test("public issue text never includes server, track or install ID", () => {
  const row = { signature: "mxbikes.exe+0x1", reports: 3, first_day: "2026-11-03", last_day: "2026-11-03", last_version: "0.1.6", last_build: "0x6a21833d" };
  const text = issueTitle(row.signature) + issueBody(row, 2);
  assert.ok(text.includes("mxbikes.exe+0x1"));
  assert.ok(!/server|track|install/i.test(text));
});

test("accepts crashes outside any module and modules with brackets", () => {
  for (const sig of ["0x12345678", "some (x86) module.dll+0x10"]) {
    const r = good();
    r.signature = sig;
    assert.ok(cleanReport(r).report, sig);
  }
});

test("removes paths nested anywhere, whatever the field is called", () => {
  const r = good();
  r.context = { server: "S", track: "T", path: "x", note: "C:\\Users\\someone\\file", deep: [{ path: "y", a: "\\\\host\\share" }] };
  const text = JSON.stringify(cleanReport(r).report.body);
  for (const bad of ["someone", "host", '"path"']) assert.ok(!text.includes(bad), bad);
  assert.ok(text.includes('"track":"T"'));
});

test("issues only open for real modules, never bare addresses or unclean exits", () => {
  assert.ok(issueWorthy("mxbikes.exe+0x1f1923"));
  assert.ok(issueWorthy("nvoglv64.dll+0x10"));
  for (const sig of ["0x12345678", "unclean-exit", "fake+0x1"]) assert.ok(!issueWorthy(sig), sig);
});
