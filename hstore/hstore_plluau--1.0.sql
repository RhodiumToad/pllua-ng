/* hstore_plluau--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION hstore_plluau" to load this file. \quit

-- force module load; at runtime this doesn't matter because the transforms
-- can't be legitimately called except from within pllua.so, but we want
-- CREATE EXTENSION to work even in a cold session
do language plluau $$ $$;

CREATE FUNCTION hstore_to_plluau(val internal) RETURNS internal
LANGUAGE C STRICT IMMUTABLE
AS 'MODULE_PATHNAME','hstore_to_pllua';

CREATE FUNCTION plluau_to_hstore(val internal) RETURNS hstore
LANGUAGE C STRICT IMMUTABLE
AS 'MODULE_PATHNAME','pllua_to_hstore';

CREATE TRANSFORM FOR hstore LANGUAGE plluau (
    FROM SQL WITH FUNCTION hstore_to_plluau(internal),
    TO SQL WITH FUNCTION plluau_to_hstore(internal)
);
