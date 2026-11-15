# 赛题四：全文索引热路径优化与本地验证

本补丁基于 `afa14c66e423e3dcff2ab0180f6a6018144c3a31`，沿用
`feat/competition-ai-cmake` 分支，前三题的实现和测试文件保持不变。
本地构建和正确性对照已通过；**官方基准尚未证明整体性能提升，平台得分未验证**。

## 改动

- IK 字符分类：每个字符只解码一次，再按原来的优先级判断类别，保留错误返回和 UTF-16 代理项处理。
- 停用词检查：相同字符集直接查询，避免创建临时 arena；跨字符集仍使用原转换，并在 arena 存活期间完成查询。
- 词频归并：用一次 `set_or_update` 替代查找后覆盖，减少哈希、遍历和替换节点分配。保留首次计数为 1、后续累加 `word_freq` 的行为；按排序规则相等时仍保留最后一个词形。例如希腊字母 `σ` 和 `ς` 不得因优化改变输出词形。
- 英文规范化：UTF-8 下已经是小写 ASCII 的 token 直接使用原文切片，其他输入继续走原小写转换。下游分词器仍把 token 复制到拥有其生命周期的分配器。

未修改官方数据生成器、SQL、计时脚本、评分脚本或评分基线，也未改变 TOKENIZE/MATCH 的输出约定。

## 正确性验证

2026-11-16 在 Linux/WSL、本地 Release 构建、专用 SQL 端口 2884 上实测。
机器内存约 8 GB，测试实例配置 `memory_budget=2G`、`cpu_count=2`。
构建使用 `cmake --build build_release --target seekdb -j2`，成功。

`test_fts_equivalence.py` 使用固定随机种子，覆盖：

- 113 段空串、中英文、大小写、全角、组合字符、emoji、长串、控制字符和随机混合文本，分别测试 ik/beng/space/ngram，共 452 例。
- latin1/gbk/utf16/utf16le 与 beng/space 的 8 例字符集转换或原有不支持错误。
- 四组 MATCH 的全部 ID 与相关度，既比较词频、文档长度，也比较完整查询结果。

在同一份索引上切换原版/优化版，双向比较 **460 个分词用例和四组完整 MATCH 结果完全一致**。
早期快照出现过相关度微小变化；没有放宽比较精度，重新捕获并双向切换后才确认一致。
比较期间不要重建索引：采样统计变化可能影响相关度。

```bash
python3 -m pip install pymysql
# 原版运行时捕获；该脚本只读，要求先有完整官方基准数据和索引。
python3 unittest/sql/engine/expr/test_fts_equivalence.py \
  --mode capture --snapshot /tmp/fts-before.json --port 2884
# 在同一数据目录切换为优化版并启动后：
python3 unittest/sql/engine/expr/test_fts_equivalence.py \
  --mode compare --snapshot /tmp/fts-before.json --port 2884
```

`test_fts_char_classification.cpp` 是独立的字符分类差分程序。使用服务器的头文件、编译选项和链接依赖，
**取消 `-DNDEBUG` 并添加 `-UNDEBUG`**，以启用断言。实测遍历全部 Unicode 标量值、畸形 UTF-8，
并检查本构建不注册 UTF-16 排序规则时的原有不支持行为，全部通过。
可选 `--benchmark` 仅作局部诊断：三个样本原版/优化版为
39.644/34.840、39.996/37.197、39.620/32.418 ms，校验和均为 7680000。
该局部结果不代表整题提速。

未经改动的原版 mysqltest 用例实测通过：
`tokenize_function`、`cn_word`、`parser_properties`、`simple_query`、`create_fts_index_afterward`。
`basic_dml` 在原版程序上已失败：用例重建表后沿用旧 `$data_table_id`，
`query_get_value` 得到 `No such row`，后续 SQL 报错；未修改用例或预期结果绕过。
因此这里不宣称全量 FTS 回归通过。

前三题回归也已实际通过：赛题一原有 8 项 AI 测试、赛题二原有 36 条文件 SQL 检查、
赛题三的 `ai_column_ddl`、`ai_column_t1_dml`、`ai_column_worker`、
`ai_column_retry_strict`、`ai_column_vector_index` 五组原版用例。

## 官方完整基准：保留全部四轮

使用仓库原始 `tools/benchmark/fts_large_bench.sh`，参数为
`ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 SAMPLES=3 WARMUP=30 SKIP_LOAD=0`。
顺序为原版、优化版、原版、优化版，每轮重新加载数据和建立索引，期间没有编译或并行回归。
下面取每种程序两轮的指标算术平均；正数表示耗时下降，负数表示变慢。

| 指标 | 原版 | 优化版 | 耗时下降 |
| --- | ---: | ---: | ---: |
| build_ik_all_sec | 12.5720 | 11.0985 | 11.72% |
| build_ik_content_sec | 9.7930 | 9.5360 | 2.62% |
| build_beng_en_sec | 5.0645 | 4.1575 | 17.91% |
| tokenize_ik_avg_ms | 0.4915 | 0.5857 | -19.17% |
| tokenize_beng_avg_ms | 0.3934 | 0.4486 | -14.02% |
| query_cn_avg_ms | 2.8952 | 2.6071 | 9.95% |
| query_beng_avg_ms | 3.5671 | 3.9965 | -12.03% |
| query_mixed_avg_ms | 3.1772 | 3.1618 | 0.48% |
| query_limit_avg_ms | 3.5994 | 4.0770 | -13.27% |

四轮命中数均为 8001/11000/7332/20。
三类指标平均耗时下降分别为 10.75%、-16.59%、-3.72%，等权综合为 **-3.19%**。
因此不能声称这四轮官方基准证明整体加速，更不能声称已经达到 50% 或得到平台满分。
SQL 循环与样本之间存在明显波动，不能将本机与 CI 的硬件差异算作补丁收益。

复现单轮示例（仅使用可丢弃的测试实例；脚本会重建 `fts_large_bench` 库）：

```bash
MYSQL='mysql -h127.0.0.1 -P2884 -uroot -N -s -A --default-character-set=utf8mb4' \
MYSQL_VERBOSE='mysql -h127.0.0.1 -P2884 -uroot -A --default-character-set=utf8mb4' \
LABEL=before ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 \
SAMPLES=3 WARMUP=30 SKIP_LOAD=0 OUTPUT=/tmp/fts-before-report.txt \
bash tools/benchmark/fts_large_bench.sh
```

较早的探索测量也保存在本地证据目录；其中一次原版测量与测试程序链接重叠，
一次优化版测量处于编译后内存压力状态，未用它们作为受控性能结论。

## 补充整表分词诊断

这项测量不属于官方评分，也不替代上面的完整基准。
固定同一份 2 万行数据，每种程序运行两轮，每轮一次预热和五次计时。
SQL 消费所有文档的完整分词 JSON，不通过跳过分词来降低耗时：

```sql
SELECT SUM(OCTET_LENGTH(TOKENIZE(LEFT(content,10000),'ik','[{"output":"all"}]')))
FROM fts_large_bench.docs;
SELECT SUM(OCTET_LENGTH(TOKENIZE(LEFT(content_en,10000),'beng','[{"output":"all"}]')))
FROM fts_large_bench.docs;
```

`LEFT` 用于物化 TEXT 定位器，实测最大字符长度为 195/141，小于 10000，无截断。
原版直接向 TOKENIZE 传 TEXT 定位器时可能报错或返回空结果，因此没有把这种调用用于测量。

| 轮次 | 程序 | IK 中位数（秒） | beng 中位数（秒） |
| --- | --- | ---: | ---: |
| 1 | 原版 | 1.219782 | 0.472657 |
| 2 | 优化版 | 1.575441 | 0.535798 |
| 3 | 原版 | 1.939665 | 0.586973 |
| 4 | 优化版 | 1.359727 | 0.448095 |

对两轮中位数取平均，IK 为 1.579724 → 1.467584 秒（下降 7.10%），
beng 为 0.529815 → 0.491947 秒（下降 7.15%）。
输出字节总和分别固定为 10872917 和 5327800。
轮次波动仍明显，该结果只提供补充证据，不保证官方测量会得到同等收益。

本机原始报告、构建日志和对照快照保存在 `/home/xy/seekdb-ai-work/fts/`。
提交时需使用同一分支上包含四题的新提交 SHA；平台核验结果以实际提测为准。
