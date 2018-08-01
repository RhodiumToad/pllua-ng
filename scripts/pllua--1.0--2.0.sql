\echo Use "ALTER EXTENSION pllua UPDATE TO '2.0'" to load this file. \quit

-- nothing actually needed here, the version change is cosmetic;
-- but we take the opportunity to fix up some dubious properties

ALTER FUNCTION pllua_call_handler() VOLATILE CALLED ON NULL INPUT;
ALTER FUNCTION pllua_inline_handler(internal) VOLATILE STRICT;
ALTER FUNCTION pllua_validator(oid) VOLATILE STRICT;

--end
