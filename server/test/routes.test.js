// The real Worker against a real SQLite database, answering the way the plugin needs: 2xx files a report as sent, 400 files it as refused
// for good, anything else keeps it and tries again at the next game start.
import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { DatabaseSync } from "node:sqlite";
import worker, { addressKey } from "../src/index.js";
import { DAILY, DAILY_COUNT_CEILING, MAX_REPORTS_PER_SIGNATURE_PER_DAY } from "../src/validate.js";

class Statement {
  constructor(db, sql, params = []) { Object.assign(this, { db, sql, params }); }
  bind(...params) { return new Statement(this.db, this.sql, params); }
  run_() {
    this.db.log.push(this.sql);
    if (this.db.failOn?.(this.sql)) throw new Error("D1_ERROR: injected failure");
    const r = this.db.sqlite.prepare(this.sql).run(...this.params);
    return { meta: { changes: Number(r.changes) } };
  }
  async run() { return this.run_(); }
  async first() {
    this.db.log.push(this.sql);
    return this.db.sqlite.prepare(this.sql).get(...this.params) ?? null;
  }
  async all() {
    this.db.log.push(this.sql);
    return { results: this.db.sqlite.prepare(this.sql).all(...this.params) };
  }
}

class Database {
  constructor() {
    this.sqlite = new DatabaseSync(":memory:");
    this.sqlite.exec(readFileSync(new URL("../schema.sql", import.meta.url), "utf8"));
    this.log = [];
  }
  prepare(sql) { return new Statement(this, sql); }
  // A batch is one transaction, as Cloudflare documents.
  async batch(statements) {
    this.sqlite.exec("BEGIN");
    try {
      const out = statements.map((s) => s.run_());
      this.sqlite.exec("COMMIT");
      return out;
    } catch (e) {
      this.sqlite.exec("ROLLBACK");
      throw e;
    }
  }
  value(sql, ...params) { return Object.values(this.sqlite.prepare(sql).get(...params))[0]; }
}

const open = { limit: async () => ({ success: true }) };
function makeEnv() {
  const seenKeys = [];
  const recording = { limit: async ({ key }) => { seenKeys.push(key); return { success: true }; } };
  return { DB: new Database(), RATE_KEY: "test-secret", GITHUB_REPO: "x/y", GITHUB_TOKEN: "t", PER_INSTALL: open, OVERALL: open, REPORT_PER_ADDRESS: recording, COUNTS_PER_ADDRESS: recording, READ_PER_ADDRESS: recording, seenKeys };
}

let serial = 0;
const report = (over = {}) => ({
  schema: 1, kind: "crash", id: `crash-${++serial}`, signature: "mxbikes.exe+0x1a29ae", install_id: (serial % 16).toString(16).repeat(32),
  plugin: { name: "mxbfix", version: "0.1.7" }, game: { build: "0x6a21833d", image_size: 1 }, known: null,
  exception: { name: "ACCESS_VIOLATION", address: "0x1", target: "0x2", code_at_fault: "f30f10", code_before_fault: "aabb" },
  diagnosis: [{ id: "float_to_int_overflow", text: "x" }], registers: { rax: "0x70617373776f7264", rip: "0x1" }, xmm_low: ["0x70617373776f7264"],
  register_targets: [{ reg: "rax", state: "commit" }], stack: ["mxbikes.exe+0x1a29ae"], context: { server: "S", track: "T" },
  modules: [{ name: "a.dll", path: "C:\\Users\\me\\a.dll", base: "0x1", size: "0x2", timestamp: "0x3" }], ...over,
});
const post = (env, path, value, address = "203.0.113.7") =>
  worker.fetch(new Request(`https://x${path}`, { method: "POST", body: JSON.stringify(value), headers: { "cf-connecting-ip": address } }), env);
const send = (env, value, address) => post(env, "/v1/report", value, address);
const get = (env, path) => worker.fetch(new Request(`https://x${path}`, { headers: { "cf-connecting-ip": "203.0.113.7" } }), env);
const useUpBudget = (env, kind, n = DAILY[kind]) => env.DB.sqlite.prepare("INSERT INTO daily_budget (day, kind, n) VALUES (?, ?, ?)").run(new Date().toISOString().slice(0, 10), kind, n);

function githubMock(handler = () => ({ number: 7 })) {
  const calls = [];
  const original = globalThis.fetch;
  globalThis.fetch = async (url, init) => {
    calls.push({ url, ...init, parsed: JSON.parse(init.body) });
    const out = handler(calls.length);
    if (out instanceof Error) return new Response("no", { status: 503 });
    return new Response(JSON.stringify(out), { status: 200 });
  };
  return { calls, restore: () => { globalThis.fetch = original; } };
}
const cron = (env) => {
  const waits = [];
  worker.scheduled({}, env, { waitUntil: (p) => waits.push(p) });
  return Promise.all(waits);
};

test("a report is saved and counted, without its raw CPU values or memory bytes", async () => {
  const env = makeEnv();
  assert.equal((await send(env, report())).status, 201);
  const body = env.DB.value("SELECT body FROM reports");
  for (const raw of ["70617373776f7264", "registers\"", "xmm_low", "code_at_fault", "code_before_fault", "f30f10"]) assert.ok(!body.includes(raw), raw);
  for (const kept of ["float_to_int_overflow", "ACCESS_VIOLATION", "register_targets", "mxbikes.exe+0x1a29ae"]) assert.ok(body.includes(kept), kept);
  assert.equal(env.DB.value("SELECT reports FROM signatures"), 1);
  assert.equal(env.DB.value("SELECT COUNT(*) FROM signature_installs"), 1);
});

test("a retry of a saved report is recognised and counted once", async () => {
  const env = makeEnv();
  const r = report();
  assert.equal((await send(env, r)).status, 201);
  assert.equal((await send(env, r)).status, 200);
  assert.equal(env.DB.value("SELECT reports FROM signatures"), 1);
});

test("two copies at once: the loser is told it's saved only because the winner saved it", async () => {
  const env = makeEnv();
  const r = report();
  const original = env.DB.batch.bind(env.DB);
  let raced = false;
  env.DB.batch = async (steps) => {
    if (!raced) { raced = true; await send(env, r); }     // the other copy finishes just before this one saves
    return original(steps);
  };
  assert.equal((await send(env, r)).status, 200);
  assert.equal(env.DB.value("SELECT reports FROM signatures"), 1);
  assert.equal(env.DB.value("SELECT COUNT(*) FROM reports"), 1);
});

test("if saving fails, nothing is remembered and the retry really saves it", async () => {
  const env = makeEnv();
  const r = report();
  env.DB.failOn = (sql) => sql.includes("INTO reports");
  assert.equal((await send(env, r)).status, 500);
  assert.equal(env.DB.value("SELECT COUNT(*) FROM seen"), 0);
  assert.equal(env.DB.value("SELECT COUNT(*) FROM signatures"), 0);
  env.DB.failOn = null;
  assert.equal((await send(env, r)).status, 201);
  assert.equal(env.DB.value("SELECT COUNT(*) FROM reports"), 1);
  assert.equal(env.DB.value("SELECT reports FROM signatures"), 1);
});

test("reports never open a GitHub issue and never recount players, however many arrive", async () => {
  const env = makeEnv();
  const gh = githubMock();
  try {
    for (let i = 0; i < 40; i++) assert.equal((await send(env, report({ install_id: i.toString(16).padStart(32, "0") }))).status, 201);
    assert.equal(gh.calls.length, 0);
    assert.ok(!env.DB.log.some((sql) => /COUNT\(\*\) AS n FROM signature_installs/.test(sql)));
    assert.equal(env.DB.value("SELECT COUNT(*) FROM signature_installs"), 40);
  } finally { gh.restore(); }
});

test("a flood of one crash type stops using storage at the per-type limit, and is still counted", async () => {
  const env = makeEnv();
  const n = MAX_REPORTS_PER_SIGNATURE_PER_DAY + 10;
  const used = () => env.DB.value("SELECT n FROM daily_budget WHERE kind = 'stored_bytes'");
  let atLimit;
  for (let i = 0; i < n; i++) {
    assert.equal((await send(env, report())).status, 201);
    if (i === MAX_REPORTS_PER_SIGNATURE_PER_DAY - 1) atLimit = used();
  }
  assert.equal(env.DB.value("SELECT reports FROM signatures"), n);
  assert.equal(env.DB.value("SELECT COUNT(*) FROM reports"), MAX_REPORTS_PER_SIGNATURE_PER_DAY);
  assert.equal(used(), atLimit);
});

test("no storage left: a crash type we have no copy of waits, unsaved and uncounted, then gets in", async () => {
  const env = makeEnv();
  useUpBudget(env, "stored_bytes");
  const r = report();
  assert.equal((await send(env, r)).status, 429);
  assert.equal(env.DB.value("SELECT COUNT(*) FROM seen"), 0);
  assert.equal(env.DB.value("SELECT COUNT(*) FROM reports"), 0);
  env.DB.sqlite.exec("DELETE FROM daily_budget");     // the next day
  assert.equal((await send(env, r)).status, 201);
  assert.equal(env.DB.value("SELECT COUNT(*) FROM reports"), 1);
});

test("no storage left: a crash type we already have is still counted", async () => {
  const env = makeEnv();
  assert.equal((await send(env, report())).status, 201);
  env.DB.sqlite.exec("DELETE FROM daily_budget");
  useUpBudget(env, "stored_bytes");
  assert.equal((await send(env, report())).status, 201);
  assert.equal(env.DB.value("SELECT reports FROM signatures"), 2);
  assert.equal(env.DB.value("SELECT COUNT(*) FROM reports"), 1);
});

test("too many reports or new crash types today: told to try later, nothing saved", async () => {
  for (const kind of ["intake", "signatures"]) {
    const env = makeEnv();
    useUpBudget(env, kind);
    assert.equal((await send(env, report())).status, 429, kind);
    assert.equal(env.DB.value("SELECT COUNT(*) FROM seen"), 0, kind);
  }
});

test("a retry doesn't use today's allowance again", async () => {
  const env = makeEnv();
  const r = report();
  await send(env, r);
  await send(env, r);
  assert.equal(env.DB.value("SELECT n FROM daily_budget WHERE kind = 'intake'"), 1);
});

test("a good report is never refused for good because of a limit", async () => {
  const env = makeEnv();
  env.REPORT_PER_ADDRESS = { limit: async () => ({ success: false }) };
  assert.equal((await send(env, report())).status, 429);
  const env2 = makeEnv();
  env2.PER_INSTALL = { limit: async () => ({ success: false }) };
  assert.equal((await send(env2, report())).status, 429);
  const env3 = makeEnv();
  env3.OVERALL = { limit: async () => ({ success: false }) };
  assert.equal((await send(env3, report())).status, 429);
});

test("junk doesn't use up the shared limit, and made-up install IDs don't get past the address limit", async () => {
  const env = makeEnv();
  let overall = 0;
  env.OVERALL = { limit: async () => { overall++; return { success: true }; } };
  assert.equal((await send(env, { junk: true })).status, 400);
  assert.equal(overall, 0);
  await send(env, report({ install_id: "a".repeat(32) }));
  await send(env, report({ install_id: "b".repeat(32) }));
  assert.equal(new Set(env.seenKeys).size, 1);
});

test("the address is scrambled, changes daily, and is never stored", async () => {
  const req = (a) => new Request("https://x/", { headers: { "cf-connecting-ip": a } });
  const a = await addressKey(req("203.0.113.7"), "s", "2026-11-03");
  assert.equal(a, await addressKey(req("203.0.113.7"), "s", "2026-11-03"));
  assert.notEqual(a, await addressKey(req("203.0.113.7"), "s", "2026-11-04"));
  assert.notEqual(a, await addressKey(req("203.0.113.8"), "s", "2026-11-03"));
  assert.notEqual(a, await addressKey(req("203.0.113.7"), "other", "2026-11-03"));
  assert.ok(!a.includes("203.0.113.7"));
  assert.equal(await addressKey(req("203.0.113.7"), undefined, "2026-11-03"), null);

  const env = makeEnv();
  await send(env, report());
  await post(env, "/v1/counts", { schema: 1, counts: { "ground-nan-guard": 1 } });
  const dump = JSON.stringify(env.DB.sqlite.prepare("SELECT * FROM reports").all()) + JSON.stringify(env.DB.sqlite.prepare("SELECT * FROM seen").all())
    + JSON.stringify(env.DB.sqlite.prepare("SELECT * FROM signature_installs").all()) + JSON.stringify(env.DB.sqlite.prepare("SELECT * FROM daily_counts").all());
  assert.ok(!dump.includes("203.0.113.7"));
  assert.ok(!env.seenKeys.includes("203.0.113.7"));
});

test("counts add up, and the public total shows them", async () => {
  const env = makeEnv();
  assert.equal((await post(env, "/v1/counts", { schema: 1, counts: { "ground-nan-guard": 2, "gate-vertex-pointer": 3 } })).status, 201);
  assert.equal((await post(env, "/v1/counts", { schema: 1, counts: { "ground-nan-guard": 1 } })).status, 201);
  assert.equal(env.DB.value("SELECT SUM(count) FROM daily_counts"), 6);
  assert.equal(env.DB.value("SELECT count FROM daily_counts WHERE fix = 'ground-nan-guard'"), 3);
  const fresh = await import("../src/index.js?fresh=counts");     // a new copy of the Worker has nothing in memory yet
  const stats = await (await fresh.default.fetch(new Request("https://x/v1/stats", { headers: { "cf-connecting-ip": "203.0.113.7" } }), env)).json();
  assert.equal(stats.crashes_prevented, 6);
  assert.equal(stats.by_fix["ground-nan-guard"], 3);
});

test("counts over the daily ceiling wait, with nothing from that send saved, so a retry can't double count", async () => {
  const env = makeEnv();
  useUpBudget(env, "counts:ground-nan-guard", DAILY_COUNT_CEILING);
  const send2 = { schema: 1, counts: { "gate-vertex-pointer": 5, "ground-nan-guard": 1 } };
  assert.equal((await post(env, "/v1/counts", send2)).status, 429);
  assert.equal(env.DB.value("SELECT COUNT(*) FROM daily_counts WHERE fix = 'gate-vertex-pointer'"), 0);
  assert.equal(env.DB.value("SELECT n FROM daily_budget WHERE kind = 'counts:gate-vertex-pointer'"), 0, "gave back the room it reserved");
  env.DB.sqlite.exec("DELETE FROM daily_budget");     // the next day
  assert.equal((await post(env, "/v1/counts", send2)).status, 201);
  assert.equal(env.DB.value("SELECT SUM(count) FROM daily_counts"), 6);
});

test("a count of zero or nothing is fine and writes nothing", async () => {
  const env = makeEnv();
  assert.equal((await post(env, "/v1/counts", { schema: 1, counts: { "ground-nan-guard": 0 } })).status, 201);
  assert.equal(env.DB.value("SELECT COUNT(*) FROM daily_counts"), 0);
});

test("issues only open once approved, and a GitHub failure tries again on its own", async () => {
  const env = makeEnv();
  await send(env, report({ install_id: "a".repeat(32) }));
  await send(env, report({ install_id: "b".repeat(32) }));
  let gh = githubMock();
  await cron(env);
  assert.equal(gh.calls.length, 0, "not approved yet");
  gh.restore();

  env.DB.sqlite.exec("UPDATE signatures SET approved = 1");
  gh = githubMock(() => new Error("down"));
  await cron(env);
  assert.equal(gh.calls.length, 1);
  assert.equal(env.DB.value("SELECT issue_number FROM signatures"), null);
  gh.restore();

  gh = githubMock(() => ({ number: 7 }));
  await cron(env);
  assert.equal(gh.calls.length, 1);
  assert.equal(gh.calls[0].parsed.title, "Crash: mxbikes.exe+0x1a29ae");
  assert.ok(gh.calls[0].parsed.body.includes("Players affected: 2"));
  assert.equal(env.DB.value("SELECT issue_number FROM signatures"), 7);
  await cron(env);
  assert.equal(gh.calls.length, 1, "opens once");
  gh.restore();
});

test("approved issues stay current, and busy crash types can't keep others waiting", async () => {
  const env = makeEnv();
  const day = new Date().toISOString().slice(0, 10);
  const add = env.DB.sqlite.prepare("INSERT INTO signatures (signature, reports, first_day, last_day, approved, issue_number, issue_reports, issue_synced_day) VALUES (?, 2, ?, ?, 1, ?, 1, ?)");
  for (let i = 0; i < 12; i++) add.run(`m${i}.dll+0x1`, day, day, i + 1, i < 11 ? day : "2026-01-01");
  const gh = githubMock(() => ({}));
  await cron(env);
  assert.equal(gh.calls.length, 8);
  assert.ok(gh.calls[0].url.endsWith("/issues/12"), "the one waiting longest goes first");
  gh.restore();
});

test("a bare address or unclean exit never becomes an issue, even if approved", async () => {
  const env = makeEnv();
  await send(env, report({ signature: "unclean-exit", kind: "unclean_exit", previous_session: { server: "S", track: "" } }));
  await send(env, report({ signature: "0x12345678" }));
  env.DB.sqlite.exec("UPDATE signatures SET approved = 1");
  const gh = githubMock();
  await cron(env);
  assert.equal(gh.calls.length, 0);
  gh.restore();
});

test("old data goes at 90 days, unapproved crash types only, and a late retry is still recognised before then", async () => {
  const env = makeEnv();
  const r = report({ install_id: "a".repeat(32) });
  await send(env, r);
  const old = (n) => new Date(Date.now() - n * 86400000).toISOString().slice(0, 10);
  env.DB.sqlite.prepare("UPDATE seen SET day = ?").run(old(60));
  await cron(env);
  assert.equal((await send(env, r)).status, 200);
  assert.equal(env.DB.value("SELECT reports FROM signatures"), 1);
  env.DB.sqlite.prepare("UPDATE seen SET day = ?").run(old(91));
  await cron(env);
  assert.equal(env.DB.value("SELECT COUNT(*) FROM seen"), 0);
  assert.equal(env.DB.value("SELECT COUNT(*) FROM reports"), 1);
  env.DB.sqlite.prepare("UPDATE reports SET day = ?").run(old(91));
  env.DB.sqlite.prepare("UPDATE signature_installs SET last_day = ?").run(old(91));
  env.DB.sqlite.prepare("UPDATE signatures SET last_day = ?").run(old(91));
  env.DB.sqlite.prepare("INSERT INTO signatures (signature, reports, first_day, last_day, approved) VALUES ('kept.dll+0x1', 1, ?, ?, 1)").run(old(91), old(91));
  env.GITHUB_TOKEN = "";
  await cron(env);
  assert.equal(env.DB.value("SELECT COUNT(*) FROM reports"), 0);
  assert.equal(env.DB.value("SELECT COUNT(*) FROM signature_installs"), 0);
  assert.equal(env.DB.value("SELECT COUNT(*) FROM signatures"), 1);
  assert.equal(env.DB.value("SELECT signature FROM signatures"), "kept.dll+0x1");
});

// Fills one worst-case day through the real Worker and measures the real database, for two shapes of flood.
for (const [name, bodySize] of [["many small reports", 0], ["real-sized reports", 17000]]) {
  test(`90 days of the worst case stays under the free plan's 500 MB: ${name}`, async () => {
    const env = makeEnv();
    const long = "x".repeat(160);
    const padding = "p".repeat(bodySize);
    for (let i = 0; i < DAILY.intake; i++) {
      const sig = (i % DAILY.signatures).toString(36).padStart(70, "m");
      const res = await send(env, {
        schema: 1, kind: "crash", id: `${i}`.padStart(96, "r"), install_id: i.toString(16).padStart(32, "0"), signature: `${sig}.dll+0x${"f".repeat(16)}`,
        plugin: { version: "9999.9999.9999" }, game: { build: "0xffffffff" }, context: { server: long, track: long }, summary: padding,
      });
      assert.ok(res.status === 201 || res.status === 429, String(res.status));
    }
    const page = (p) => env.DB.value(`PRAGMA ${p}`);
    const perDay = page("page_count") * page("page_size");
    const in90Days = perDay * 90;
    console.log(`${name}: ${Math.round(in90Days / 1024 / 1024)} MB in 90 days`);
    assert.ok(in90Days < 450 * 1024 * 1024, `${Math.round(in90Days / 1024 / 1024)} MB`);
  });
}

test("sends at the same moment can't pass the counts ceiling together", async () => {
  const env = makeEnv();
  useUpBudget(env, "counts:ground-nan-guard", DAILY_COUNT_CEILING - 100);
  env.DB.sqlite.prepare("INSERT INTO daily_counts (day, fix, count) VALUES (?, 'ground-nan-guard', ?)").run(new Date().toISOString().slice(0, 10), DAILY_COUNT_CEILING - 100);
  const one = { schema: 1, counts: { "ground-nan-guard": 100 } };
  const res = await Promise.all([post(env, "/v1/counts", one), post(env, "/v1/counts", one)]);
  assert.deepEqual(res.map((r) => r.status).sort(), [201, 429]);
  assert.equal(env.DB.value("SELECT count FROM daily_counts"), DAILY_COUNT_CEILING);
});

test("if saving counts fails, nothing is kept and the retry counts once", async () => {
  const env = makeEnv();
  const one = { schema: 1, counts: { "ground-nan-guard": 5, "gate-vertex-pointer": 2 } };
  env.DB.failOn = (sql) => sql.includes("INTO daily_counts");
  assert.equal((await post(env, "/v1/counts", one)).status, 500);
  assert.equal(env.DB.value("SELECT COUNT(*) FROM daily_counts"), 0);
  env.DB.failOn = null;
  assert.equal((await post(env, "/v1/counts", one)).status, 201);
  assert.equal(env.DB.value("SELECT SUM(count) FROM daily_counts"), 7);
  assert.equal(env.DB.value("SELECT n FROM daily_budget WHERE kind = 'counts:ground-nan-guard'"), 5);
});

test("a failed save gives back the storage and new-type allowance it took", async () => {
  const env = makeEnv();
  env.DB.failOn = (sql) => sql.includes("INTO seen");
  assert.equal((await send(env, report())).status, 500);
  assert.equal(env.DB.value("SELECT n FROM daily_budget WHERE kind = 'stored_bytes'"), 0);
  assert.equal(env.DB.value("SELECT n FROM daily_budget WHERE kind = 'signatures'"), 0);
});

test("copies of one report at once use up storage only once", async () => {
  const env = makeEnv();
  const r = report();
  const res = await Promise.all(Array.from({ length: 8 }, () => send(env, r)));
  assert.equal(res.filter((x) => x.status === 201).length, 1);
  assert.equal(res.filter((x) => x.status === 200).length, 7);
  assert.equal(env.DB.value("SELECT COUNT(*) FROM reports"), 1);
  const one = makeEnv();
  await send(one, r);
  assert.equal(env.DB.value("SELECT n FROM daily_budget WHERE kind = 'stored_bytes'"), one.DB.value("SELECT n FROM daily_budget WHERE kind = 'stored_bytes'"));
  assert.equal(env.DB.value("SELECT n FROM daily_budget WHERE kind = 'signatures'"), 1);
});

test("the per-type limit holds for reports at the same moment", async () => {
  const env = makeEnv();
  for (let i = 0; i < MAX_REPORTS_PER_SIGNATURE_PER_DAY - 1; i++) await send(env, report());
  // Both have checked and neither has saved: hold the first save until the second is about to save too.
  const original = env.DB.batch.bind(env.DB);
  let arrived = 0, release;
  const both = new Promise((r) => { release = r; });
  env.DB.batch = async (steps) => {
    if (++arrived === 2) release();
    await both;
    return original(steps);
  };
  const res = await Promise.all([send(env, report()), send(env, report())]);
  assert.deepEqual(res.map((r) => r.status), [201, 201]);
  assert.equal(env.DB.value("SELECT COUNT(*) FROM reports"), MAX_REPORTS_PER_SIGNATURE_PER_DAY);
  assert.equal(env.DB.value("SELECT reports FROM signatures"), MAX_REPORTS_PER_SIGNATURE_PER_DAY + 1);
});

test("raw values are removed wherever a sender puts them", async () => {
  const env = makeEnv();
  await send(env, report({
    known: { registers: { rax: "SECRET" } }, batches: [{ code_at_fault: "SECRET" }], register_targets: [{ xmm_low: ["SECRET"], reg: "rax" }], exception: [{ code_before_fault: "SECRET", name: "x" }],
  }));
  const body = env.DB.value("SELECT body FROM reports");
  assert.ok(!body.includes("SECRET"), body);
  assert.ok(body.includes('"reg":"rax"'));
});

test("two daily jobs at once open one issue", async () => {
  const env = makeEnv();
  await send(env, report());
  env.DB.sqlite.exec("UPDATE signatures SET approved = 1");
  const gh = githubMock(() => ({ number: 9 }));
  await Promise.all([cron(env), cron(env)]);
  assert.equal(gh.calls.length, 1);
  gh.restore();
});

test("an issue from before approval existed isn't updated until approved", async () => {
  const env = makeEnv();
  const day = new Date().toISOString().slice(0, 10);
  env.DB.sqlite.prepare("INSERT INTO signatures (signature, reports, first_day, last_day, issue_number, issue_reports) VALUES ('old.dll+0x1', 5, ?, ?, 3, 1)").run(day, day);
  const gh = githubMock(() => ({}));
  await cron(env);
  assert.equal(gh.calls.length, 0);
  gh.restore();
});

test("the daily job stays inside the free plan's 50 calls, even when every issue fails to save", async () => {
  const env = makeEnv();
  const day = new Date().toISOString().slice(0, 10);
  const add = env.DB.sqlite.prepare("INSERT INTO signatures (signature, reports, first_day, last_day, approved, issue_number, issue_reports) VALUES (?, 2, ?, ?, 1, ?, ?)");
  for (let i = 0; i < 10; i++) add.run(`new${i}.dll+0x1`, day, day, null, 0);
  for (let i = 0; i < 12; i++) add.run(`old${i}.dll+0x1`, day, day, i + 1, 1);
  const before = env.DB.log.length;
  const gh = githubMock(() => new Error("down"));
  await cron(env);
  const database = env.DB.log.length - before;
  assert.ok(database + gh.calls.length <= 50, `${database} database statements + ${gh.calls.length} GitHub calls`);
  gh.restore();
});

test("public totals are served from memory, with a limit only when the database is needed", async () => {
  const env = makeEnv();
  const stats = await (await get(env, "/v1/stats")).json();
  assert.equal(typeof stats.crashes_prevented, "number");
  assert.deepEqual(Object.keys(stats.by_fix).sort(), ["gate-vertex-pointer", "ground-nan-guard", "trainer-cleanup-reset", "trainer-series-lookup"]);
  const before = env.DB.log.length;
  env.READ_PER_ADDRESS = { limit: async () => ({ success: false }) };
  assert.equal((await get(env, "/v1/badge")).status, 200);
  assert.equal(env.DB.log.length, before);
  const b = await (await get(env, "/v1/badge")).json();
  assert.equal(b.label, "crashes prevented");
});

test("unknown paths and methods are a plain not found", async () => {
  const env = makeEnv();
  assert.equal((await get(env, "/v1/report")).status, 404);
  assert.equal((await post(env, "/v1/other", {})).status, 404);
});
