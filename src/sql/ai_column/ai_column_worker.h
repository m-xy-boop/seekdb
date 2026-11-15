/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

#include "sql/ai_column/ai_column_model.h"
#include "sql/engine/expr/ob_expr_uuid.h"
#include "lib/thread/thread_pool.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "common/mysqlclient/ob_mysql_transaction.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "share/rc/ob_server_runtime.h"
#include "query/vector/ob_vector_index_util.h"
#include <cmath>

namespace oceanbase { namespace sql { namespace ai_column {

class Worker : public lib::ThreadPool {
  struct Task {
    std::string id, request, predicate, input_sql, lease;
    int64_t table_id=0, column_id=0, version=0, retries=0, maximum=0;
  };
public:
  Worker() : lib::ThreadPool(4), proxy_(nullptr) {}
  int start_with(common::ObMySQLProxy &proxy)
  {
    proxy_=&proxy;
    return start();
  }
  void run(int64_t index) override
  {
    UNUSED(index);
    while (!has_set_stop()) {
      Task task;
      int ret=common::OB_SUCCESS;
      try {
        ret=claim(task);
        if (ret==common::OB_SUCCESS) execute(task);
        else if (ret==common::OB_ITER_END) recover_publication();
        else LOG_ERROR("AI column task claim failed", K(ret));
      } catch (const std::exception &error) {
        ret=common::OB_ERR_UNEXPECTED;
        LOG_ERROR("AI column worker exception", K(ret), "reason", error.what());
      }
      // A bounded pause also permits startup before schema bootstrap finishes.
      if (ret!=common::OB_SUCCESS) usleep(100000);
    }
  }
private:
  static int string(common::sqlclient::ObMySQLResult &result, int64_t index, std::string &value)
  {
    common::ObString text;
    const int ret=result.get_varchar(index,text);
    if (ret==common::OB_SUCCESS) value.assign(text.ptr(),text.length());
    return ret;
  }
  static int write(common::ObISQLClient &client,const std::string &sql,int64_t *count=nullptr)
  {
    int64_t affected=0;
    int ret=client.write(sql.c_str(),affected);
    if (count) *count=affected;
    return ret;
  }
  static int scalar(common::ObISQLClient &client,const std::string &sql,std::string &value,bool &is_null)
  {
    common::ObMySQLProxy::MySQLResult result;
    int ret=client.read(result,sql.c_str());
    if (ret!=common::OB_SUCCESS) return ret;
    auto *row=result.get_result();
    if (row==nullptr) return common::OB_ERR_UNEXPECTED;
    if (OB_FAIL(row->next())) return ret;
    ret=string(*row,0,value);
    is_null=ret==common::OB_ERR_NULL_VALUE;
    return is_null ? common::OB_SUCCESS : ret;
  }
  int claim(Task &task)
  {
    using namespace common;
    ObMySQLTransaction transaction;
    int ret=transaction.start(proxy_);
    if (ret!=OB_SUCCESS) return ret;
    {
      ObMySQLProxy::MySQLResult result;
      const char *query="SELECT task_id,table_id,column_id,request_id,definition_version,"
          "retry_count,max_retries,CAST(rowkey_predicate AS CHAR),CAST(input_expr AS CHAR) "
          "FROM oceanbase.__all_ai_column_task WHERE "
          "((state IN (0,3) AND eligible_ts<=now(6)) OR (state=1 AND lease_until<now(6))) "
          "ORDER BY eligible_ts,task_id LIMIT 1 FOR UPDATE SKIP LOCKED";
      if (OB_FAIL(transaction.read(result,query))) {
      } else if (result.get_result()==nullptr) ret=OB_ERR_UNEXPECTED;
      else {
        auto &row=*result.get_result();
        if (OB_FAIL(row.next())) {
        } else if (OB_FAIL(string(row,0,task.id))) {
        } else if (OB_FAIL(row.get_int(1,task.table_id))) {
        } else if (OB_FAIL(row.get_int(2,task.column_id))) {
        } else if (OB_FAIL(string(row,3,task.request))) {
        } else if (OB_FAIL(row.get_int(4,task.version))) {
        } else if (OB_FAIL(row.get_int(5,task.retries))) {
        } else if (OB_FAIL(row.get_int(6,task.maximum))) {
        } else if (OB_FAIL(string(row,7,task.predicate))) {
        } else ret=string(row,8,task.input_sql);
      }
    }
    char owner[UuidCommon::LENGTH_UUID];
    if (ret==OB_SUCCESS) ret=ObExprUuid::gen_server_uuid(owner,sizeof(owner));
    if (ret==OB_SUCCESS) task.lease.assign(owner,sizeof(owner));
    if (ret==OB_SUCCESS) ret=write(transaction,"UPDATE oceanbase.__all_ai_column_task SET state=1,"
        "lease_until=DATE_ADD(now(6),INTERVAL 120 SECOND),lease_owner="+Definition::literal(task.lease)
        +" WHERE task_id="+Definition::literal(task.id));
    const int end=transaction.end(ret==OB_SUCCESS);
    return ret==OB_SUCCESS ? end : ret;
  }
  void recover_publication()
  {
    // A crash can occur after staging the final result but before publication.
    // Recover those committed terminal groups even when there is no runnable
    // HTTP task left to drive the normal publication path.
    Task task;
    common::ObMySQLProxy::MySQLResult result;
    const char *query="SELECT a.table_id,a.request_id,CAST(a.rowkey_predicate AS CHAR) "
        "FROM oceanbase.__all_ai_column_task a WHERE a.state IN (2,4) "
        "AND NOT EXISTS(SELECT 1 FROM oceanbase.__all_ai_column_task b "
        "WHERE b.table_id=a.table_id AND b.request_id=a.request_id AND b.state NOT IN(2,4)) "
        "ORDER BY a.eligible_ts LIMIT 1";
    int ret = proxy_->read(result,query);
    if (ret != common::OB_SUCCESS || result.get_result()==nullptr) {
      LOG_ERROR("AI column terminal recovery query failed", K(ret));
      return;
    }
    auto &row=*result.get_result();
    if ((ret=row.next()) != common::OB_SUCCESS) {
      if (ret != common::OB_ITER_END) LOG_ERROR("AI column terminal recovery fetch failed", K(ret));
      return;
    }
    if ((ret=row.get_int(int64_t{0},task.table_id)) != common::OB_SUCCESS
        || (ret=string(row,1,task.request)) != common::OB_SUCCESS
        || (ret=string(row,2,task.predicate)) != common::OB_SUCCESS) {
      LOG_ERROR("AI column terminal recovery decode failed", K(ret));
      return;
    }
    result.reset();
    publish(task);
  }
  int schema(const Task &task,share::schema::ObSchemaGetterGuard &guard,
      const share::schema::ObTableSchema *&table,const share::schema::ObColumnSchemaV2 *&column,Definition &definition)
  {
    int ret=common::OB_SUCCESS;
    if (GCTX.schema_service_==nullptr) return common::OB_NOT_INIT;
    if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(guard))) {
    } else if (OB_FAIL(guard.get_table_schema(task.table_id,table))) {
    } else if (table==nullptr || (column=table->get_column_schema(task.column_id))==nullptr
        || !column->is_ai_column() || column->get_schema_version()!=task.version) {
      ret=common::OB_SCHEMA_ERROR;
    } else {
      const auto &spec=column->get_ai_column_spec();
      definition=Definition::decode(std::string(spec.ptr(),spec.length()));
    }
    return ret;
  }
  int model(const Task &task,const Definition &definition,
      const share::schema::ObColumnSchemaV2 &column,std::string &result)
  {
    using namespace common;
    std::string input;
    bool is_null=false;
    int ret=scalar(*proxy_,task.input_sql,input,is_null);
    if (ret!=OB_SUCCESS) return ret;
    if (is_null) { result="NULL"; return OB_SUCCESS; }
    if (definition.function=="ai_embed") {
      int64_t dimension=0;
      if (OB_FAIL(share::ObVectorIndexUtil::get_vector_dim_from_extend_type_info(column.get_extended_type_info(),dimension))) return ret;
      std::string vector;
      ret=scalar(*proxy_,"SELECT AI_EMBED("+Definition::literal(definition.model)+","+Definition::literal(input)+","+std::to_string(dimension)+")",vector,is_null);
      if (ret==OB_SUCCESS) result=is_null ? "NULL" : Definition::literal(vector);
      return ret;
    }
    ObArenaAllocator allocator("AiColModel");
    ObString input_value(input.size(),input.data()),output;
    // AI_PROMPT returns a structured prompt; retain the accepted formatter.
    if (definition.input_expression.rfind("ai_prompt(",0)==0) {
      ObJsonObject *prompt=nullptr;
      if (OB_FAIL(ObAIFuncJsonUtils::get_json_object_form_str(allocator,input_value,prompt))) return ret;
      if (!ObAIFuncPromptObjectUtils::is_valid_prompt_object(prompt)) return OB_INVALID_ARGUMENT;
      if (OB_FAIL(ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator,prompt,input_value))) return ret;
    }
    ObJsonObject *config=nullptr;
    if (!definition.config_expression.empty() && definition.config_expression!="NULL") {
      std::string config_text;
      if (OB_FAIL(scalar(*proxy_,"SELECT "+definition.config_expression,config_text,is_null))) return ret;
      if (!is_null) {
        ObString config_value(config_text.size(),config_text.data());
        if (OB_FAIL(ObAIFuncJsonUtils::get_json_object_form_str(allocator,config_value,config))) return ret;
      }
    }
    if (OB_FAIL(Model::complete(allocator,definition,input_value,config,output))) return ret;
    const std::string text(output.ptr(),output.length());
    if (column.is_json()) {
      rapidjson::Document json;json.Parse(text.data(),text.size());
      if (json.HasParseError()) return OB_INVALID_DATA;
    } else if (ob_is_numeric_type(column.get_data_type())) {
      rapidjson::Document number;number.Parse(text.data(),text.size());
      if (number.HasParseError() || !number.IsNumber()) return OB_INVALID_DATA;
      if (ob_is_integer_type(column.get_data_type()) && !number.IsInt64() && !number.IsUint64()) return OB_INVALID_DATA;
    }
    result=Definition::literal(text);
    return OB_SUCCESS;
  }
  void execute(const Task &task)
  {
    share::schema::ObSchemaGetterGuard guard;
    const share::schema::ObTableSchema *table=nullptr;
    const share::schema::ObColumnSchemaV2 *column=nullptr;
    Definition definition;
    int ret=schema(task,guard,table,column,definition);
    if (ret!=common::OB_SUCCESS) {
      write(*proxy_,"DELETE FROM oceanbase.__all_ai_column_task WHERE task_id="+Definition::literal(task.id));
      return;
    }
    std::string result;
    ret=model(task,definition,*column,result);
    std::string update="UPDATE oceanbase.__all_ai_column_task SET lease_until=NULL,";
    if (ret!=common::OB_SUCCESS && task.retries<task.maximum) {
      update+="state=3,retry_count=retry_count+1,eligible_ts=DATE_ADD(now(6),INTERVAL 2 SECOND),last_error="
          +Definition::literal(std::to_string(ret));
    } else {
      update+="state="+std::string(ret==common::OB_SUCCESS ? "2" : "4")+",staged_result="
          +(ret==common::OB_SUCCESS ? Definition::literal(result) : "NULL")+",last_error="
          +(ret==common::OB_SUCCESS ? "NULL" : Definition::literal(std::to_string(ret)));
    }
    update+=" WHERE task_id="+Definition::literal(task.id)+" AND state=1 AND request_id="+Definition::literal(task.request)
        +" AND lease_owner="+Definition::literal(task.lease);
    if (write(*proxy_,update)!=common::OB_SUCCESS) return;
    publish(task);
  }
  void publish(const Task &task)
  {
    using namespace common;
    ObMySQLTransaction transaction;
    if (transaction.start(proxy_)!=OB_SUCCESS) return;
    const std::string group="table_id="+std::to_string(task.table_id)+" AND request_id="+Definition::literal(task.request);
    struct Result { int64_t id,version,state; std::string value; };
    std::vector<Result> columns;
    int ret=OB_SUCCESS;
    {
      ObMySQLProxy::MySQLResult result;
      const std::string query="SELECT column_id,definition_version,state,COALESCE(CAST(staged_result AS CHAR),'NULL') FROM oceanbase.__all_ai_column_task WHERE "+group+" ORDER BY column_id FOR UPDATE";
      if (OB_FAIL(transaction.read(result,query.c_str()))) {
      } else if (result.get_result()==nullptr) ret=OB_ERR_UNEXPECTED;
      else {
        auto &row=*result.get_result();
        while (ret==OB_SUCCESS && (ret=row.next())==OB_SUCCESS) {
          Result value{};
          if (OB_FAIL(row.get_int(int64_t{0},value.id))) {
          } else if (OB_FAIL(row.get_int(1,value.version))) {
          } else if (OB_FAIL(row.get_int(2,value.state))) {
          } else if (OB_FAIL(string(row,3,value.value))) {
          } else columns.push_back(value);
        }
        if (ret==OB_ITER_END) ret=OB_SUCCESS;
      }
    }
    if (ret!=OB_SUCCESS || columns.empty() || std::any_of(columns.begin(),columns.end(),[](const Result &v){return v.state!=2 && v.state!=4;})) {
      if (ret != OB_SUCCESS) LOG_ERROR("AI column staged group read failed", K(ret), K(task.table_id));
      transaction.end(false);return;
    }
    share::schema::ObSchemaGetterGuard guard;
    const share::schema::ObTableSchema *table=nullptr;
    const share::schema::ObDatabaseSchema *database=nullptr;
    std::string assignments,conditions;
    if (GCTX.schema_service_==nullptr) ret=OB_NOT_INIT;
    else if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(guard))) {
    } else if (OB_FAIL(guard.get_table_schema(task.table_id,table))) {
    } else if (table==nullptr) ret=OB_SCHEMA_ERROR;
    else if (OB_FAIL(guard.get_database_schema(table->get_database_id(),database))) {
    } else if (database==nullptr) ret=OB_SCHEMA_ERROR;
    for (const auto &value : columns) {
      if (ret!=OB_SUCCESS) break;
      const auto *column=table->get_column_schema(value.id);
      if (column==nullptr || !column->is_ai_column() || column->get_schema_version()!=value.version) {ret=OB_SCHEMA_ERROR;break;}
      const auto &spec=column->get_ai_column_spec();
      const auto definition=Definition::decode(std::string(spec.ptr(),spec.length()));
      const auto *state=table->get_column_schema(definition.state_column_id);
      const auto *request=table->get_column_schema(definition.request_column_id);
      if (state==nullptr || request==nullptr) {ret=OB_SCHEMA_ERROR;break;}
      auto name=[](const share::schema::ObColumnSchemaV2 &c){return Definition::identifier(std::string(c.get_column_name_str().ptr(),c.get_column_name_str().length()));};
      if (!assignments.empty()) assignments+=",";
      assignments+=name(*column)+"="+(value.state==2 ? value.value : "NULL")+","+name(*state)+"="+(value.state==2 ? "1" : "2");
      conditions+=" AND "+name(*request)+"="+Definition::literal(task.request);
    }
    if (ret==OB_SUCCESS) {
      const std::string target=Definition::identifier(std::string(database->get_database_name_str().ptr(),database->get_database_name_str().length()))+"."
          +Definition::identifier(std::string(table->get_table_name_str().ptr(),table->get_table_name_str().length()));
      ret=write(transaction,"UPDATE "+target+" SET "+assignments+" WHERE "+task.predicate+conditions);
    }
    if (ret==OB_SUCCESS || ret==OB_SCHEMA_ERROR) {
      ret=write(transaction,"DELETE FROM oceanbase.__all_ai_column_task WHERE "+group);
    }
    const int end_ret = transaction.end(ret==OB_SUCCESS);
    if (ret != OB_SUCCESS || end_ret != OB_SUCCESS) {
      LOG_ERROR("AI column publication failed", K(ret), K(end_ret), K(task.table_id), K(task.column_id));
    }
  }
  common::ObMySQLProxy *proxy_;
};
}}}
