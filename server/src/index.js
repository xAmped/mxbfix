import { MAX_BODY, MAX_REPORTS_PER_SIGNATURE_PER_DAY, ISSUE_MIN_INSTALLS, FIXES, DAILY, issueWorthy, cleanReport, cleanCounts, badge, issueTitle, issueBody } from "./validate.js";

const RETENTION_DAYS = 90;
const CLAIM_TIMEOUT_S = 3600;

const today = () => new Date().toISOString().slice(0, 10);
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

// Takes one from today's allowance in a single statement, so simultaneous requests can't overshoot. False when it's used up.
async function takeBudget(env, kind) {
  const r = await env.DB.prepare(
    "INSERT INTO daily_budget (day, kind, n) VALUES (?1, ?2, 1) ON CONFLICT (day, kind) DO UPDATE SET n = n + 1 WHERE n < ?3",
  ).bind(today(), kind, DAILY[kind]).run();
  return r.meta.changes > 0;
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

// One issue per crash type, once more than one install has hit it. The claim stops two reports opening two issues.
async function maybeOpenIssue(env, signature) {
  if (!env.GITHUB_TOKEN || !issueWorthy(signature)) return;
  const installs = await installsFor(env, signature);
  if (installs < ISSUE_MIN_INSTALLS) return;
  const now = Math.floor(Date.now() / 1000);
  const claim = await env.DB.prepare(
    "UPDATE signatures SET issue_claimed_at = ? WHERE signature = ? AND issue_number IS NULL AND (issue_claimed_at IS NULL OR issue_claimed_at < ?)",
  ).bind(now, signature, now - CLAIM_TIMEOUT_S).run();
  if (!claim.meta.changes) return;
  if (!(await takeBudget(env, "issues"))) {
    await env.DB.prepare("UPDATE signatures SET issue_claimed_at = NULL WHERE signature = ?").bind(signature).run();
    console.log(JSON.stringify({ event: "issue_budget_used", signature }));
    return;
  }
  const row = await env.DB.prepare("SELECT * FROM signatures WHERE signature = ?").bind(signature).first();
  const issue = await github(env, "POST", "/issues", { title: issueTitle(signature), body: issueBody(row, installs) });
  await env.DB.prepare("UPDATE signatures SET issue_number = ?, issue_reports = ? WHERE signature = ?")
    .bind(issue.number, row.reports, signature).run();
  console.log(JSON.stringify({ event: "issue_opened", signature, issue: issue.number }));
}

async function handleReport(request, env, ctx) {
  const input = await readBody(request);
  if (!input) return json(400, { error: "body" });
  const { report, error } = cleanReport(input);
  if (error) return json(400, { error });
  const perInstall = await env.PER_INSTALL.limit({ key: report.install_id });
  if (!perInstall.success) return json(429, { error: "slow down" });

  const day = today();
  // Claim the report first, so two copies at once count once. If storing fails below, the claim is undone and a retry works.
  const claim = await env.DB.prepare("INSERT OR IGNORE INTO seen (id, install_id, day) VALUES (?, ?, ?)").bind(report.id, report.install_id, day).run();
  if (!claim.meta.changes) return json(200, { ok: true, duplicate: true });
  try {
    const known = await env.DB.prepare("SELECT 1 FROM signatures WHERE signature = ?").bind(report.signature).first();
    if (!known && !(await takeBudget(env, "signatures"))) {
      await env.DB.prepare("DELETE FROM seen WHERE id = ? AND install_id = ?").bind(report.id, report.install_id).run();
      return json(429, { error: "too many new crash types today" });
    }
    const steps = [
      env.DB.prepare(
        `INSERT INTO signatures (signature, reports, first_day, last_day, last_version, last_build) VALUES (?1, 1, ?2, ?2, ?3, ?4)
         ON CONFLICT (signature) DO UPDATE SET reports = reports + 1, last_day = ?2, last_version = ?3, last_build = ?4`,
      ).bind(report.signature, day, report.version, report.build),
      env.DB.prepare(
        `INSERT INTO signature_installs (signature, install_id, last_day) VALUES (?1, ?2, ?3)
         ON CONFLICT (signature, install_id) DO UPDATE SET last_day = ?3`,
      ).bind(report.signature, report.install_id, day),
    ];
    // A flood still counts, but only so many are kept in full. The limit is part of the insert, so it holds under load.
    if (await takeBudget(env, "stored")) {
      steps.push(env.DB.prepare(
        `INSERT OR IGNORE INTO reports (id, install_id, signature, kind, plugin_version, game_build, server, track, day, body)
         SELECT ?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10
         WHERE (SELECT COUNT(*) FROM reports WHERE signature = ?3 AND day = ?9) < ?11`,
      ).bind(report.id, report.install_id, report.signature, report.kind, report.version, report.build, report.server, report.track, day, JSON.stringify(report.body), MAX_REPORTS_PER_SIGNATURE_PER_DAY));
    }
    await env.DB.batch(steps);
  } catch (e) {
    await env.DB.prepare("DELETE FROM seen WHERE id = ? AND install_id = ?").bind(report.id, report.install_id).run();
    throw e;
  }
  ctx.waitUntil(maybeOpenIssue(env, report.signature).catch((e) => console.error(JSON.stringify({ event: "issue_failed", signature: report.signature, error: String(e) }))));
  return json(201, { ok: true });
}

async function handleCounts(request, env) {
  const input = await readBody(request);
  if (!input) return json(400, { error: "body" });
  const { counts, error } = cleanCounts(input);
  if (error) return json(400, { error });
  const day = today();
  const steps = Object.entries(counts).map(([fix, n]) =>
    env.DB.prepare("INSERT INTO daily_counts (day, fix, count) VALUES (?1, ?2, ?3) ON CONFLICT (day, fix) DO UPDATE SET count = count + ?3").bind(day, fix, n));
  if (steps.length) await env.DB.batch(steps);
  return json(201, { ok: true });
}

async function handleStats(env) {
  const { results } = await env.DB.prepare("SELECT fix, SUM(count) AS total FROM daily_counts GROUP BY fix").all();
  const prevented = Object.fromEntries(FIXES.map((f) => [f, 0]));
  let total = 0;
  for (const r of results) {
    prevented[r.fix] = r.total;
    total += r.total;
  }
  return json(200, { crashes_prevented: total, by_fix: prevented }, { "cache-control": "public, max-age=3600", "access-control-allow-origin": "*" });
}

async function handleBadge(env) {
  const row = await env.DB.prepare("SELECT COALESCE(SUM(count), 0) AS total FROM daily_counts").first();
  return json(200, badge(row.total), { "cache-control": "public, max-age=3600" });
}

async function daily(env) {
  const cutoff = new Date(Date.now() - RETENTION_DAYS * 86400000).toISOString().slice(0, 10);
  await env.DB.batch([
    env.DB.prepare("DELETE FROM reports WHERE day < ?").bind(cutoff),
    env.DB.prepare("DELETE FROM signature_installs WHERE last_day < ?").bind(cutoff),
    env.DB.prepare("DELETE FROM seen WHERE day < ?").bind(cutoff),
    env.DB.prepare("DELETE FROM daily_budget WHERE day < ?").bind(cutoff),
  ]);
  if (!env.GITHUB_TOKEN) return;
  const { results } = await env.DB.prepare("SELECT * FROM signatures WHERE issue_number IS NOT NULL AND reports > issue_reports LIMIT 20").all();
  for (const row of results) {
    await github(env, "PATCH", `/issues/${row.issue_number}`, { body: issueBody(row, await installsFor(env, row.signature)) });
    await env.DB.prepare("UPDATE signatures SET issue_reports = ? WHERE signature = ?").bind(row.reports, row.signature).run();
  }
}

export default {
  async fetch(request, env, ctx) {
    const { pathname } = new URL(request.url);
    if (request.method === "GET" && pathname === "/v1/stats") return handleStats(env);
    if (request.method === "GET" && pathname === "/v1/badge") return handleBadge(env);
    if (request.method !== "POST" || (pathname !== "/v1/report" && pathname !== "/v1/counts")) return json(404, { error: "not found" });
    const overall = await env.OVERALL.limit({ key: pathname });
    if (!overall.success) return json(429, { error: "slow down" });
    try {
      return pathname === "/v1/report" ? await handleReport(request, env, ctx) : await handleCounts(request, env);
    } catch (e) {
      console.error(JSON.stringify({ event: "failed", path: pathname, error: String(e) }));
      return json(500, { error: "try later" });
    }
  },
  async scheduled(event, env, ctx) {
    ctx.waitUntil(daily(env));
  },
};
