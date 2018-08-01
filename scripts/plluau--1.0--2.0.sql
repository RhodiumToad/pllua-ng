\echo Use "ALTER EXTENSION plluau UPDATE TO '2.0'" to load this file. \quit

-- nothing actually needed here, the version change is cosmetic
-- but we take the opportunity to fix up some dubious properties

ALTER FUNCTION plluau_call_handler() VOLATILE CALLED ON NULL INPUT;
ALTER FUNCTION plluau_inline_handler(internal) VOLATILE STRICT;
ALTER FUNCTION plluau_validator(oid) VOLATILE STRICT;

--end
