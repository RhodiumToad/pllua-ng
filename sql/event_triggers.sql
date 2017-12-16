--

\set VERBOSITY terse
\set QUIET 0

-- Test event triggers.

create function evtrig() returns event_trigger language pllua_ng as $$
  print(trigger.event, trigger.tag)
$$;

create event trigger et1 on ddl_command_start execute procedure evtrig();
create event trigger et2 on ddl_command_end execute procedure evtrig();
create event trigger et3 on sql_drop execute procedure evtrig();
create event trigger et4 on table_rewrite execute procedure evtrig();

create table evt1 (a text);
alter table evt1 alter column a type integer using null;
drop table evt1;

drop event trigger et1;
drop event trigger et2;
drop event trigger et3;
drop event trigger et4;

--end
