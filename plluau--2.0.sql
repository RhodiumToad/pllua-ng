\echo Use "CREATE EXTENSION plluau" to load this file. \quit

CREATE FUNCTION plluau_call_handler()
  RETURNS language_handler AS 'MODULE_PATHNAME', 'plluau_call_handler'
  LANGUAGE C;

CREATE FUNCTION plluau_inline_handler(internal)
  RETURNS VOID AS 'MODULE_PATHNAME', 'plluau_inline_handler'
  LANGUAGE C STRICT;

CREATE FUNCTION plluau_validator(oid)
  RETURNS VOID AS 'MODULE_PATHNAME', 'plluau_validator'
  LANGUAGE C STRICT;

CREATE LANGUAGE plluau
  HANDLER plluau_call_handler
  INLINE plluau_inline_handler
  VALIDATOR plluau_validator;

--
