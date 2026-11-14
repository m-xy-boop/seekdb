# AI function enhancement regression

These tests invoke seekdb SQL expressions over the MySQL protocol. The Python
service is a local HTTP protocol fixture, not a model or the competition's
unpublished Mock AI Service. Production behavior is implemented in C++ SQL
expressions and provider utilities; the fixture does not implement SQL behavior.

Build the database from this branch:

```bash
python3 build.py build -j2 seekdb
```

Start the fixture in a separate terminal:

```bash
python3 unittest/sql/engine/expr/mock_ai_service.py
```

Start a disposable test instance with the repository's `tools/deploy/sdb.py`
using the built binary, a dedicated empty base directory, and port 2884. Then run:

```bash
python3 unittest/sql/engine/expr/test_ai_functions.py \
  --client "$PWD/deps/3rd/u01/obclient/bin/obclient" --port 2884 -v
```

The tests create `enh_*` models/endpoints and an `enh_rows` table in the `test`
database. Use a disposable instance. Successful completion removes those objects.
An interrupted setup can leave them behind; recreate the disposable instance
before rerunning. The fixture binds only to 127.0.0.1:8000 and records requests in
`/tmp/seekdb-ai-mock-requests.jsonl` (override with `AI_MOCK_LOG` for both processes).
No real model access or reusable credential is needed.

Coverage includes JSON and nested prompts, repeated placeholders and invalid
indices, Unicode and long text, OpenAI completion configurations, optional NULL
configuration, embedding dimensions and malformed values, mixed-case Ollama and
empty keys, multi-batch reranking with stable ties, and embedding row mapping
through NULL/filter/LIMIT. Requests are inspected to confirm actual cross-row
batching. Other tests verify protocol errors return SQL errors without making the
server unusable.

Also run the established `ai_function.ai_prompt`, model DDL, and endpoint DDL
mysqltest cases to check existing SQL behavior. The prompt regression's expected
output has been recorded and verified for its newly supported JSON and TEXT inputs.

Competition submission uses the registered repository plus the branch and its
full commit SHA. Local test results do not establish an official score.

## Local validation (2026-11-15)

Full server build succeeded on Ubuntu 22.04 / WSL2 x86_64. The expanded SQL
end-to-end runner passed all 8 tests, including 600-row mapping and varying
models/dimensions. The existing mysqltest cases `ai_function.ai_prompt`,
`ai_function.ai_model_ddl`, and `ai_function.ai_model_endpoint_ddl` all passed.
The HTTP fixture is independently authored; official competition acceptance and
full repository regressions have not been run.
