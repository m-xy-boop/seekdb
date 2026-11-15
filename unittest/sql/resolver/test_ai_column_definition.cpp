/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "sql/ai_column/ai_column_definition.h"
#include <cassert>
#include <iostream>
using namespace oceanbase::sql::ai_column;

static ParseNode leaf(ObItemType type, const char *value = nullptr)
{
  ParseNode n{}; n.type_ = type; n.str_value_ = value; n.str_len_ = value ? std::strlen(value) : 0; return n;
}
int main()
{
  assert(retry_predicate("WHERE status='FAILED' AND `status`='active'", "state_expr")
      == "WHERE state_expr='FAILED' AND `status`='active'");
  assert(retry_predicate("WHERE STATUS='READY' /* status */ AND x='status'", "state_expr")
      == "WHERE state_expr='READY' /* status */ AND x='status'");
  assert(retry_predicate("WHERE status='it\\'s status' # status\n AND id=2", "state_expr")
      == "WHERE state_expr='it\\'s status' # status\n AND id=2");
  assert(retry_predicate("WHERE status_code='FAILED'", "state_expr") == "WHERE status_code='FAILED'");
  auto fn = leaf(T_IDENT, "AI_COMPLETE");
  auto model = leaf(T_VARCHAR, "some-user-model");
  auto name = leaf(T_IDENT, "content");
  ParseNode *ref_children[] = {nullptr, nullptr, &name};
  auto ref = leaf(T_COLUMN_REF); ref.children_ = ref_children; ref.num_child_ = 3;
  ParseNode *args_children[] = {&model, &ref};
  auto args = leaf(T_EXPR_LIST); args.children_ = args_children; args.num_child_ = 2;
  ParseNode *expr_children[] = {&fn, &args};
  auto expr = leaf(T_FUN_SYS, "AI_COMPLETE('some-user-model', content)"); expr.children_ = expr_children; expr.num_child_ = 2;
  ParseNode *spec_children[] = {&expr, nullptr};
  auto spec = leaf(T_AI_COLUMN_DEFINITION); spec.children_ = spec_children; spec.num_child_ = 2;
  auto d = Definition::parse(spec);
  assert(d.dependencies == std::vector<std::string>{"content"});
  assert(d.model == "some-user-model");
  d.state_column_id = 20; d.request_column_id = 21; d.result_spec_column_id = 22;
  d.normalize(expr, "Return exactly one valid JSON value.", 0);
  assert(d.expression.find("Return exactly one valid JSON value.") != std::string::npos);
  assert(d.expression.find("`content`") != std::string::npos);
  assert(Definition::identifier("a`b") == "`a``b`");
  assert(Definition::literal("'\\") == "CONVERT(0x275c USING utf8mb4)");
  auto copy = Definition::decode(d.encode());
  assert(copy.input_expression == "`content`");
  assert(copy.system_prompt == "Return exactly one valid JSON value.");
  assert(copy.expression.find("CONCAT") == std::string::npos);
  assert(copy.original_expression == "AI_COMPLETE('some-user-model', content)");
  assert(copy.state_column_id == 20 && copy.request_column_id == 21 && copy.result_spec_column_id == 22);
  assert(copy.expression == d.expression && copy.dependencies == d.dependencies && copy.max_retries == 2);
  auto rejects = [](auto action) { bool rejected = false; try { action(); } catch (const DefinitionError &) { rejected = true; } assert(rejected); };
  auto prefix_value = leaf(T_VARCHAR, "Classify: ");
  ParseNode *literal_children[] = {nullptr, &prefix_value};
  auto prefix = leaf(T_VARCHAR, "Classify: "); prefix.children_ = literal_children; prefix.num_child_ = 2;
  auto concat_name = leaf(T_IDENT, "concat");
  ParseNode *concat_args_children[] = {&prefix, &ref};
  auto concat_args = leaf(T_EXPR_LIST); concat_args.children_ = concat_args_children; concat_args.num_child_ = 2;
  ParseNode *concat_children[] = {&concat_name, &concat_args};
  auto concat = leaf(T_FUN_SYS); concat.children_ = concat_children; concat.num_child_ = 2;
  args_children[1] = &concat;
  auto with_prefix = Definition::parse(spec);
  with_prefix.normalize(expr, "Return exactly one valid JSON value.", 0);
  assert(with_prefix.dependencies == std::vector<std::string>{"content"});
  assert(with_prefix.input_expression == "concat(CONVERT(0x436c6173736966793a20 USING utf8mb4),`content`)");
  args_children[1] = &ref;
  fn.str_value_ = "RAND"; fn.str_len_ = 4;
  rejects([&] { Definition::parse(spec); });
  fn.str_value_ = "AI_COMPLETE"; fn.str_len_ = 11;
  auto option_key = leaf(T_IDENT, "sync_mode"); auto option_value = leaf(T_VARCHAR, "sync");
  ParseNode *opt_children[] = {&option_key, &option_value};
  auto option = leaf(T_OP_EQ); option.children_ = opt_children; option.num_child_ = 2;
  ParseNode *opts_children[] = {&option};
  auto opts = leaf(T_EXPR_LIST); opts.children_ = opts_children; opts.num_child_ = 1; spec_children[1] = &opts;
  rejects([&] { Definition::parse(spec); });
  option_value.str_value_ = "async"; option_value.str_len_ = 5;
  assert(Definition::parse(spec).max_retries == 2);
  ParseNode *duplicates[] = {&option, &option}; opts.children_ = duplicates; opts.num_child_ = 2;
  rejects([&] { Definition::parse(spec); });
  rejects([] { Definition::decode("{}"); });
  rejects([] { Definition::decode("{invalid"); });
  ref_children[1] = &name; spec_children[1] = nullptr;
  rejects([&] { Definition::parse(spec); });
  std::cout << "AI column definition roundtrip and invalid-definition checks passed\n";
}
