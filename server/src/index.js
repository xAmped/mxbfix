import { MAX_BODY, MAX_REPORTS_PER_SIGNATURE_PER_DAY, DAILY, DAILY_COUNT_CEILING, ROW_OVERHEAD_BYTES, FIXES, issueWorthy, cleanReport, cleanCounts, badge, issueTitle, issueBody } from "./validate.js";

const RETENTION_DAYS = 90;
const BUDGET_RETENTION_DAYS = 30;
const CLAIM_TIMEOUT_S = 3600;
const AGGREGATE_TTL_MS = 5 * 60 * 1000;
// One daily job stays inside Cloudflare's per-run limits on the free plan. What doesn't fit goes first on later days.
const ISSUES_OPENED_PER_RUN = 4;
const ISSUES_UPDATED_PER_RUN = 8;

// What each answer means to the plugin: 2xx files the report as sent, 400 files it as refused for good, anything else keeps it and
// tries again at the next game start. So anything that might succeed later must not be a 2xx or a 400.
const TRY_LATER = 429;

const today = () => new Date().toISOString().slice(0, 10);
const daysAgo = (n) => new Date(Date.now() - n * 86400000).toISOString().slice(0, 10);
const json = (status, value, headers = {}) =>
  new Response(JSON.stringify(value), { status, headers: { "content-type": "application/json", ...headers } });

// Reads at most MAX_BODY bytes, however big the sender claims the body is.
async function readBody(request) {
  if (!request.body) return null;
  const reader = request.body.getReader();
  const chunks = [];
  let size = 0;
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    size += value.byteLength;
    if (size > MAX_BODY) {
      await reader.cancel();
      return null;
    }
    chunks.push(value);
  }
  const all = new Uint8Array(size);
  let at = 0;
  for (const c of chunks) {
    all.set(c, at);
    at += c.byteLength;
  }
  try {
    return JSON.parse(new TextDecoder().decode(all));
  } catch {
    return null;
  }
}

// Takes from today's allowance in a single statement, so simultaneous requests can't overshoot. False when it wouldn't fit.
async function takeBudget(env, kind, amount = 1, limit = DAILY[kind]) {
  if (amount > limit) return false;
  const r = await env.DB.prepare(
    "INSERT INTO daily_budget (day, kind, n) VALUES (?1, ?2, ?3) ON CONFLICT (day, kind) DO UPDATE SET n = n + ?3 WHERE n + ?3 <= ?4",
  ).bind(today(), kind, amount, limit).run();
  return r.meta.changes > 0;
}

// Gives back what a save didn't use. If this itself fails, the only cost is a smaller allowance until tomorrow.
async function giveBack(env, kind, amount) {
  if (!amount) return;
  await env.DB.prepare("UPDATE daily_budget SET n = MAX(0, n - ?) WHERE day = ? AND kind = ?").bind(amount, today(), kind).run().catch(() => {});
}

// The sender's address, scrambled with a secret and the date. The limiter only needs a value that's the same for one address today:
// the address itself is never stored or logged, and today's value can't be linked to tomorrow's. Null if it can't be worked out.
export async function addressKey(request, secret, day) {
  const address = request.headers.get("cf-connecting-ip");
  if (!address || !secret) return null;
  const encode = (s) => new TextEncoder().encode(s);
  const key = await crypto.subtle.importKey("raw", encode(secret), { name: "HMAC", hash: "SHA-256" }, false, ["sign"]);
  const mac = await crypto.subtle.sign("HMAC", key, encode(`${day}|${address}`));
  return btoa(String.fromCharCode(...new Uint8Array(mac)));
}

// Address limits come first, since an address is the one thing a sender can't simply make up. They're generous, because players can
// share a connection. Being limited costs an honest player nothing: the report waits for the next game start.
async function addressAllowed(request, env, limiter) {
  const key = await addressKey(request, env.RATE_KEY, today());
  if (!key) return true;
  return (await limiter.limit({ key })).success;
}

async function github(env, method, path, body) {
  const res = await fetch(`https://api.github.com/repos/${env.GITHUB_REPO}${path}`, {
    method,
    headers: {
      authorization: `Bearer ${env.GITHUB_TOKEN}`,
      accept: "application/vnd.github+json",
      "user-agent": "mxbfix-reports",
      "x-github-api-version": "2022-11-28",
    },
    body: JSON.stringify(body),
  });
  if (!res.ok) throw new Error(`github ${method} ${path}: ${res.status}`);
  return res.json();
}

async function installsFor(env, signature) {
  const row = await env.DB.prepare("SELECT COUNT(*) AS n FROM signature_installs WHERE signature = ?").bind(signature).first();
  return row.n;
}

async function handleReport(request, env) {
  if (!(await addressAllowed(request, env, env.REPORT_PER_ADDRESS))) return json(TRY_LATER, { error: "slow down" });
  const input = await readBody(request);
  if (!input) return json(400, { error: "body" });
  const { report, error } = cleanReport(input);
  if (error) return json(400, { error });
  if (!(await env.PER_INSTALL.limit({ key: report.install_id })).success) return json(TRY_LATER, { error: "slow down" });
  if (!(await env.OVERALL.limit({ key: "/v1/report" })).success) return json(TRY_LATER, { error: "slow down" });

  const seen = env.DB.prepare("SELECT 1 FROM seen WHERE id = ? AND install_id = ?").bind(report.id, report.install_id);
  if (await seen.first()) return json(200, { ok: true, duplicate: true });
  if (!(await takeBudget(env, "intake"))) return json(TRY_LATER, { error: "too many reports today" });

  const day = today();
  const known = await env.DB.prepare("SELECT 1 FROM signatures WHERE signature = ?").bind(report.signature).first();
  const newType = known ? 0 : 1;
  if (newType && !(await takeBudget(env, "signatures"))) return json(TRY_LATER, { error: "too many new crash types today" });

  // Keep a full report if there's room. Only one that's kept uses up today's storage, so a flood of one crash type can't spend it.
  // With no room left, a crash type we have no copy of waits for tomorrow; one we already have just gets counted.
  const body = JSON.stringify(report.body);
  const kept = await env.DB.prepare("SELECT COUNT(*) AS n FROM reports WHERE signature = ? AND day = ?").bind(report.signature, day).first();
  const encoder = new TextEncoder();
  const cost = encoder.encode(body + (report.server ?? "") + (report.track ?? "")).length + ROW_OVERHEAD_BYTES;
  let store = false;
  if (kept.n < MAX_REPORTS_PER_SIGNATURE_PER_DAY) {
    store = await takeBudget(env, "stored_bytes", cost);
    if (!store) {
      const sample = await env.DB.prepare("SELECT 1 FROM reports WHERE signature = ? LIMIT 1").bind(report.signature).first();
      if (!sample) {
        await giveBack(env, "signatures", newType);
        return json(TRY_LATER, { error: "no room today" });
      }
    }
  }

  // All of it or none of it: a report is only ever remembered as received once it's really saved.
  const steps = [
    env.DB.prepare("INSERT INTO seen (id, install_id, day) VALUES (?, ?, ?)").bind(report.id, report.install_id, day),
    env.DB.prepare(
      `INSERT INTO signatures (signature, reports, first_day, last_day, last_version, last_build) VALUES (?1, 1, ?2, ?2, ?3, ?4)
       ON CONFLICT (signature) DO UPDATE SET reports = reports + 1, last_day = ?2, last_version = ?3, last_build = ?4`,
    ).bind(report.signature, day, report.version, report.build),
    env.DB.prepare(
      `INSERT INTO signature_installs (signature, install_id, last_day) VALUES (?1, ?2, ?3)
       ON CONFLICT (signature, install_id) DO UPDATE SET last_day = ?3`,
    ).bind(report.signature, report.install_id, day),
  ];
  if (store) {
    steps.push(env.DB.prepare(
      `INSERT OR IGNORE INTO reports (id, install_id, signature, kind, plugin_version, game_build, server, track, day, body)
       SELECT ?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10
       WHERE (SELECT COUNT(*) FROM reports WHERE signature = ?3 AND day = ?9) < ?11`,
    ).bind(report.id, report.install_id, report.signature, report.kind, report.version, report.build, report.server, report.track, day, body, MAX_REPORTS_PER_SIGNATURE_PER_DAY));
  }
  try {
    const results = await env.DB.batch(steps);
    if (store && !results[3].meta.changes) await giveBack(env, "stored_bytes", cost);     // the per-type limit was reached at the last moment
  } catch (e) {
    await giveBack(env, "signatures", newType);
    if (store) await giveBack(env, "stored_bytes", cost);
    // Another copy of this report got in first, and it's saved.
    if (/UNIQUE constraint failed/.test(String(e))) return json(200, { ok: true, duplicate: true });
    throw e;
  }
  return json(201, { ok: true });
}

async function handleCounts(request, env) {
  if (!(await addressAllowed(request, env, env.COUNTS_PER_ADDRESS))) return json(TRY_LATER, { error: "slow down" });
  const input = await readBody(request);
  if (!input) return json(400, { error: "body" });
  const { counts, error } = cleanCounts(input);
  if (error) return json(400, { error });
  if (!(await env.OVERALL.limit({ key: "/v1/counts" })).success) return json(TRY_LATER, { error: "slow down" });
  const entries = Object.entries(counts);
  if (!entries.length) return json(201, { ok: true });

  // All or nothing: on any answer but 2xx the plugin sends the whole batch again, so a partial save would count some crashes twice. Each
  // fix's room is reserved first, in one statement each, so sends at the same moment can't pass the ceiling together.
  const reserved = [];
  const giveAllBack = () => Promise.all(reserved.map(([fix, n]) => giveBack(env, `counts:${fix}`, n)));
  for (const [fix, n] of entries) {
    if (!(await takeBudget(env, `counts:${fix}`, n, DAILY_COUNT_CEILING))) {
      await giveAllBack();
      return json(TRY_LATER, { error: "too many today" });
    }
    reserved.push([fix, n]);
  }
  const day = today();
  try {
    await env.DB.batch(entries.map(([fix, n]) =>
      env.DB.prepare("INSERT INTO daily_counts (day, fix, count) VALUES (?1, ?2, ?3) ON CONFLICT (day, fix) DO UPDATE SET count = count + ?3").bind(day, fix, n)));
  } catch (e) {
    await giveAllBack();
    throw e;
  }
  return json(201, { ok: true });
}

// The public total, kept for a few minutes in this copy of the Worker so public pages don't reach the database on every visit. It holds
// nothing about any request, only the same numbers anyone can read.
let aggregate = null;

async function publicTotals(request, env) {
  if (aggregate && Date.now() - aggregate.at < AGGREGATE_TTL_MS) return aggregate;
  if (!(await addressAllowed(request, env, env.READ_PER_ADDRESS))) return aggregate;
  const { results } = await env.DB.prepare("SELECT fix, SUM(count) AS total FROM daily_counts GROUP BY fix").all();
  const prevented = Object.fromEntries(FIXES.map((f) => [f, 0]));
  let total = 0;
  for (const r of results) {
    prevented[r.fix] = r.total;
    total += r.total;
  }
  aggregate = { at: Date.now(), total, prevented };
  return aggregate;
}

async function handleStats(request, env) {
  const t = await publicTotals(request, env);
  if (!t) return json(TRY_LATER, { error: "slow down" });
  return json(200, { crashes_prevented: t.total, by_fix: t.prevented }, { "cache-control": "public, max-age=3600", "access-control-allow-origin": "*" });
}

async function handleBadge(request, env) {
  const t = await publicTotals(request, env);
  if (!t) return json(TRY_LATER, { error: "slow down" });
  return json(200, badge(t.total), { "cache-control": "public, max-age=3600" });
}

// Public issues only open for crash types the maintainer has approved (tools/crash_list.sh). A failure just tries again tomorrow.
async function openApprovedIssues(env) {
  const now = Math.floor(Date.now() / 1000);
  const { results } = await env.DB.prepare(
    `SELECT * FROM signatures WHERE approved = 1 AND issue_number IS NULL AND (issue_claimed_at IS NULL OR issue_claimed_at < ?)
     ORDER BY first_day LIMIT ?`,
  ).bind(now - CLAIM_TIMEOUT_S, ISSUES_OPENED_PER_RUN).all();
  for (const row of results) {
    if (!issueWorthy(row.signature)) continue;
    const claim = await env.DB.prepare(
      "UPDATE signatures SET issue_claimed_at = ?1 WHERE signature = ?2 AND issue_number IS NULL AND (issue_claimed_at IS NULL OR issue_claimed_at < ?3)",
    ).bind(now, row.signature, now - CLAIM_TIMEOUT_S).run();
    if (!claim.meta.changes) continue;
    let issue;
    try {
      issue = await github(env, "POST", "/issues", { title: issueTitle(row.signature), body: issueBody(row, await installsFor(env, row.signature)) });
    } catch (e) {
      await env.DB.prepare("UPDATE signatures SET issue_claimed_at = NULL WHERE signature = ?").bind(row.signature).run().catch(() => {});
      console.error(JSON.stringify({ event: "issue_failed", signature: row.signature, error: String(e) }));
      continue;
    }
    // If this save fails, the claim runs out and a later run opens a second issue for the same crash: rare, visible and harmless.
    await env.DB.prepare("UPDATE signatures SET issue_number = ?, issue_reports = ?, issue_synced_day = ? WHERE signature = ?")
      .bind(issue.number, row.reports, today(), row.signature).run();
    console.log(JSON.stringify({ event: "issue_opened", signature: row.signature, issue: issue.number }));
  }
}

// Least recently updated first, so busy crash types can't keep the others waiting.
async function updateIssues(env) {
  const { results } = await env.DB.prepare(
    "SELECT * FROM signatures WHERE approved = 1 AND issue_number IS NOT NULL AND reports > issue_reports ORDER BY COALESCE(issue_synced_day, '') LIMIT ?",
  ).bind(ISSUES_UPDATED_PER_RUN).all();
  for (const row of results) {
    try {
      await github(env, "PATCH", `/issues/${row.issue_number}`, { body: issueBody(row, await installsFor(env, row.signature)) });
      await env.DB.prepare("UPDATE signatures SET issue_reports = ?, issue_synced_day = ? WHERE signature = ?").bind(row.reports, today(), row.signature).run();
    } catch (e) {
      console.error(JSON.stringify({ event: "issue_update_failed", signature: row.signature, error: String(e) }));
    }
  }
}

async function daily(env) {
  const cutoff = daysAgo(RETENTION_DAYS);
  await env.DB.batch([
    env.DB.prepare("DELETE FROM reports WHERE day < ?").bind(cutoff),
    env.DB.prepare("DELETE FROM signature_installs WHERE last_day < ?").bind(cutoff),
    env.DB.prepare("DELETE FROM seen WHERE day < ?").bind(cutoff),
    env.DB.prepare("DELETE FROM daily_budget WHERE day < ?").bind(daysAgo(BUDGET_RETENTION_DAYS)),
    env.DB.prepare("DELETE FROM signatures WHERE last_day < ? AND approved = 0 AND issue_number IS NULL").bind(cutoff),
  ]);
  if (!env.GITHUB_TOKEN) return;
  await openApprovedIssues(env);
  await updateIssues(env);
}

export default {
  async fetch(request, env) {
    const { pathname } = new URL(request.url);
    try {
      if (request.method === "GET" && pathname === "/v1/stats") return await handleStats(request, env);
      if (request.method === "GET" && pathname === "/v1/badge") return await handleBadge(request, env);
      if (request.method === "POST" && pathname === "/v1/report") return await handleReport(request, env);
      if (request.method === "POST" && pathname === "/v1/counts") return await handleCounts(request, env);
      return json(404, { error: "not found" });
    } catch (e) {
      console.error(JSON.stringify({ event: "failed", path: pathname, error: String(e) }));
      return json(500, { error: "try later" });
    }
  },
  async scheduled(event, env, ctx) {
    ctx.waitUntil(daily(env).catch((e) => console.error(JSON.stringify({ event: "daily_failed", error: String(e) }))));
  },
};
