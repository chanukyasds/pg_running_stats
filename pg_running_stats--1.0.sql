-- complain if sourced directly
\echo Use "CREATE EXTENSION pg_running_stats" to load this file. \quit

-- Result composite type (once)
DO $$
BEGIN
  IF NOT EXISTS (SELECT 1 FROM pg_type WHERE typname = 'rstat_result_t') THEN
    CREATE TYPE rstat_result_t AS (
      n      bigint,
      mean   double precision,
      var    double precision,
      stddev double precision,
      skew   double precision,
      kurt   double precision,
      min    double precision,
      max    double precision
    );
  END IF;
END$$;

-- C bindings
CREATE OR REPLACE FUNCTION rstat_sfunc(state bytea, x double precision)
RETURNS bytea
AS 'pg_running_stats', 'rstat_sfunc'
LANGUAGE C IMMUTABLE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION rstat_combine(a bytea, b bytea)
RETURNS bytea
AS 'pg_running_stats', 'rstat_combine'
LANGUAGE C IMMUTABLE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION rstat_final(state bytea)
RETURNS rstat_result_t
AS 'pg_running_stats', 'rstat_final'
LANGUAGE C IMMUTABLE PARALLEL SAFE;

-- Helpers (aliases)
CREATE OR REPLACE FUNCTION rstat_state_merge(a bytea, b bytea)
RETURNS bytea
AS 'pg_running_stats', 'rstat_combine'
LANGUAGE C IMMUTABLE PARALLEL SAFE;

CREATE OR REPLACE FUNCTION rstat_state_result(state bytea)
RETURNS rstat_result_t
AS 'pg_running_stats', 'rstat_final'
LANGUAGE C IMMUTABLE PARALLEL SAFE;

-- Aggregate returning raw state (mergeable)
DROP AGGREGATE IF EXISTS rstat_state(double precision);
CREATE AGGREGATE rstat_state(double precision)
(
  SFUNC       = rstat_sfunc,
  STYPE       = bytea,
  COMBINEFUNC = rstat_combine,
  PARALLEL    = safe
);

-- One-run aggregate returning full record (mergeable)
DROP AGGREGATE IF EXISTS running_stats(double precision);
CREATE AGGREGATE running_stats(double precision)
(
  SFUNC       = rstat_sfunc,
  STYPE       = bytea,
  FINALFUNC   = rstat_final,
  COMBINEFUNC = rstat_combine,
  PARALLEL    = safe
);
