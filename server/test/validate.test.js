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

test("keeps the match to mxbmrp3's crash record, scrubbed like the rest", () => {
  const r = good();
  r.mxbmrp3 = { crash_log: true, dump: true, fault: "mxbikes.exe+0x1a29ae", stack: ["mxbikes.exe+0x1a29ae", "C:\\Users\\someone\\a.dll+0x1"] };
  const mx = cleanReport(r).report.body.mxbmrp3;
  assert.equal(mx.fault, "mxbikes.exe+0x1a29ae");
  assert.equal(mx.stack, undefined, "a stack with anything but locations is dropped whole");
});

test("mxbmrp3 and freeze keep only the plugin's own fields", () => {
  const r = good();
  r.mxbmrp3 = { crash_log: true, fault: "mxbikes.exe+0x1a29ae", stack: ["mxbikes.exe+0x1"], rider: "someone", log_name: "mxbmrp3_crash_20261108_143507_1.log" };
  r.freeze = { froze: true, hung_s: 42, hang_type: "Unknown", note: "someone", hung_at_end: "yes", time_utc: "2026-11-07T22:43:53Z" };
  const b = cleanReport(r).report.body;
  assert.deepEqual(b.mxbmrp3, { crash_log: true, fault: "mxbikes.exe+0x1a29ae", stack: ["mxbikes.exe+0x1"] });
  assert.deepEqual(b.freeze, { froze: true, hung_s: 42, hang_type: "Unknown", time_utc: "2026-11-07T22:43:53Z" });
  r.freeze = "froze";
  assert.equal(cleanReport(r).report.body.freeze, undefined);
});

test("deeply nested junk in the new members is dropped, and the report is still kept", () => {
  let deep = {};
  for (let i = 0; i < 20000; i++) deep = { a: deep };
  const r = good();
  r.freeze = { froze: true, junk: deep };
  r.mxbmrp3 = { crash_log: true, stack: [deep], fault: deep };
  const { report, error } = cleanReport(r);
  assert.equal(error, undefined);
  assert.deepEqual(report.body.freeze, { froze: true });
  assert.deepEqual(report.body.mxbmrp3, { crash_log: true });
});

test("keeps the freeze evidence of an unclean exit", () => {
  const r = good();
  r.kind = "unclean_exit";
  r.signature = "unclean-exit";
  r.freeze = { froze: true, record: true, game_quiet_s: 44, hung_at_end: true, hung_s: 42, windows_hang_event: true };
  assert.deepEqual(cleanReport(r).report.body.freeze, r.freeze);
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
