// What the relay accepts. The plugin strips these fields first; this is the second lock.
export const MAX_BODY = 128 * 1024;
export const MAX_REPORTS_PER_SIGNATURE_PER_DAY = 50;
// Daily limits across all senders, sized so a flood can't outgrow the database: 90 days of the worst case stays under 500 MB (a test
// measures this on the real schema). Reaching one means look into it and adjust. Nothing is lost when one is reached: the plugin keeps
// the report and tries again.
export const DAILY = { intake: 3000, stored_bytes: 2 * 1024 * 1024, signatures: 500 };
// What a kept report costs beyond its text: IDs, versions, the row and its three indexes.
export const ROW_OVERHEAD_BYTES = 600;
// Per fix, per day. Real counts over this wait for the next day on the player's PC.
export const DAILY_COUNT_CEILING = 1000;
export const FIXES = ["gate-vertex-pointer", "trainer-series-lookup", "trainer-cleanup-reset", "ground-nan-guard"];
const MAX_COUNT = 100;

// Raw CPU values and memory bytes aren't on this list: they could hold a fragment of whatever the game was handling. The plugin's
// own diagnosis, made from them on the player's PC, is kept.
const KEEP = new Set(["schema", "kind", "id", "time_utc", "plugin", "game", "signature", "known", "exception", "diagnosis",
  "register_targets", "stack", "stack_scan", "context", "batches", "recent_events", "env", "fixes",
  "modules", "truncated", "partial", "summary", "previous_session", "windows_event", "install_id", "mxbmrp3", "freeze"]);
const RAW_FIELDS = new Set(["registers", "xmm_low", "code_at_fault", "code_before_fault"]);

// module+offset, or a bare address when the crash was outside any module. Nothing here can carry Markdown or a mention.
const SIGNATURE = /^(unclean-exit|0x[0-9a-f]{1,16}|[a-z0-9_.\-() ]{1,80}\+0x[0-9a-f]{1,16})$/;
const ISSUE_WORTHY = /^[a-z0-9_.\-() ]{1,80}\.(exe|dll|dlo)\+0x[0-9a-f]{1,16}$/;
const LOOKS_LIKE_PATH = /[A-Za-z]:[\\/]|\\\\|\/Users\//;
const INSTALL_ID = /^[0-9a-f]{32}$/;
const REPORT_ID = /^[A-Za-z0-9_\-]{1,96}$/;
const VERSION = /^\d{1,4}\.\d{1,4}\.\d{1,4}$/;
const BUILD = /^0x[0-9a-f]{1,8}$/;

// Second lock on private data at any depth: no "path" members, no raw CPU values or memory bytes, no strings that look like file paths.
function scrub(v) {
  if (typeof v === "string") return LOOKS_LIKE_PATH.test(v) ? "(removed)" : v;
  if (Array.isArray(v)) return v.map(scrub);
  if (v && typeof v === "object") {
    const out = {};
    for (const [k, x] of Object.entries(v)) if (k !== "path" && !RAW_FIELDS.has(k)) out[k] = scrub(x);
    return out;
  }
  return v;
}

// The plugin's own fields for these two members, and nothing else: they're the newest, so check them exactly.
const LOCATION = /^[a-z0-9_.\-() ]{1,80}\+0x[0-9a-f]{1,16}$/i;
const isBool = (v) => typeof v === "boolean";
const isInt = (v) => Number.isInteger(v) && Math.abs(v) <= 1e9;
const MEMBER_FIELDS = {
  mxbmrp3: {
    crash_log: isBool, dump: isBool, same_fault: isBool, seconds_from_ours: isInt,
    fault: (v) => typeof v === "string" && LOCATION.test(v),
    code: (v) => typeof v === "string" && /^0x[0-9a-f]{1,8}$/i.test(v),
    stack: (v) => Array.isArray(v) && v.length <= 16 && v.every((x) => typeof x === "string" && LOCATION.test(x)),
  },
  freeze: {
    froze: isBool, record: isBool, hung_at_end: isBool, windows_hang_event: isBool,
    game_quiet_s: isInt, hung_s: isInt, windows_hung_s: isInt,
    hang_type: (v) => typeof v === "string" && /^[A-Za-z0-9 _]{1,32}$/.test(v),
    time_utc: (v) => typeof v === "string" && /^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ$/.test(v),
  },
};

function pickFields(v, rules) {
  if (!v || typeof v !== "object" || Array.isArray(v)) return undefined;
  const out = {};
  for (const [k, ok] of Object.entries(rules)) if (k in v && ok(v[k])) out[k] = v[k];
  return out;
}

export function issueWorthy(signature) {
  return ISSUE_WORTHY.test(signature);
}

function shortString(v, max = 160) {
  return typeof v === "string" ? v.slice(0, max) : null;
}

export function cleanReport(input) {
  if (!input || typeof input !== "object" || Array.isArray(input)) return { error: "not an object" };
  if (input.schema !== 1) return { error: "schema" };
  if (input.kind !== "crash" && input.kind !== "unclean_exit") return { error: "kind" };
  if (typeof input.signature !== "string" || !SIGNATURE.test(input.signature)) return { error: "signature" };
  if (typeof input.install_id !== "string" || !INSTALL_ID.test(input.install_id)) return { error: "install_id" };
  if (typeof input.id !== "string" || !REPORT_ID.test(input.id)) return { error: "id" };
  const version = input.plugin?.version;
  const build = input.game?.build;
  if (typeof version !== "string" || !VERSION.test(version)) return { error: "version" };
  if (typeof build !== "string" || !BUILD.test(build)) return { error: "build" };

  const body = {};
  for (const key of Object.keys(input)) {
    if (!KEEP.has(key)) continue;
    if (Object.hasOwn(MEMBER_FIELDS, key)) {
      // Checked field by field (flat values and one short array), never walked: deep junk here can't cost a save.
      const picked = pickFields(input[key], MEMBER_FIELDS[key]);
      if (picked) body[key] = picked;
    } else {
      body[key] = scrub(input[key]);
    }
  }
  if (Array.isArray(body.modules)) {
    body.modules = body.modules.map((m) => ({ name: shortString(m?.name), base: m?.base, size: m?.size, timestamp: m?.timestamp }));
  } else {
    delete body.modules;
  }
  const where = input.kind === "crash" ? input.context : input.previous_session;
  return {
    report: {
      id: input.id,
      install_id: input.install_id,
      signature: input.signature,
      kind: input.kind,
      version,
      build,
      server: shortString(scrub(where?.server)),
      track: shortString(scrub(where?.track)),
      body,
    },
  };
}

export function cleanCounts(input) {
  if (!input || typeof input !== "object" || Array.isArray(input)) return { error: "not an object" };
  if (input.schema !== 1) return { error: "schema" };
  if (typeof input.counts !== "object" || input.counts === null || Array.isArray(input.counts)) return { error: "counts" };
  const out = {};
  for (const fix of FIXES) {
    const n = input.counts[fix];
    if (n === undefined) continue;
    if (!Number.isInteger(n) || n < 0 || n > MAX_COUNT) return { error: "count" };
    if (n > 0) out[fix] = n;
  }
  return { counts: out };
}

// Everything here has passed the patterns above, so nothing a sender wrote free-form can reach a public page.
export function badge(total) {
  return { schemaVersion: 1, label: "crashes prevented", message: String(total), color: "39ff14" };
}

export function issueTitle(signature) {
  return `Crash: ${signature}`;
}

export function issueBody(row, installs) {
  return [
    "Filed automatically from players' crash reports. Raw reports aren't public.",
    "",
    `- Crash location: \`${row.signature}\``,
    `- Reports: ${row.reports}`,
    `- Players affected: ${installs}`,
    `- First seen: ${row.first_day}`,
    `- Last seen: ${row.last_day}`,
    `- Latest plugin version: ${row.last_version}`,
    `- Latest game build: ${row.last_build}`,
  ].join("\n");
}
