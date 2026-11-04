-- For the database created before crash types needed approval. Run once; schema.sql already has these for a new database.
ALTER TABLE signatures ADD COLUMN approved INTEGER NOT NULL DEFAULT 0;
ALTER TABLE signatures ADD COLUMN issue_synced_day TEXT;
