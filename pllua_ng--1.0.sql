\echo Use "CREATE EXTENSION pllua_ng" to load this file. \quit

CREATE FUNCTION pllua_ng_call_handler()
  RETURNS language_handler AS 'MODULE_PATHNAME', 'pllua_call_handler'
  LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION pllua_ng_inline_handler(internal)
  RETURNS VOID AS 'MODULE_PATHNAME', 'pllua_inline_handler'
  LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION pllua_ng_validator(oid)
  RETURNS VOID AS 'MODULE_PATHNAME', 'pllua_validator'
  LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION plluau_ng_call_handler()
  RETURNS language_handler AS 'MODULE_PATHNAME', 'plluau_call_handler'
  LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION plluau_ng_inline_handler(internal)
  RETURNS VOID AS 'MODULE_PATHNAME', 'plluau_inline_handler'
  LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION plluau_ng_validator(oid)
  RETURNS VOID AS 'MODULE_PATHNAME', 'plluau_validator'
  LANGUAGE C IMMUTABLE STRICT;

CREATE TRUSTED LANGUAGE pllua_ng
  HANDLER pllua_ng_call_handler
  INLINE pllua_ng_inline_handler
  VALIDATOR pllua_ng_validator;

CREATE LANGUAGE plluau_ng
  HANDLER plluau_ng_call_handler
  INLINE plluau_ng_inline_handler
  VALIDATOR plluau_ng_validator;

--
