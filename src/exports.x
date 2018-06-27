{
  global:
    Pg_magic_func;
    _PG_init;
    pllua_validator;	    pg_finfo_pllua_validator;
    pllua_call_handler;	    pg_finfo_pllua_call_handler;
    pllua_inline_handler;   pg_finfo_pllua_inline_handler;
    plluau_validator;	    pg_finfo_plluau_validator;
    plluau_call_handler;    pg_finfo_plluau_call_handler;
    plluau_inline_handler;  pg_finfo_plluau_inline_handler;
    pllua_rethrow_from_pg;
    pllua_pcall_nothrow;
    pllua_cpcall;
    pllua_pcall;
    pllua_trampoline;

  local: *;
};
