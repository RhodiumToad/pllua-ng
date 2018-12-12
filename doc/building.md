Building PL/Lua
===============

GNU Make is required to build, as usual for PostgreSQL extensions.

This module assumes you have already built Lua itself, either as a
shared library or as an archive library with `-fPIC` (on most
platforms a non-PIC archive library will not work). A shared library
is recommended.


Building the `pllua` module
---------------------------

Lua unfortunately does not provide much in the way of infrastructure
for determining build locations; accordingly, those have to be
specified explicitly to build this module. The following values must
be defined on the `make` command line or in the environment:

+ `LUA_INCDIR`\
  directory containing `lua.h`, `luaconf.h`, `lualib.h`
+ `LUALIB`\
  linker options needed to link, typically `-Lsomedir -llua-5.3`

And if building with standard Lua:

+ `LUAC`\
  name or full path of the luac binary (bytecode compiler)
+ `LUA`\
  name or full path of the lua binary

Or if building with Luajit:

+ `LUAJIT`\
  name or full path of the luajit binary

In addition, as for all PGXS modules, `PG_CONFIG` must be set to the
name or full path of the `pg_config` binary corresponding to the
PostgreSQL server version being compiled against, unless the correct
`pg_config` is already findable via `$PATH` (which is usually not the
case).

Example:

    make PG_CONFIG=/usr/lib/postgresql/10/bin/pg_config \
         LUA_INCDIR="/usr/include/lua5.3" \
         LUALIB="-llua5.3" \
         LUAC="luac5.3" LUA="lua5.3" install


Building the `hstore_pllua` module
----------------------------------

Currently, the `hstore_pllua` module does not need `LUALIB` on most
platforms (since it will reference lua functions either exported by
`pllua.so` or by a library loaded by `pllua.so`).

You should specify `LUALIB` if you're using a shared lua library and
your platform isn't exposing symbols from one module's loaded
dependencies to other modules. If you're using a shared library then
specifying `LIBLUA` unnecessarily is harmless.

Example:

    make -C hstore \
         PG_CONFIG=/usr/lib/postgresql/10/bin/pg_config \
         LUA_INCDIR="/usr/include/lua5.3" \
         LUAC="luac5.3" LUA="lua5.3" install


Building the documentation
--------------------------

Specifying `BUILD_DOCS=1` will build the HTML documentation from the
Markdown doc sources; this requires `cmark` and `xsltproc`.

Additionally specifying `BUILD_ICON=1` will include the favicon in the
HTML documentation; this requires ImageMagick's `convert` program.


`VPATH` builds
--------------

Both modules support building with `VPATH`, which can either be
explicitly set or, if `make -f /path/to/Makefile` is used to specify a
makefile outside the current directory and `VPATH` is not explicitly
set, then `VPATH` will be set to the directory containing the
Makefile.


Luajit options
--------------

`PLLUA_CONFIG_OPTS` can be used to control certain aspects of pllua's
behavior when built with Luajit.

+ `-DNO_LUAJIT`\
  disables all use of luajit features
+ `-DUSE_INT8_CDATA`\
  convert sql bigints to cdata int64_t

The latter is off by default because it has some possibly undesirable
effects on bigint handling, especially when serializing to JSON.
However, as long as `NO_LUAJIT` was not specified, cdata integers can be
freely returned from functions or passed to SQL type constructors.

Actual JIT compilation of user-supplied lua code is not affected by
any of these options.


Porting options
---------------

If you have problems building on an unusual platform, then these
options might be useful. The values shown are the defaults if any.

+ `BIN_LD`\
  `$(LD) -r -b binary`

The command `$(BIN_LD) -o file.o dir/datafile.ext` is assumed to
produce `file.o` containing a data section populated with the content of
`datafile.ext`, with symbols `_binary_dir_datafile_ext_start` and
`_binary_dir_datafile_ext_end` bracketing the data. The default is
believed to work for most GNU ld and (recent) LLVM lld targets, but it
is known to fail on some non-mainstream architecture distributions.

The value of `BIN_LD` can be set to any suitable equivalent command.

+ `OBJCOPY`\
  `objcopy`

The output of `BIN_LD` is passed through `OBJCOPY` to make the data
section read-only, but this is a non-critical operation. If no working
objcopy is available, this can be set to 'false'.

+ `BIN_ARCH`\
  <i>unset</i>
+ `BIN_FMT`\
  <i>unset</i>

If both of these are set, then `BIN_LD` is assumed not to work, and instead
the command

    $(OBJCOPY) -B $(BIN_ARCH) -I binary -O $(BIN_FMT) datafile.ext file.o

will be used in its place. The following values have been used on
linux-mips64el to work around build failures with `ld -r`:

    BIN_ARCH=mips:isa64r2 BIN_FMT=elf64-tradlittlemips

+ `LUAJITC`\
  `$(LUAJIT) -b -g -t raw`

On Luajit, the bytecode compile option only works if luajit has been
fully installed. In test environments where only the luajit build dir
is otherwise needed, the bytecode compilation step can be skipped by
setting `LUAJITC="cp"`. (The bytecode compile can also be skipped in
non-luajit builds by setting `LUAC='$(REORDER_O) cp'` but this is not
expected to be useful.)

<!--eof-->
