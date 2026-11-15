/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

#include "sql/ai_column/ai_column_definition.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_client.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"

namespace oceanbase { namespace sql { namespace ai_column {

// Keep the accepted AI-function implementation intact. AI columns supply a
// separate system instruction through the provider's existing public API.
class Model {
public:
  static int complete(common::ObArenaAllocator &allocator, const Definition &definition,
      common::ObString input, common::ObJsonObject *config, common::ObString &output)
  {
    using namespace common;
    int ret = OB_SUCCESS;
    ObAIFuncExprInfo *info = nullptr;
    share::ObAiModelEndpointInfo endpoint;
    auto *resolver = share::server_service<query::ObIAiEndpointResolver>();
    ObString model_name(definition.model.size(), definition.model.data());
    if (resolver == nullptr) return OB_NOT_INIT;
    if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(allocator, model_name, info))) return ret;
    if (OB_FAIL(resolver->resolve_by_model_name(model_name, allocator, endpoint))) return ret;
    if (!ObAIFuncUtils::is_completion_type(info)) return OB_INVALID_ARGUMENT;
    ObAIFuncIComplete *provider = nullptr;
    ObArray<ObString> headers;
    ObJsonObject *body = nullptr, *response = nullptr;
    ObIJsonBase *result = nullptr;
    ObString access_key;
    ObString request_model = endpoint.get_request_model_name().empty() ? info->model_ : endpoint.get_request_model_name();
    ObString system_prompt(definition.system_prompt.size(), definition.system_prompt.data());
    ObAIFuncClient client;
    if (OB_FAIL(ObAIFuncUtils::get_complete_provider(allocator, endpoint.get_provider(), provider))) {
    } else if (OB_FAIL(endpoint.get_unencrypted_access_key(allocator, access_key))) {
    } else if (OB_FAIL(provider->get_header(allocator, access_key, headers))) {
    } else if (OB_FAIL(provider->get_body(allocator, request_model, system_prompt, input, config, body))) {
    } else if (OB_FAIL(client.send_post(allocator, endpoint.get_url(), headers, body, response))) {
    } else if (OB_FAIL(provider->parse_output(allocator, response, result))) {
    } else if (result == nullptr || result->json_type() != ObJsonNodeType::J_STRING) {
      ret = OB_INVALID_DATA;
    } else {
      output = ObAIFuncJsonUtils::string_value(result);
    }
    return ret;
  }
};
}}}
