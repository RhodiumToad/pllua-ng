--

\set VERBOSITY terse
\set QUIET 0

-- Test triggers.
--
-- Don't use pg10-specific stuff here; that goes in triggers_10.sql

create table trigtst (
  id integer primary key,
  name text,
  flag boolean,
  qty integer,
  weight numeric
);

create function misctrig() returns trigger language pllua
as $$
  print(trigger.name,...)
  print(trigger.when, trigger.level, trigger.operation, trigger.relation.name)
  if trigger.level == "row" then
    print(old,new)
  end
$$;

create trigger t1
  before insert or update or delete on trigtst
  for each statement
  execute procedure misctrig('foo','bar');
create trigger t2
  after insert or update or delete on trigtst
  for each statement
  execute procedure misctrig('foo','bar');

insert into trigtst
  values (1, 'fred', true, 23, 1.73),
  	 (2, 'jim', false, 11, 3.1),
	 (3, 'sheila', false, 9, 1.3),
  	 (4, 'dougal', false, 1, 9.3),
 	 (5, 'brian', false, 31, 51.5),
	 (6, 'ermintrude', true, 91, 52.7),
	 (7, 'dylan', false, 35, 12.1),
	 (8, 'florence', false, 23, 5.4),
	 (9, 'zebedee', false, 199, 7.4);
update trigtst set qty = qty + 1;
delete from trigtst where name = 'sheila';

create trigger t3
  before insert or update or delete on trigtst
  for each row
  execute procedure misctrig('wot');
create trigger t4
  after insert or update or delete on trigtst
  for each row
  execute procedure misctrig('wot');

insert into trigtst values (3, 'sheila', false, 9, 1.3);
update trigtst set flag = true where name = 'dylan';
delete from trigtst where name = 'jim';
-- check result is as expected
select * from trigtst order by id;

drop trigger t1 on trigtst;
drop trigger t2 on trigtst;
drop trigger t3 on trigtst;
drop trigger t4 on trigtst;

-- compatible mode: assign to row fields
create function modtrig1() returns trigger language pllua
as $$
  print(trigger.name,trigger.operation,old,new)
  trigger.row.weight = 10 * trigger.row.qty
  trigger.row.flag = false
  print(trigger.name,trigger.operation,old,new)
$$;

create trigger t1
  before insert or update or delete on trigtst
  for each row
  execute procedure modtrig1();

insert into trigtst values (2, 'jim', true, 11, 3.1);
update trigtst set flag = true where name = 'ermintrude';
delete from trigtst where name = 'fred';
select * from trigtst order by id;

drop trigger t1 on trigtst;

-- compatible mode: assign to row wholesale
create function modtrig2() returns trigger language pllua
as $$
  print(trigger.name,trigger.op,old,new)
  local id,name,flag,qty,weight = new.id, new.name, new.flag, new.qty, new.weight
  qty = 2 + qty
  weight = weight * 2
  flag = not flag
  trigger.row = { id = id, name = name, flag = flag, qty = qty, weight = weight }
$$;

create trigger t1
  before insert or update on trigtst
  for each row
  execute procedure modtrig2();

insert into trigtst values (1, 'fred', true, 23, 1.73);
update trigtst set flag = true where name = 'zebedee';
delete from trigtst where name = 'jim';
select * from trigtst order by id;

drop trigger t1 on trigtst;

-- compatible mode: assign to row wholesale with new datum row
create function modtrig3() returns trigger language pllua
as $$
  print(trigger.name,trigger.operation,old,new)
  local id,name,flag,qty,weight = new.id, new.name, new.flag, new.qty, new.weight
  qty = 2 + qty
  weight = weight * 2
  flag = not flag
  trigger.row = pgtype(new)(id,name,flag,qty,weight)
$$;

create trigger t1
  before insert or update on trigtst
  for each row
  execute procedure modtrig3();

insert into trigtst values (2, 'jim', false, 11, 3.1);
update trigtst set flag = true where name = 'zebedee';
delete from trigtst where name = 'fred';
select * from trigtst order by id;

drop trigger t1 on trigtst;

-- return value mode
create function modtrig4() returns trigger language pllua
as $$
  print(trigger.name,trigger.operation,old,new)
  local id,name,flag,qty,weight = new.id, new.name, new.flag, new.qty, new.weight
  qty = 2 + qty
  weight = weight * 2
  flag = not flag
  return { id = id, name = name, flag = flag, qty = qty, weight = weight }
$$;

create trigger t1
  before insert or update on trigtst
  for each row
  execute procedure modtrig4();

insert into trigtst values (1, 'fred', true, 23, 1.73);
update trigtst set flag = false where name = 'dylan';
delete from trigtst where name = 'jim';
select * from trigtst order by id;

drop trigger t1 on trigtst;

-- return value mode
create function modtrig5() returns trigger language pllua
as $$
  print(trigger.name,trigger.operation,old,new)
  local id,name,flag,qty,weight = new.id, new.name, new.flag, new.qty, new.weight
  qty = 2 + qty
  weight = weight * 2
  flag = not flag
  return pgtype(new)(id,name,flag,qty,weight)
$$;

create trigger t1
  before insert or update on trigtst
  for each row
  execute procedure modtrig5();

insert into trigtst values (2, 'jim', false, 11, 3.1);
update trigtst set flag = false where name = 'dougal';
delete from trigtst where name = 'fred';
select * from trigtst order by id;

drop trigger t1 on trigtst;

-- throw error from trigger
create function modtrig6() returns trigger language pllua
as $$
  print(trigger.name,trigger.operation,old,new)
  if new.flag ~= old.flag then error("no changing flags") end
$$;

create trigger t1
  before update on trigtst
  for each row
  execute procedure modtrig6();

update trigtst set flag = false where name = 'dougal';
select * from trigtst order by id;

drop trigger t1 on trigtst;

-- throw error from trigger
create function modtrig7() returns trigger language pllua
as $$
  print(trigger.name,trigger.operation,old,new)
  if new.flag ~= old.flag then error("no changing flags") end
$$;

create trigger t1
  before update on trigtst
  for each row
  execute procedure modtrig7();

update trigtst set flag = true where name = 'florence';
select * from trigtst order by id;

drop trigger t1 on trigtst;

-- suppress action 1
create function modtrig8() returns trigger language pllua
as $$
  print(trigger.name,trigger.operation,old,new)
  if new.flag ~= old.flag then return nil end
$$;

create trigger t1
  before update on trigtst
  for each row
  execute procedure modtrig8();

update trigtst set flag = true where name = 'florence';
select * from trigtst order by id;

drop trigger t1 on trigtst;

-- suppress action 2
create function modtrig9() returns trigger language pllua
as $$
  print(trigger.name,trigger.operation,old,new)
  if new.flag ~= old.flag then trigger.row = nil end
$$;

create trigger t1
  before update on trigtst
  for each row
  execute procedure modtrig9();

update trigtst set flag = true where name = 'florence';
select * from trigtst order by id;

drop trigger t1 on trigtst;

-- table with one column exercises several edge cases:

create table trigtst1col (col integer);
create function modtrig10() returns trigger language pllua
as $$
  print(trigger.name,trigger.operation,old,new)
  new.col = 123
$$;
create trigger t1
  before insert on trigtst1col
  for each row
  execute procedure modtrig10();
insert into trigtst1col values (1);
insert into trigtst1col values (2);
select * from trigtst1col;

create type t2col as (a integer, b text);
create table trigtst1col2 (col t2col);
create function modtrig11() returns trigger language pllua
as $$
  print(trigger.name,trigger.operation,old,new)
  new.col.a = 123
$$;
create trigger t1
  before insert on trigtst1col2
  for each row
  execute procedure modtrig11();
insert into trigtst1col2 values (row(1,'foo')::t2col);
select * from trigtst1col2;

-- exercise dropped columns:

create table trigtst1dcol (col text, dcol text);
alter table trigtst1dcol drop column dcol;
create trigger t1
  after insert or update or delete on trigtst1dcol
  for each row
  execute procedure misctrig('blah');
insert into trigtst1dcol values ('foo');
update trigtst1dcol set col = 'bar';
delete from trigtst1dcol;

--
