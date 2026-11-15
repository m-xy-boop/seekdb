/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

#include "sql/ai_column/ai_column_definition.h"
#include "sql/engine/ob_physical_plan.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/session/ob_inner_sql_connection.h"
#include "query/session/ob_inner_sql_connection_access.h"

namespace oceanbase { namespace sql { namespace ai_column {

// Check on every execution, including a cached plan, before opening the user
// plan, after its transaction/snapshot starts. The unfiltered state scan
// deliberately ignores the user's WHERE clause.
inline int check_strict_read(ObExecContext &ctx, const ObPhysicalPlan &plan)
{
  int ret = common::OB_SUCCESS;
  auto *session = ctx.get_my_session();
  if (!session || !session->is_user_session() || plan.ai_column_reads_.empty()) return ret;
  int64_t enabled = 0;
  if (OB_FAIL(session->get_sys_variable_by_name(common::ObString("ai_column_strict_read"), enabled))) return ret;
  if (!enabled) return ret;
  const auto *sql_ctx = ctx.get_sql_ctx();
  if (!sql_ctx || !sql_ctx->schema_guard_) return common::OB_ERR_UNEXPECTED;
  common::sqlclient::ObISQLConnectionGuard connection;
  bool owned = false;
  if (!session->get_inner_conn()) {
    if (OB_FAIL(query::ObInnerSQLConnectionAccess::create_connection_with_external_session(session, connection))) return ret;
    session->set_inner_conn(connection.get_ptr());
    owned = true;
  }
  auto *inner = as_inner_sql_connection(session->get_inner_conn());
  if (!inner) ret = common::OB_ERR_UNEXPECTED;
  else {
    ObSQLSessionInfo::StmtSavedValue saved_session;
    ObIInnerSQLConnection::SavedValue saved_connection;
    if (OB_SUCC(ret) && OB_FAIL(inner->begin_nested_session(saved_session, saved_connection, false))) {
    } else if (OB_SUCC(ret)) {
      auto &das = ctx.get_das_ctx();
      const bool saved_marker = das.is_ai_column_queue_write_;
      das.is_ai_column_queue_write_ = true;
      uint64_t failed_table = 0, failed_column = 0;
      for (int64_t i = 0; OB_SUCC(ret) && i + 1 < plan.ai_column_reads_.count(); i += 2) {
        const uint64_t table_id = plan.ai_column_reads_.at(i), column_id = plan.ai_column_reads_.at(i+1);
        const share::schema::ObTableSchema *table = nullptr;
        const share::schema::ObDatabaseSchema *database = nullptr;
        if (OB_FAIL(sql_ctx->schema_guard_->get_table_schema(table_id, table))) break;
        if (!table) { ret = common::OB_SCHEMA_ERROR; break; }
        const auto *column = table->get_column_schema(column_id);
        if (!column || !column->is_ai_column()) continue;
        try {
          const auto &spec = column->get_ai_column_spec();
          const auto definition = Definition::decode(std::string(spec.ptr(), spec.length()));
          const auto *state = table->get_column_schema(definition.state_column_id);
          if (!state) { ret = common::OB_SCHEMA_ERROR; break; }
          if (OB_FAIL(sql_ctx->schema_guard_->get_database_schema(table->get_database_id(), database))) break;
          if (!database) { ret = common::OB_SCHEMA_ERROR; break; }
          const auto quote = [](const common::ObString &name) { return Definition::identifier(std::string(name.ptr(),name.length())); };
          const std::string query = "SELECT 1 FROM " + quote(database->get_database_name_str()) + "." + quote(table->get_table_name_str())
              + " WHERE " + quote(state->get_column_name_str()) + " IS NULL OR " + quote(state->get_column_name_str()) + " <> 1 LIMIT 1";
          common::ObMySQLProxy::MySQLResult result;
          if (OB_FAIL(inner->execute_read(query.c_str(), result))) break;
          const int next = result.get_result() ? result.get_result()->next() : common::OB_ERR_UNEXPECTED;
          if (next == common::OB_SUCCESS) {
            failed_table = table_id; failed_column = column_id;
            ret = OB_AI_COLUMN_UNFINISHED;
          } else if (next != common::OB_ITER_END) ret = next;
          if (result.get_result()) result.get_result()->close();
        } catch (const std::exception &) { ret = common::OB_SCHEMA_ERROR; }
      }
      das.is_ai_column_queue_write_ = saved_marker;
      const int end_ret = inner->end_nested_session(saved_session, saved_connection);
      if (ret == common::OB_SUCCESS) ret = end_ret;
      if (ret == OB_AI_COLUMN_UNFINISHED) LOG_USER_ERROR(OB_AI_COLUMN_UNFINISHED, failed_table, failed_column);
    }
  }
  if (owned) session->set_inner_conn(nullptr);
  return ret;
}
}}}
