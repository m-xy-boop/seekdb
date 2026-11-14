# AI function enhancement on the competition baseline

Base: official `2026-competition`, commit
`5bce9e06d1281347ad61344e4b01fab9158c58a2`.

This implementation preserves the competition branch's CMake build entry and
mutable C++ JSON tree APIs. The Python service is a local protocol fixture, not
the competition service. Production behavior is implemented in the SQL engine.

Build with the branch's pinned Rust 1.98.1 toolchain and initialized SDK:

```bash
./build.sh release
cmake --build build_release --target seekdb -j2
```

Use a disposable database on port 2884 and start the fixture on port 8000.
Then run `test_ai_functions.py --client PATH --port 2884 -v`, and the official
`ai_function.ai_function_enhancement` mysqltest case. The fixture records HTTP
requests in `/tmp/seekdb-ai-mock-requests.jsonl`.

Validation (2026-11-15, Ubuntu/WSL2, x86_64):

- Complete CMake `seekdb` target built successfully with `-j2` in a separate
  source mirror. All nine changed production files were checked byte-for-byte
  against this checkout before SQL execution.
- Unmodified official `ai_function.ai_function_enhancement` test and expected
  result passed against the new binary and the independently authored fixture.
- `ai_function.ai_prompt` and `ai_function.ai_model_ddl` passed.
- Eight Python E2E tests passed, including a 600-row embedding query with real
  multi-input HTTP requests, response reordering, varying models/dimensions,
  NULLs, long text, native Ollama routes, stable rerank ties, and error handling.
- `ai_function.ai_model_endpoint_ddl` FAILED at its first `max_concurrency`
  configuration. The pinned baseline endpoint parser has no implementation for
  that parameter (nor the corresponding view column). The test and its expected
  output remain unchanged; this failure is not counted as a passing regression.

The fixture is not an official model service. Local success does not establish
platform acceptance, hidden-test results, or a contest score. The platform's
previous generic execution failure has no diagnostic log, so its exact cause
remains unconfirmed. No complete repository regression or sanitizer run was done.
