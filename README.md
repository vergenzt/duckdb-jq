# duckdb-jq

A DuckDB extension that exposes [`jq`](https://jqlang.org/) as a scalar SQL function, letting you directly run jq filters on JSON values in SQL queries.

The extension links `libjq` (with its bundled oniguruma regex engine) statically, so no external `jq` binary is required at runtime.

This repository is based on https://github.com/duckdb/extension-template.

## Installation

- [ ] TODO: Set up distribution / installation instructions

## Functions

### `jq(input JSON, filter VARCHAR) → JSON`

Run the given jq `filter` against the given `input` expression, expecting at most one result per input value.

> [!important]
>
> If `filter` yields more than one JSON value, an error is raised at execution time.
>
> If you wish to return multiple results per input, wrap your filter in array brackets: `[ ... ]`.

Parameters:

- `input` — a JSON value
- `filter` — a [jq filter](https://jqlang.org/manual/v1.8); must be a **constant expression** (it's compiled once at bind time and reused across rows)

Return value:

* If filter yields a single JSON value (*including JSON-`null`*!), returns that value.
* If filter yields no value (i.e. `empty`), returns SQL `NULL`.

Additional error cases:

* If input is not valid JSON, an error is raised at execution time.
* If filter string is not valid jq syntax, an error is raised at bind time.

#### // TODO: `unnest_multiple` keyword argument

to "unnest" multiple results; so `select foo, bar, jq(<...>, unnest_multiple:=true) as blah, baz` behaves similarly to jq's own behavior for e.g. `jq '{ foo, bar, blah: <...>, baz }'`

### // TODO: `jq_agg` aggregate function

Filter receives all JSON inputs from each group as a "slurped" array, and is expected to output one value.

## Examples

<!-- TODO: generator snippet to keep this in sync with `test/sql/jq.test` -->

```sql
-- Pick a single value
SELECT jq('{"a": 1, "b": [10,20,30]}', '.b[1]');
-- 20

-- More complicated filter
SELECT jq('{"a": 1, "b": [10,20,30]}', '[ {a} + {b: .b[]} ]');
-- [{"a":1,"b":10},{"a":1,"b":20},{"a":1,"b":30}]

-- Built-in jq functions, including oniguruma-backed ones
SELECT jq('{"name": "alice"}', '.name | ascii_upcase');
-- "ALICE"

-- Empty results become SQL NULL
SELECT jq('1', 'empty') IS NULL;
-- true

-- Apply a fixed filter across a column of JSON values
SELECT jq(j, '.x') FROM (VALUES ('{"x":1}'), ('{"x":2}')) t(j);
-- 1
-- 2
```

## Building

The jq source lives in the `./jq` submodule and is built via its own autotools build. Make sure submodules are initialized:

```sh
git submodule update --init --recursive
```

Then build:

```sh
make
```

The main binaries produced are:

```
./build/release/duckdb
./build/release/test/unittest
./build/release/extension/jq/jq.duckdb_extension
```

- `duckdb` is the DuckDB shell with the extension preloaded.
- `unittest` is the DuckDB test runner with the extension linked in.
- `jq.duckdb_extension` is the loadable binary as it would be distributed.

## Running the extension

Start the shell with the extension preloaded:

```sh
./build/release/duckdb
```

Then use `jq(...)` directly in SQL — see the examples above.

## Running the tests

SQL tests live in `./test/sql`. Run them with:

```sh
make test
```
