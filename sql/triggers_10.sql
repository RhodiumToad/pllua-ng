--

\set VERBOSITY terse
\set QUIET 0

-- Test pg10+ trigger functionality.

create table trigtst2 (
  id integer primary key,
  name text,
  flag boolean,
  qty integer,
  weight numeric
);

create function ttrig1() returns trigger language pllua
as $$
  print(trigger.name,...)
  print(trigger.when, trigger.level, trigger.operation, trigger.relation.name)
  for r in spi.rows([[ select * from newtab ]]) do print(r) end
$$;

create function ttrig2() returns trigger language pllua
as $$
  print(trigger.name,...)
  print(trigger.when, trigger.level, trigger.operation, trigger.relation.name)
  for r in spi.rows([[ select 'old', * from oldtab union all select 'new', * from newtab ]]) do print(r) end
$$;

create function ttrig3() returns trigger language pllua
as $$
  print(trigger.name,...)
  print(trigger.when, trigger.level, trigger.operation, trigger.relation.name)
  for r in spi.rows([[ select * from oldtab ]]) do print(r) end
$$;

create trigger t1
  after insert on trigtst2
  referencing new table as newtab
  for each statement
  execute procedure ttrig1('t1 insert');

create trigger t2
  after update on trigtst2
  referencing old table as oldtab
              new table as newtab
  for each statement
  execute procedure ttrig2('t2 update');

create trigger t3
  after delete on trigtst2
  referencing old table as oldtab
  for each statement
  execute procedure ttrig3('t3 delete');

insert into trigtst2
  values (1, 'fred', true, 23, 1.73),
  	 (2, 'jim', false, 11, 3.1),
	 (3, 'sheila', false, 9, 1.3),
  	 (4, 'dougal', false, 1, 9.3),
 	 (5, 'brian', false, 31, 51.5),
	 (6, 'ermintrude', true, 91, 52.7),
	 (7, 'dylan', false, 35, 12.1),
	 (8, 'florence', false, 23, 5.4),
	 (9, 'zebedee', false, 199, 7.4);
update trigtst2 set qty = qty + 1;
delete from trigtst2 where name = 'sheila';

--
