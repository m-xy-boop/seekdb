/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

#include <algorithm>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include "sql/parser/parse_node.h"
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace oceanbase { namespace sql { namespace ai_column {
struct DefinitionError : std::runtime_error { using std::runtime_error::runtime_error; };
inline std::string text(const ParseNode *n)
{
  return n && n->str_value_ ? std::string(n->str_value_, n->str_len_) : std::string();
}
inline std::string lower(std::string s)
{
  for (char &c : s) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  return s;
}
// Preserve SQL quoting: unquoted status is the RETRY pseudo-column, whereas
// `status` remains a business column and string/comment contents are untouched.
inline std::string retry_predicate(const std::string &sql, const std::string &state)
{
  std::string out;
  for (size_t i = 0; i < sql.size();) {
    const char c = sql[i];
    if (c == '\'' || c == '"' || c == '`') {
      const char quote = c;
      out += sql[i++];
      while (i < sql.size()) {
        const char next = sql[i++]; out += next;
        if (next == '\\' && i < sql.size()) out += sql[i++];
        else if (next == quote) {
          if (i < sql.size() && sql[i] == quote) out += sql[i++];
          else break;
        }
      }
    } else if ((c == '/' && i + 1 < sql.size() && sql[i+1] == '*')
        || c == '#' || (c == '-' && i + 2 < sql.size() && sql[i+1] == '-' && sql[i+2] <= ' ')) {
      const bool block = c == '/';
      size_t end = block ? sql.find("*/", i + 2) : sql.find('\n', i);
      if (end == std::string::npos) end = sql.size(); else if (block) end += 2;
      out.append(sql, i, end - i); i = end;
    } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
      const size_t start = i++;
      while (i < sql.size() && ((sql[i] >= 'a' && sql[i] <= 'z') || (sql[i] >= 'A' && sql[i] <= 'Z')
          || (sql[i] >= '0' && sql[i] <= '9') || sql[i] == '_' || sql[i] == '$')) ++i;
      const auto word = sql.substr(start, i - start);
      out += lower(word) == "status" ? state : word;
    } else out += sql[i++];
  }
  return out;
}
struct Definition {
  std::string expression, original_expression, input_expression, config_expression, system_prompt, model, function;
  std::vector<std::string> dependencies;
  int max_retries = 2;
  int explicit_dimension = 0;
  uint64_t state_column_id = 0, request_column_id = 0, result_spec_column_id = 0;

  static void collect(const ParseNode *node, std::vector<std::string> &deps, unsigned depth = 0)
  {
    if (!node || depth > 64) throw DefinitionError("invalid or excessively nested AI column input");
    // The parser represents string literals with charset/value children. Those
    // children describe the literal; they are not executable input expressions.
    if (node->type_ == T_VARCHAR || node->type_ == T_CHAR) return;
    if (node->type_ == T_COLUMN_REF) {
      if (node->num_child_ != 3 || node->children_[0] || node->children_[1] || !node->children_[2])
        throw DefinitionError("AI column dependencies must be unqualified ordinary columns");
      std::string name = text(node->children_[2]);
      if (name.empty()) throw DefinitionError("empty AI column dependency");
      if (std::none_of(deps.begin(), deps.end(), [&](const std::string &d) { return lower(d) == lower(name); }))
        deps.push_back(name);
      return;
    }
    // Built-in special syntax such as CONCAT may have a dedicated parser tag;
    // only explicitly handled deterministic forms are admitted.
    if (node->type_ == T_FUN_SYS) {
      static const std::set<std::string> allowed = {"concat", "concat_ws", "coalesce", "ifnull", "lower", "upper", "trim", "replace", "substr", "substring", "ai_prompt", "json_object", "json_array"};
      if (node->num_child_ != 2 || !allowed.count(lower(text(node->children_[0]))))
        throw DefinitionError("unsupported or nondeterministic AI column input function");
      collect(node->children_[1], deps, depth + 1);
      return;
    }
    if (node->num_child_ == 0) {
      switch (node->type_) {
        case T_NULL: case T_BOOL: case T_INT: case T_UINT64: case T_NUMBER:
        case T_VARCHAR: case T_CHAR: case T_DOUBLE: case T_FLOAT: return;
        default: throw DefinitionError("unsupported AI column input literal");
      }
    }
    switch (node->type_) {
      case T_EXPR_LIST: case T_OP_ADD: case T_OP_MINUS: case T_OP_MUL:
      case T_OP_DIV: case T_OP_NEG: case T_OP_POS: break;
      default: throw DefinitionError("unsupported AI column input expression");
    }
    for (int i = 0; i < node->num_child_; ++i) collect(node->children_[i], deps, depth + 1);
  }

  static Definition parse(const ParseNode &spec)
  {
    if (spec.type_ != T_AI_COLUMN_DEFINITION || spec.num_child_ != 2 || !spec.children_[0])
      throw DefinitionError("invalid AI column definition");
    Definition d;
    const ParseNode *expr = spec.children_[0];
    if (expr->type_ != T_FUN_SYS || expr->num_child_ != 2 || !expr->children_[1])
      throw DefinitionError("AI column requires AI_COMPLETE or AI_EMBED");
    d.function = lower(text(expr->children_[0]));
    if (d.function != "ai_complete" && d.function != "ai_embed")
      throw DefinitionError("AI column requires AI_COMPLETE or AI_EMBED");
    const ParseNode *args = expr->children_[1];
    if (args->num_child_ < 2 || args->num_child_ > 3 || !args->children_[0] || args->children_[0]->type_ != T_VARCHAR)
      throw DefinitionError("AI column requires a constant model name and an input expression");
    d.model = text(args->children_[0]);
    if (d.model.empty() || d.model.size() > 128) throw DefinitionError("invalid AI column model name");
    collect(args->children_[1], d.dependencies);
    if (args->num_child_ == 3) {
      const ParseNode *arg = args->children_[2];
      if (d.function == "ai_embed") {
        if (!arg || arg->type_ != T_INT || arg->value_ <= 0 || arg->value_ > 65535)
          throw DefinitionError("AI_EMBED dimension must be a positive constant");
        d.explicit_dimension = arg->value_;
      } else if (!arg || (arg->type_ != T_NULL && arg->type_ != T_VARCHAR)) {
        throw DefinitionError("AI_COMPLETE configuration must be a constant string or NULL");
      }
    }
    d.expression = text(expr);
    d.original_expression = d.expression;
    if (d.expression.empty() || d.expression.size() > 65536) throw DefinitionError("AI column expression size is invalid");
    std::set<std::string> seen;
    const ParseNode *opts = spec.children_[1];
    for (int i = 0; opts && i < opts->num_child_; ++i) {
      const ParseNode *opt = opts->children_[i];
      if (!opt || opt->num_child_ != 2 || !opt->children_[0] || !opt->children_[1]) throw DefinitionError("invalid AI column option");
      const std::string key = lower(text(opt->children_[0]));
      const ParseNode *value = opt->children_[1];
      if (!seen.insert(key).second) throw DefinitionError("duplicate AI column option");
      if (key == "sync_mode" && value->type_ == T_VARCHAR && lower(text(value)) == "async") {
      } else if (key == "max_retries" && value->type_ == T_INT && value->value_ >= 0 && value->value_ <= 100) {
        d.max_retries = value->value_;
      } else throw DefinitionError("unsupported AI column option or value");
    }
    return d;
  }

  static std::string identifier(const std::string &name)
  {
    std::string out = "`";
    for (char c : name) { if (c == '`') out += '`'; out += c; }
    return out + "`";
  }
  static std::string literal(const std::string &value)
  {
    if (value.empty()) return "''";
    const char *digits = "0123456789abcdef";
    std::string out = "CONVERT(0x";
    for (unsigned char c : value) { out += digits[c >> 4]; out += digits[c & 15]; }
    return out + " USING utf8mb4)";
  }
  // Render the validated subset without copying untrusted SQL fragments. String
  // constants use hex so their value is independent of NO_BACKSLASH_ESCAPES.
  static std::string render(const ParseNode *n, unsigned depth = 0)
  {
    if (!n || depth > 64) throw DefinitionError("invalid AI column expression tree");
    if (n->type_ == T_COLUMN_REF) return identifier(text(n->children_[2]));
    if (n->type_ == T_VARCHAR || n->type_ == T_CHAR) return literal(text(n));
    if (n->type_ == T_NULL) return "NULL";
    if (n->type_ == T_BOOL) return n->value_ ? "TRUE" : "FALSE";
    if (n->type_ == T_INT) return std::to_string(n->value_);
    if (n->type_ == T_UINT64) return std::to_string(static_cast<uint64_t>(n->value_));
    if (n->num_child_ == 0) return text(n);
    if (n->type_ == T_FUN_SYS) return lower(text(n->children_[0])) + "(" + render(n->children_[1], depth + 1) + ")";
    if (n->type_ == T_EXPR_LIST) {
      std::string out;
      for (int i = 0; i < n->num_child_; ++i) { if (i) out += ','; out += render(n->children_[i], depth + 1); }
      return out;
    }
    if (n->type_ == T_OP_NEG || n->type_ == T_OP_POS)
      return std::string(n->type_ == T_OP_NEG ? "-(" : "+(") + render(n->children_[0], depth + 1) + ")";
    const char *op = n->type_ == T_OP_ADD ? "+" : n->type_ == T_OP_MINUS ? "-" : n->type_ == T_OP_MUL ? "*" : "/";
    return "(" + render(n->children_[0], depth + 1) + op + render(n->children_[1], depth + 1) + ")";
  }
  void normalize(const ParseNode &expr, const std::string &type_prompt, int target_dimension)
  {
    const ParseNode *args = expr.children_[1];
    input_expression = render(args->children_[1]);
    system_prompt = type_prompt;
    config_expression = args->num_child_ == 3 && function == "ai_complete" ? render(args->children_[2]) : "NULL";
    const std::string &input = input_expression;
    expression = function + "(" + literal(model) + "," + input;
    if (function == "ai_embed") expression += "," + std::to_string(target_dimension);
    else if (args->num_child_ == 3) expression += "," + render(args->children_[2]);
    expression += ")";
    // Persist the type contract alongside the normalized expression. The worker
    // must send it as a system message, preserving the user input verbatim.
    if (!type_prompt.empty()) expression += " /* " + type_prompt + " */";
  }

  std::string encode() const
  {
    rapidjson::StringBuffer b; rapidjson::Writer<rapidjson::StringBuffer> w(b);
    w.StartObject(); w.Key("version"); w.Int(1);
    w.Key("expression"); w.String(expression.data(), expression.size());
    w.Key("input_expression"); w.String(input_expression.data(), input_expression.size());
    w.Key("config_expression"); w.String(config_expression.data(), config_expression.size());
    w.Key("system_prompt"); w.String(system_prompt.data(), system_prompt.size());
    w.Key("original_expression"); w.String(original_expression.data(), original_expression.size());
    w.Key("model"); w.String(model.data(), model.size());
    w.Key("function"); w.String(function.c_str());
    w.Key("max_retries"); w.Int(max_retries);
    w.Key("dimension"); w.Int(explicit_dimension);
    w.Key("state_column_id"); w.Uint64(state_column_id);
    w.Key("request_column_id"); w.Uint64(request_column_id);
    w.Key("result_spec_column_id"); w.Uint64(result_spec_column_id);
    w.Key("dependencies"); w.StartArray();
    for (const auto &d : dependencies) w.String(d.data(), d.size());
    w.EndArray(); w.EndObject(); return std::string(b.GetString(), b.GetSize());
  }
  static Definition decode(const std::string &s)
  {
    rapidjson::Document j; j.Parse(s.data(), s.size());
    if (j.HasParseError() || !j.IsObject() || !j.HasMember("version") || !j["version"].IsInt() || j["version"].GetInt() != 1 ||
        !j.HasMember("expression") || !j["expression"].IsString() || !j.HasMember("model") || !j["model"].IsString() ||
        !j.HasMember("function") || !j["function"].IsString() || !j.HasMember("max_retries") || !j["max_retries"].IsInt() ||
        !j.HasMember("dimension") || !j["dimension"].IsInt() || !j.HasMember("dependencies") || !j["dependencies"].IsArray())
      throw DefinitionError("invalid stored AI column definition");
    Definition d; d.expression.assign(j["expression"].GetString(), j["expression"].GetStringLength());
    d.original_expression = d.expression;
    if (j.HasMember("original_expression")) {
      if (!j["original_expression"].IsString()) throw DefinitionError("invalid stored source AI expression");
      d.original_expression.assign(j["original_expression"].GetString(), j["original_expression"].GetStringLength());
    }
    auto optional_string = [&](const char *key) -> std::string {
      if (!j.HasMember(key)) return {};
      if (!j[key].IsString()) throw DefinitionError("invalid stored AI column text field");
      return std::string(j[key].GetString(), j[key].GetStringLength());
    };
    d.input_expression = optional_string("input_expression");
    d.config_expression = optional_string("config_expression");
    d.system_prompt = optional_string("system_prompt");
    d.model.assign(j["model"].GetString(), j["model"].GetStringLength()); d.function = j["function"].GetString();
    d.max_retries = j["max_retries"].GetInt(); d.explicit_dimension = j["dimension"].GetInt();
    for (const auto &v : j["dependencies"].GetArray()) {
      if (!v.IsString()) throw DefinitionError("invalid stored AI column dependency");
      d.dependencies.emplace_back(v.GetString(), v.GetStringLength());
    }
    auto column_id = [&](const char *key) -> uint64_t {
      if (!j.HasMember(key)) return 0;
      if (!j[key].IsUint64()) throw DefinitionError("invalid hidden AI column id");
      return j[key].GetUint64();
    };
    d.state_column_id = column_id("state_column_id");
    d.request_column_id = column_id("request_column_id");
    d.result_spec_column_id = column_id("result_spec_column_id");
    if (d.max_retries < 0 || d.max_retries > 100 || d.expression.empty() || d.model.empty() ||
        (d.function != "ai_complete" && d.function != "ai_embed")) throw DefinitionError("invalid stored AI column configuration");
    return d;
  }
};
}}}
