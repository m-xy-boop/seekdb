/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

#include "sql/ai_column/ai_column_definition.h"
#include "sql/engine/dml/ob_table_modify_op.h"
#include "sql/engine/expr/ob_expr_uuid.h"
#include "sql/ob_sql_context.h"
#include "share/schema/ob_schema_getter_guard.h"

namespace oceanbase { namespace sql { namespace ai_column {
using common::ObDatum;

// DML queue writes reuse the external session's transaction. No independent
// SQL proxy transaction is allowed here: statement errors and ROLLBACK must
// undo both the business row and its pending work.
class Dml {
public:
  static int process(ObTableModifyOp &op, const ObDMLBaseCtDef &ct, bool deleting, bool updating = false)
  {
    int ret = common::OB_SUCCESS;
    if (!ct.is_primary_index_) return ret;
    // Inner-table DML never owns AI columns. Its nested execution context may
    // deliberately omit the user statement's schema guard.
    if (common::is_inner_table(ct.das_base_ctdef_.index_tid_)) return ret;
    auto *session = op.get_exec_ctx().get_my_session();
    auto *ctx = op.get_exec_ctx().get_sql_ctx();
    const share::schema::ObTableSchema *table = nullptr;
    if (session == nullptr || ctx == nullptr || ctx->schema_guard_ == nullptr) return common::OB_ERR_UNEXPECTED;
    // Background publication uses an internal session. SQL issued by a user
    // through PL still has USER_SESSION type and follows the normal path.
    if (!session->is_user_session()) return ret;
    if (OB_FAIL(ctx->schema_guard_->get_table_schema(ct.das_base_ctdef_.index_tid_, table))) return ret;
    if (table == nullptr) return common::OB_SCHEMA_ERROR;
    bool has_ai = false;
    for (int64_t i = 0; i < table->get_column_count(); ++i)
      has_ai |= table->get_column_schema_by_idx(i)->is_ai_column();
    if (!has_ai) return ret;
    try {
      std::string old_key, new_key;
      if (deleting || updating) {
        if (OB_FAIL(rowkey(op, ct, *table, ct.old_row_, old_key))) return ret;
      }
      if (!deleting && OB_FAIL(rowkey(op, ct, *table, ct.new_row_, new_key))) return ret;
      char uuid[UuidCommon::LENGTH_UUID];
      if (!deleting && OB_FAIL(ObExprUuid::gen_server_uuid(uuid, sizeof(uuid)))) return ret;
      const std::string request = deleting ? std::string() : std::string(uuid, sizeof(uuid));
      std::vector<std::string> statements;
      std::vector<std::pair<uint64_t, Definition>> invalidations;
      for (int64_t i = 0; OB_SUCC(ret) && i < table->get_column_count(); ++i) {
        const auto *column = table->get_column_schema_by_idx(i);
        if (!column->is_ai_column()) continue;
        const auto &spec = column->get_ai_column_spec();
        const auto definition = Definition::decode(std::string(spec.ptr(), spec.length()));
        bool changed = !updating || old_key != new_key;
        // RETRY rewrites to a native UPDATE of this column's request token.
        // Normal synthesized self-assignments leave the token unchanged.
        if (!changed && updating) {
          const int64_t index = column_index(ct, definition.request_column_id);
          if (index < 0) return common::OB_ERR_UNEXPECTED;
          ObDatum *old_request = nullptr, *new_request = nullptr;
          if (OB_FAIL(ct.old_row_.at(index)->eval(op.get_eval_ctx(), old_request))) return ret;
          if (OB_FAIL(ct.new_row_.at(index)->eval(op.get_eval_ctx(), new_request))) return ret;
          changed = !ObDatum::binary_equal(*old_request, *new_request);
        }
        for (const auto &name : definition.dependencies) {
          if (changed) break;
          const auto *dependency = table->get_column_schema(common::ObString(name.size(), name.data()));
          if (dependency == nullptr) return common::OB_SCHEMA_ERROR;
          const int64_t index = column_index(ct, dependency->get_column_id());
          if (index < 0) return common::OB_ERR_UNEXPECTED;
          ObDatum *old_value = nullptr, *new_value = nullptr;
          if (OB_FAIL(ct.old_row_.at(index)->eval(op.get_eval_ctx(), old_value))) return ret;
          if (OB_FAIL(ct.new_row_.at(index)->eval(op.get_eval_ctx(), new_value))) return ret;
          changed = !ObDatum::binary_equal(*old_value, *new_value);
        }
        if (!changed) continue;
        const std::string owner = "table_id=" + std::to_string(table->get_table_id())
            + " AND column_id=" + std::to_string(column->get_column_id());
        statements.push_back("DELETE FROM oceanbase.__all_ai_column_task WHERE " + owner
            + " AND rowkey_predicate=" + Definition::literal(deleting || updating ? old_key : new_key));
        if (deleting) continue;
        invalidations.emplace_back(column->get_column_id(), definition);
        std::string input_query;
        if (OB_FAIL(input_sql(op, ct, *table, definition, input_query))) return ret;
        statements.push_back("INSERT INTO oceanbase.__all_ai_column_task "
            "(task_id,table_id,rowkey_predicate,column_id,request_id,definition_version,state,retry_count,max_retries,eligible_ts,input_expr) VALUES("
            + Definition::literal(request + "-" + std::to_string(column->get_column_id())) + ","
            + std::to_string(table->get_table_id()) + "," + Definition::literal(new_key) + ","
            + std::to_string(column->get_column_id()) + "," + Definition::literal(request) + ","
            + std::to_string(column->get_schema_version()) + ",0,0," + std::to_string(definition.max_retries)
            + ",now(6)," + Definition::literal(input_query) + ")");
      }
      if (!statements.empty()) {
        if (OB_FAIL(op.open_inner_conn())) return ret;
        if (OB_FAIL(op.begin_nested_session(false))) return ret;
        auto &das = op.get_exec_ctx().get_das_ctx();
        const bool saved_queue_write = das.is_ai_column_queue_write_;
        das.is_ai_column_queue_write_ = true;
        int64_t statement_index = 0;
        for (const auto &statement : statements) {
          if (OB_FAIL(op.execute_write(statement.c_str()))) {
            LOG_ERROR("AI column transactional queue write failed", K(ret), K(statement_index));
            break;
          }
          ++statement_index;
        }
        das.is_ai_column_queue_write_ = saved_queue_write;
        const int end_ret = op.end_nested_session();
        if (ret == common::OB_SUCCESS) ret = end_ret;
      }
      // Finish nested queue SQL before setting the final business row image.
      for (const auto &entry : invalidations) {
        if (OB_FAIL(ret)) break;
        if (OB_FAIL(invalidate(op, ct, entry.first, entry.second, request))) break;
      }
    } catch (const std::exception &) {
      ret = common::OB_INVALID_ARGUMENT;
    }
    return ret;
  }

private:
  static int64_t column_index(const ObDMLBaseCtDef &ct, uint64_t id)
  {
    for (int64_t i = 0; i < ct.column_ids_.count(); ++i) if (ct.column_ids_.at(i) == id) return i;
    return -1;
  }
  static int value_sql(ObTableModifyOp &op, ObExpr &expr, std::string &out)
  {
    int ret = common::OB_SUCCESS;
    ObDatum *datum = nullptr;
    common::ObObj value;
    common::ObArenaAllocator allocator("AiColLiteral");
    char *buffer = nullptr;
    int64_t length = 0, position = 0;
    if (OB_FAIL(expr.eval(op.get_eval_ctx(), datum))) {
    } else if (OB_FAIL(datum->to_obj(value, expr.obj_meta_))) {
    } else if (OB_FAIL(value.print_sql_literal(buffer, length, position, allocator, op.get_obj_print_params()))) {
    } else {
      out.assign(buffer, position);
    }
    return ret;
  }
  static int rowkey(ObTableModifyOp &op, const ObDMLBaseCtDef &ct,
      const share::schema::ObTableSchema &table, const ExprFixedArray &row, std::string &out)
  {
    int ret = common::OB_SUCCESS;
    for (int64_t i = 0; OB_SUCC(ret) && i < table.get_rowkey_info().get_size(); ++i) {
      const uint64_t id = table.get_rowkey_info().get_column(i)->column_id_;
      const auto *column = table.get_column_schema(id);
      const int64_t index = column_index(ct, id);
      if (column == nullptr || index < 0 || index >= row.count()) return common::OB_ERR_UNEXPECTED;
      std::string value;
      if (OB_FAIL(value_sql(op, *row.at(index), value))) break;
      if (i) out += " AND ";
      out += Definition::identifier(std::string(column->get_column_name_str().ptr(), column->get_column_name_str().length())) + " = " + value;
    }
    return out.empty() && ret == common::OB_SUCCESS ? common::OB_ERR_UNEXPECTED : ret;
  }
  static int invalidate(ObTableModifyOp &op, const ObDMLBaseCtDef &ct, uint64_t id,
      const Definition &definition, const std::string &request)
  {
    const uint64_t ids[] = {id, definition.state_column_id, definition.request_column_id, definition.result_spec_column_id};
    for (int i = 0; i < 4; ++i) {
      const int64_t index = column_index(ct, ids[i]);
      if (index < 0 || index >= ct.new_row_.count()) return common::OB_ERR_UNEXPECTED;
      auto &expr = *ct.new_row_.at(index);
      auto &datum = expr.locate_datum_for_write(op.get_eval_ctx());
      if (i == 1) datum.set_int(0);
      else if (i == 2) {
        char *buffer = expr.get_str_res_mem(op.get_eval_ctx(), request.size());
        if (buffer == nullptr) return common::OB_ALLOCATE_MEMORY_FAILED;
        MEMCPY(buffer, request.data(), request.size());
        datum.set_string(buffer, request.size());
      } else datum.set_null();
      expr.get_eval_info(op.get_eval_ctx()).evaluated_ = true;
      // Generated vector-index values may have been evaluated during row
      // checks. Recompute those parents from the final AI row image.
      op.clear_dml_evaluated_flag(expr.parent_cnt_, expr.parents_);
    }
    return common::OB_SUCCESS;
  }
  static int input_sql(ObTableModifyOp &op, const ObDMLBaseCtDef &ct,
      const share::schema::ObTableSchema &table, const Definition &definition, std::string &out)
  {
    int ret = common::OB_SUCCESS;
    out = "SELECT " + definition.input_expression;
    if (!definition.dependencies.empty()) {
      out += " FROM (SELECT ";
      for (size_t i = 0; i < definition.dependencies.size(); ++i) {
        const auto &name = definition.dependencies[i];
        const auto *column = table.get_column_schema(common::ObString(name.size(), name.data()));
        if (column == nullptr) return common::OB_SCHEMA_ERROR;
        const int64_t index = column_index(ct, column->get_column_id());
        if (index < 0 || index >= ct.new_row_.count()) return common::OB_ERR_UNEXPECTED;
        std::string value;
        if (OB_FAIL(value_sql(op, *ct.new_row_.at(index), value))) return ret;
        if (i) out += ",";
        out += value + " AS " + Definition::identifier(name);
      }
      out += ") AS ai_column_input";
    }
    return ret;
  }
};
}}}
