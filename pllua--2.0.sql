\echo Use "CREATE EXTENSION pllua" to load this file. \quit

CREATE FUNCTION pllua_call_handler()
  RETURNS language_handler AS 'MODULE_PATHNAME', 'pllua_call_handler'
  LANGUAGE C;

CREATE FUNCTION pllua_inline_handler(internal)
  RETURNS VOID AS 'MODULE_PATHNAME', 'pllua_inline_handler'
  LANGUAGE C STRICT;

CREATE FUNCTION pllua_validator(oid)
  RETURNS VOID AS 'MODULE_PATHNAME', 'pllua_validator'
  LANGUAGE C STRICT;

CREATE TRUSTED LANGUAGE pllua
  HANDLER pllua_call_handler
  INLINE pllua_inline_handler
  VALIDATOR pllua_validator;

--
