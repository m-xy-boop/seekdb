# File SQL: implementation and validation

Based on accepted AI commit `b508790060291ee79ff9d48d08bb42f0d36b06fd`.
The 14 accepted AI source/test files remain byte-identical. File tables use
seekdb's native relational executor; no external query engine or import is used.

Supported table functions: `read_csv`, `read_json`, `read_ndjson`,
`read_parquet`, and `glob`. Native filters, ordering, aggregation and joins work
on their output. `DESCRIBE SELECT` reports inferred columns. `COPY` exports
native query/table results as CSV, without replacing existing files, and removes
partial temporary output when the query fails.

File access defaults off. Enable `enable_file_sql` in the session, and configure
`secure_file_priv` through the existing authorized Unix-socket administration
path. Input/output paths are checked against that directory; file identity and
modification signatures are checked on opening and during scanning.

CSV and flat JSONL inference scans the complete input, retaining schema rather
than all rows. Only `sample_size=-1` is supported. Limits include 8 MiB records,
1024 columns, 10000 glob directory entries and bounded Parquet metadata/pages.
Parquet supports flat nullable basic types, plain/dictionary pages, Snappy,
multiple row groups, DATA_PAGE_V1/V2 and projected column decoding. Nested,
repeated, unsupported logical types, codecs and encodings return errors.
`glob` supports patterns in the basename of one directory, without recursive or
directory-component wildcards. This implementation targets Linux/POSIX and uses
`/proc/self/fd` to validate opened paths.

## Tests

`test_file_scan_reader.cpp` is a standalone reader/writer correctness test.
Pass the decoded official Parquet fixture as its first argument and a fixture
from `make_file_sql_parquet_fixture.py` as its second argument. Coverage includes
CSV/JSONL, widening, UTF-8, modifications, path boundaries, symlinks, glob,
atomic CSV export, Parquet NULLs, Snappy, projection and multiple row groups.

`test_file_sql_e2e.py --client PATH --port 2884 --socket PATH` must target a
**disposable database**. It changes that instance's global `secure_file_priv`
through its Unix socket to a temporary directory. It checks 36 real SQL
operations, including expected failures, cross-format joins, metadata, NULLs,
empty COPY, export roundtrips, no overwrite, failure cleanup and a 100000-row
COUNT/SUM query.

Local validation on 2026-11-15: full CMake seekdb build passed; standalone
reader tests passed; official `file_sql.file_sql_basic` passed; the 36 SQL
checks passed. The local official runner replaces only SSH/scp fixture setup
and cleanup with local temporary-file installation, preserving SQL, expected
errors and the official result file. ASan/UBSan passed with leak detection
disabled; this environment does not support LeakSanitizer under ptrace.

Local tests do not establish hidden-test acceptance or a contest score.
