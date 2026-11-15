# AI COLUMN 实现与验证

本改动在既有 AI 函数、文件 SQL 基础上新增异步 AI 列。
原有两题专用实现和测试保持不变；系统变量注册和共享解析入口追加新功能。

## 使用

先通过 `DBMS_AI_SERVICE` 建立 completion / dense_embedding 模型及端点，再创建表：

```sql
CREATE TABLE docs (
  id BIGINT PRIMARY KEY,
  content VARCHAR(200),
  summary VARCHAR(400) AI COLUMN (AI_COMPLETE('my_model', content))
    WITH (sync_mode='async', max_retries=2)
);
INSERT INTO docs(id, content) VALUES (1, 'example');
SHOW AI COLUMNS FROM docs;
ALTER TABLE docs RETRY AI COLUMN summary WHERE id=1;
SET SESSION ai_column_strict_read=ON;
SELECT summary FROM docs;
```

严格读取默认关闭。启用后，只检查语句实际引用的 AI 列；该列任意行尚未 READY 都返回11120，包括查询条件没有选中的行。错误包含 table_id、column_id。FAILED 也属于非 READY。
RETRY 的裸 `status` 表示该 AI 列状态（PENDING/READY/FAILED）；反引号引用的 `status` 表示表内普通列。

## 事务及后台处理

依赖列或主键变化时，业务数据失效和任务替换由同一事务提交或回滚。
普通列的无关更新保留已有结果；直接给 AI 结果列赋值会被拒绝。
任务保存输入快照、定义版本、请求 ID、重试次数及租约，HTTP 请求在领取事务外执行。
同次行变更中的多个 AI 列先暂存、再由一个事务一起发布；发布检查请求 ID，避免旧请求覆盖新值。
Worker 随数据库服务启动并停止，支持过期租约重新领取及终态暂存任务重新发布。
实际向量结果通过原生 UPDATE 路径写回，使已有向量索引随之更新。

## 官方验证

使用仓库原版 `tools/deploy/mysql_test/test_suite/ai_column` 中的 SQL、Mock 和结果文件：

- `ai_column_ddl`
- `ai_column_t1_dml`
- `ai_column_worker`
- `ai_column_retry_strict`
- `ai_column_vector_index`

使用部署工具 mysqltest runner 运行上述五组；它们需要运行中的 seekdb、MySQL 客户端以及 Python 3。
不要修改 expected 结果或 Mock 来掩盖执行失败。

定义解析的补充单元测试是 `unittest/sql/resolver/test_ai_column_definition.cpp`，使用 C++17 编译，并加入 src、src/query/api、src/oblib 和依赖 rapidjson 的 include 路径。
测试涵盖定义往返、类型约束、非法表达式、选项和 RETRY 谓词的字符串/标识符/注释保留。

完整编译成功仅证明构建可用；官方 SQL 测试、重启恢复及原有两题回归应分别记录执行结果。外部提测是否通过以平台结果为准。

## 本地验证记录（2026-11-16）

在 `feat/competition-ai-cmake`、基线 `9cf5a14ebf1cd71d98f193a9f0cce42fa93a22f9` 上，CMake Release 使用 `-j2` 完成真实构建。
专用 SQL 2884 实例上，上述五组原版 mysqltest 均通过，原始 SQL、Mock、expected 未修改。
额外检查通过：严格读取/缓存计划/事务 RETRY 回滚 4 项，持久任务重启恢复 3 项。
原有赛题一 Python 测试 8 项通过；原有赛题二文件 SQL 测试 36 项通过。
这些结果证明本地所执行的测试通过；比赛平台隐藏测试与最终得分尚未验证。

向量失效需要同时维护原生 VID，以及清除 AI 结果表达式的派生缓存。
否则主表虽为 NULL，向量索引的生成列仍可能使用更新前的数据。
原版向量测试覆盖事务内旧向量排除、提交后重新发布、ANN 查询和回滚恢复。

已知基线限制：旧端点管理用例涉及的 `max_concurrency` 不是本次新增功能，未宣称整个 seekdb 测试集全部通过。
