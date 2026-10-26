#pragma once

#include <array>
#include <string_view>

// Strata Library migrations, retained verbatim for on-disk compatibility.
namespace Slic3r::ModelLibrary {
inline constexpr std::array<std::string_view, 6> migrations{
R"schema(
CREATE TABLE IF NOT EXISTS models (
  id           INTEGER PRIMARY KEY,
  name         TEXT NOT NULL,
  dir          TEXT NOT NULL,
  designer     TEXT,
  description  TEXT,
  license      TEXT,
  notes        TEXT NOT NULL DEFAULT '',
  source_url   TEXT,
  source_site  TEXT,
  source_path  TEXT,
  source_hash  TEXT,
  favorite     INTEGER NOT NULL DEFAULT 0,
  thumb        TEXT,
  added_at     TEXT NOT NULL,
  updated_at   TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS models_source_hash ON models(source_hash);

CREATE TABLE IF NOT EXISTS files (
  id            INTEGER PRIMARY KEY,
  model_id      INTEGER NOT NULL REFERENCES models(id) ON DELETE CASCADE,
  kind          TEXT NOT NULL,
  format        TEXT NOT NULL,
  rel_path      TEXT NOT NULL,
  original_name TEXT NOT NULL,
  size          INTEGER NOT NULL,
  hash          TEXT NOT NULL,
  thumb         TEXT,
  sliced        INTEGER NOT NULL DEFAULT 0,
  triangles     INTEGER,
  vertices      INTEGER,
  size_x        REAL,
  size_y        REAL,
  size_z        REAL,
  volume        REAL,
  area          REAL,
  meta          TEXT NOT NULL DEFAULT '[]',
  error         TEXT
);
CREATE INDEX IF NOT EXISTS files_hash ON files(hash);
CREATE INDEX IF NOT EXISTS files_model ON files(model_id);

CREATE TABLE IF NOT EXISTS tags (
  id   INTEGER PRIMARY KEY,
  name TEXT NOT NULL UNIQUE COLLATE NOCASE
);
CREATE TABLE IF NOT EXISTS model_tags (
  model_id INTEGER NOT NULL REFERENCES models(id) ON DELETE CASCADE,
  tag_id   INTEGER NOT NULL REFERENCES tags(id) ON DELETE CASCADE,
  PRIMARY KEY (model_id, tag_id)
);

CREATE TABLE IF NOT EXISTS watch_folders (
  id           INTEGER PRIMARY KEY,
  path         TEXT NOT NULL UNIQUE,
  recursive    INTEGER NOT NULL DEFAULT 0,
  enabled      INTEGER NOT NULL DEFAULT 1,
  added_at     TEXT NOT NULL,
  last_scan_at TEXT
);

CREATE TABLE IF NOT EXISTS activity (
  id       INTEGER PRIMARY KEY,
  at       TEXT NOT NULL,
  path     TEXT NOT NULL,
  status   TEXT NOT NULL,
  message  TEXT,
  model_id INTEGER
);

CREATE TABLE IF NOT EXISTS settings (
  key   TEXT PRIMARY KEY,
  value TEXT NOT NULL
);
)schema",
R"schema(
ALTER TABLE models ADD COLUMN archived_at TEXT;
ALTER TABLE models ADD COLUMN stack_id INTEGER;
ALTER TABLE models ADD COLUMN stack_top INTEGER NOT NULL DEFAULT 0;
ALTER TABLE models ADD COLUMN unstacked INTEGER NOT NULL DEFAULT 0;
ALTER TABLE models ADD COLUMN dismissed TEXT NOT NULL DEFAULT '';
ALTER TABLE model_tags ADD COLUMN source TEXT NOT NULL DEFAULT 'user';
CREATE INDEX IF NOT EXISTS models_stack ON models(stack_id);
)schema",
R"schema(
CREATE TABLE collections (
  id         INTEGER PRIMARY KEY,
  name       TEXT NOT NULL,
  query      TEXT,
  created_at TEXT NOT NULL
);
CREATE TABLE collection_models (
  collection_id INTEGER NOT NULL REFERENCES collections(id) ON DELETE CASCADE,
  model_id      INTEGER NOT NULL REFERENCES models(id) ON DELETE CASCADE,
  added_at      TEXT NOT NULL,
  PRIMARY KEY (collection_id, model_id)
);
CREATE INDEX collection_models_model ON collection_models(model_id);
CREATE TABLE prints (
  id        INTEGER PRIMARY KEY,
  model_id  INTEGER NOT NULL REFERENCES models(id) ON DELETE CASCADE,
  file_name TEXT,
  at        TEXT NOT NULL,
  outcome   TEXT NOT NULL,
  printer   TEXT,
  filament  TEXT,
  notes     TEXT NOT NULL DEFAULT ''
);
CREATE INDEX prints_model ON prints(model_id);
ALTER TABLE models ADD COLUMN reshaped INTEGER NOT NULL DEFAULT 0;
-- Where models merged into another came from, so importing them again is
-- still a duplicate.
CREATE TABLE merged_sources (
  model_id    INTEGER NOT NULL REFERENCES models(id) ON DELETE CASCADE,
  source_path TEXT,
  source_hash TEXT
);
CREATE INDEX merged_sources_hash ON merged_sources(source_hash);
)schema",
R"schema(
ALTER TABLE models ADD COLUMN ai_tagged_at TEXT;
)schema",
R"schema(
ALTER TABLE models ADD COLUMN page_read_at TEXT;
)schema",
R"schema(
ALTER TABLE files ADD COLUMN geometry_hash TEXT;
ALTER TABLE files ADD COLUMN geometry_revision INTEGER NOT NULL DEFAULT 0;
CREATE INDEX files_geometry_hash ON files(geometry_hash) WHERE geometry_hash IS NOT NULL;
)schema"
};
}
