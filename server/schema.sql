-- Reports are deleted after 90 days by the daily job. No IP addresses anywhere.
CREATE TABLE IF NOT EXISTS reports (
  id TEXT NOT NULL,
  install_id TEXT NOT NULL,
  signature TEXT NOT NULL,
  kind TEXT NOT NULL,
  plugin_version TEXT NOT NULL,
  game_build TEXT NOT NULL,
  server TEXT,
  track TEXT,
  day TEXT NOT NULL,
  body TEXT NOT NULL,
  PRIMARY KEY (id, install_id)
);
CREATE INDEX IF NOT EXISTS reports_signature_day ON reports (signature, day);
CREATE INDEX IF NOT EXISTS reports_day ON reports (day);

CREATE TABLE IF NOT EXISTS signatures (
  signature TEXT PRIMARY KEY,
  reports INTEGER NOT NULL DEFAULT 0,
  first_day TEXT NOT NULL,
  last_day TEXT NOT NULL,
  last_version TEXT,
  last_build TEXT,
  issue_number INTEGER,
  issue_claimed_at INTEGER,
  issue_reports INTEGER NOT NULL DEFAULT 0,
  -- Set by the maintainer: nothing reaches a public issue before this.
  approved INTEGER NOT NULL DEFAULT 0,
  issue_synced_day TEXT
);

CREATE TABLE IF NOT EXISTS signature_installs (
  signature TEXT NOT NULL,
  install_id TEXT NOT NULL,
  last_day TEXT NOT NULL,
  PRIMARY KEY (signature, install_id)
);

-- Crash counts carry no install ID.
CREATE TABLE IF NOT EXISTS daily_counts (
  day TEXT NOT NULL,
  fix TEXT NOT NULL,
  count INTEGER NOT NULL,
  PRIMARY KEY (day, fix)
);

-- Reports already received, kept apart from the full reports so a retry is recognised even if that report wasn't stored.
CREATE TABLE IF NOT EXISTS seen (
  id TEXT NOT NULL,
  install_id TEXT NOT NULL,
  day TEXT NOT NULL,
  PRIMARY KEY (id, install_id)
);
CREATE INDEX IF NOT EXISTS seen_day ON seen (day);

-- Daily limits across all senders.
CREATE TABLE IF NOT EXISTS daily_budget (
  day TEXT NOT NULL,
  kind TEXT NOT NULL,
  n INTEGER NOT NULL,
  PRIMARY KEY (day, kind)
);
