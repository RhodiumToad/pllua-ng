/* hstore_pllua--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION hstore_pllua" to load this file. \quit

-- force module load; at runtime this doesn't matter because the transforms
-- can't be legitimately called except from within pllua.so, but we want
-- CREATE EXTENSION to work even in a cold session
do language pllua $$ $$;

CREATE FUNCTION hstore_to_pllua(val internal) RETURNS internal
LANGUAGE C STRICT IMMUTABLE
AS 'MODULE_PATHNAME';

CREATE FUNCTION pllua_to_hstore(val internal) RETURNS hstore
LANGUAGE C STRICT IMMUTABLE
AS 'MODULE_PATHNAME';

CREATE TRANSFORM FOR hstore LANGUAGE pllua (
    FROM SQL WITH FUNCTION hstore_to_pllua(internal),
    TO SQL WITH FUNCTION pllua_to_hstore(internal)
);
