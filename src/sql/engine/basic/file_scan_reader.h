/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <glob.h>
#include <dirent.h>
#include <fnmatch.h>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace oceanbase { namespace sql { namespace file_sql {
// Bounded per-record storage. Schema inference scans the entire file but never
// retains data rows. Execution reopens the same file version and scans again.
constexpr size_t MAX_RECORD = 8 * 1024 * 1024;
constexpr size_t MAX_COLUMNS = 1024;
constexpr size_t MAX_FILES = 10000;
enum class Type { Null, Boolean, Bigint, Double, Varchar, Date, Datetime };
struct Error : std::runtime_error { using std::runtime_error::runtime_error; };
struct Cell { std::string text; bool null = true; bool quoted = false; };
struct Column { std::string name; Type type = Type::Null; bool nullable = false; int scale = 0; };
struct Options {
  std::string path, format = "csv", nullstr = "", secure_root;
  bool header = true;
  char delimiter = ',', quote = '"', escape = '"';
};
struct Signature {
  uint64_t device = 0, inode = 0, size = 0;
  int64_t mtime = 0, mtime_ns = 0, ctime = 0, ctime_ns = 0;
  explicit Signature(const struct stat &s) : device(s.st_dev), inode(s.st_ino), size(s.st_size),
    mtime(s.st_mtim.tv_sec), mtime_ns(s.st_mtim.tv_nsec),
    ctime(s.st_ctim.tv_sec), ctime_ns(s.st_ctim.tv_nsec) {}
  Signature() = default;
  bool operator==(const Signature &s) const {
    return device == s.device && inode == s.inode && size == s.size &&
      mtime == s.mtime && mtime_ns == s.mtime_ns && ctime == s.ctime && ctime_ns == s.ctime_ns;
  }
};
inline std::string canonical(const std::string &p) {
  if (p.empty() || p.size() >= 4096 || p.find('\0') != std::string::npos) throw Error("invalid local path");
  char buf[4096];
  if (!::realpath(p.c_str(), buf)) throw Error("cannot resolve local file path");
  return buf;
}
inline void check_path(const std::string &root, const std::string &p) {
  if (root.empty() || root == "NULL" || root == "null") throw Error("secure_file_priv denies local file access");
  const std::string r = canonical(root);
  struct stat st{};
  if (::stat(r.c_str(), &st) || !S_ISDIR(st.st_mode)) throw Error("secure_file_priv must name a directory");
  if (r != "/" && (p.size() <= r.size() || p.compare(0, r.size(), r) || p[r.size()] != '/'))
    throw Error("path is outside secure_file_priv");
}
inline bool valid_utf8(const std::string &s) {
  size_t i = 0;
  while (i < s.size()) {
    const auto c = static_cast<unsigned char>(s[i++]);
    if (c < 0x80) continue;
    unsigned n = c >= 0xc2 && c <= 0xdf ? 1 : c >= 0xe0 && c <= 0xef ? 2 : c >= 0xf0 && c <= 0xf4 ? 3 : 99;
    if (n == 99 || i + n > s.size()) return false;
    unsigned v = c & ((1 << (6 - n)) - 1);
    for (unsigned j = 0; j < n; ++j) {
      const auto t = static_cast<unsigned char>(s[i++]);
      if ((t & 0xc0) != 0x80) return false;
      v = (v << 6) | (t & 63);
    }
    if ((n == 1 && v < 0x80) || (n == 2 && v < 0x800) || (n == 3 && v < 0x10000) ||
        v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff)) return false;
  }
  return true;
}
inline bool integer(const std::string &s, int64_t &v) {
  if (s.empty()) return false;
  const char *begin = s.data() + (s[0] == '+' ? 1 : 0);
  auto res = std::from_chars(begin, s.data() + s.size(), v);
  return res.ec == std::errc() && res.ptr == s.data() + s.size();
}
inline bool real(const std::string &s, double &v) {
  if (s.empty()) return false;
  const char *begin = s.data() + (s[0] == '+' ? 1 : 0);
  auto res = std::from_chars(begin, s.data() + s.size(), v);
  return res.ec == std::errc() && res.ptr == s.data() + s.size() && std::isfinite(v);
}
inline bool date(const std::string &s, bool datetime) {
  const size_t minlen = datetime ? 19 : 10;
  if (s.size() < minlen || (!datetime && s.size() != 10)) return false;
  if (s[4] != '-' || s[7] != '-' || (datetime && ((s[10] != ' ' && s[10] != 'T') || s[13] != ':' || s[16] != ':'))) return false;
  for (size_t i = 0; i < minlen; ++i) {
    if (i == 4 || i == 7 || (datetime && (i == 10 || i == 13 || i == 16))) continue;
    if (s[i] < '0' || s[i] > '9') return false;
  }
  auto num = [&](size_t pos, size_t len) { return std::stoi(s.substr(pos, len)); };
  const int y=num(0,4), m=num(5,2), d=num(8,2);
  const int days[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
  if (y < 1 || m < 1 || m > 12 || d < 1 || d > days[m] + (m == 2 && y%4 == 0 && (y%100 != 0 || y%400 == 0))) return false;
  if (datetime && (num(11,2) > 23 || num(14,2) > 59 || num(17,2) > 59)) return false;
  if (datetime && s.size() > 19) {
    if (s[19] != '.' || s.size() == 20 || s.size() > 26) return false;
    for (size_t i=20; i<s.size(); ++i) if (s[i]<'0' || s[i]>'9') return false;
  }
  return true;
}
inline Type classify(const Cell &v) {
  if (v.null) return Type::Null;
  if (v.text == "true" || v.text == "false" || v.text == "TRUE" || v.text == "FALSE") return Type::Boolean;
  int64_t i; double d;
  if (integer(v.text, i)) return Type::Bigint;
  // Out-of-range integral literals stay textual to avoid silently rounding them.
  if (v.text.find_first_of(".eE") != std::string::npos && real(v.text, d)) return Type::Double;
  if (date(v.text, false)) return Type::Date;
  if (date(v.text, true)) return Type::Datetime;
  return Type::Varchar;
}
inline Type widen(Type a, Type b) {
  if (a == Type::Null) return b;
  if (b == Type::Null || a == b) return a;
  if ((a == Type::Bigint && b == Type::Double) || (a == Type::Double && b == Type::Bigint)) return Type::Double;
  if ((a == Type::Date && b == Type::Datetime) || (a == Type::Datetime && b == Type::Date)) return Type::Datetime;
  return Type::Varchar;
}
inline std::string folded(std::string name) {
  std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c){return c>='A' && c<='Z'?c+('a'-'A'):c;});return name;
}
inline const char *type_name(Type t) {
  switch(t) {
    case Type::Boolean: return "BOOLEAN"; case Type::Bigint: return "BIGINT";
    case Type::Double: return "DOUBLE"; case Type::Date: return "DATE";
    case Type::Datetime: return "DATETIME"; case Type::Null: return "NULL";
    default: return "VARCHAR";
  }
}

class Reader {
public:
  explicit Reader(const Options &options) : options_(options) {}
  Reader(const Reader &) = delete;
  Reader &operator=(const Reader &) = delete;
  ~Reader() { close(); }
  void close() { if (file_) { std::fclose(file_); file_ = nullptr; } }
  void open(const Signature *expected = nullptr) {
    close(); line_ = 1; row_ = 0;
    if(!options_.delimiter || !options_.quote || !options_.escape || options_.delimiter==options_.quote ||
       options_.delimiter=='\n' || options_.delimiter=='\r' || options_.quote=='\n' || options_.quote=='\r')throw Error("invalid CSV dialect");
    path_ = canonical(options_.path); check_path(options_.secure_root, path_);
    int fd = ::open(path_.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) throw Error("cannot open local file read-only");
    struct stat st{};
    if (::fstat(fd, &st) || !S_ISREG(st.st_mode)) { ::close(fd); throw Error("input must be a regular file"); }
    // Verify the opened inode and resolved name, closing the canonicalize/open race.
    char link[64], target[4096];
    std::snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    ssize_t n = ::readlink(link, target, sizeof(target)-1);
    if (n < 0) { ::close(fd); throw Error("cannot verify opened file path"); }
    target[n] = 0;
    if (path_ != target) { ::close(fd); throw Error("file path changed while opening"); }
    signature_ = Signature(st);
    if (expected && !(signature_ == *expected)) { ::close(fd); throw Error("file changed after schema inference"); }
    file_ = ::fdopen(fd, "rb");
    if (!file_) { ::close(fd); throw Error("cannot create local file stream"); }
    // UTF-8 BOM is accepted only at the beginning of a text file.
    const int a=std::fgetc(file_), b=std::fgetc(file_), c=std::fgetc(file_);
    if (!(a==0xef && b==0xbb && c==0xbf)) std::rewind(file_);
  }
  void verify() const {
    struct stat st{}, named{};
    if (!file_ || ::fstat(::fileno(file_), &st) || ::stat(path_.c_str(), &named) ||
        !(Signature(st) == signature_) || !(Signature(named) == signature_)) throw Error("input file changed during scan");
  }
  std::string read_at(uint64_t offset, size_t size) {
    if(size>16*1024*1024 || offset>signature_.size || size>signature_.size-offset) fail("invalid bounded file range");
    std::string out(size,'\0');
    size_t done=0;
    while(done<size) {ssize_t n=::pread(::fileno(file_),out.data()+done,size-done,offset+done);if(n<=0) fail("short file read");done+=n;}
    verify();return out;
  }
  const Signature &signature() const { return signature_; }
  size_t line() const { return line_; }
  bool csv(std::vector<Cell> &cells) {
    cells.clear(); Cell field; field.null = false;
    bool quoted = false, after_quote = false, started = false;
    size_t bytes = 0;
    auto push = [&]() {
      if (!valid_utf8(field.text) || field.text.find('\0') != std::string::npos) fail("invalid UTF-8 or NUL in CSV field");
      field.null = !field.quoted && field.text == options_.nullstr;
      cells.push_back(std::move(field)); field = Cell{}; field.null = false;
      if (cells.size() > MAX_COLUMNS) fail("too many columns");
    };
    while (true) {
      int c = std::fgetc(file_);
      if (c == EOF) {
        if (std::ferror(file_)) fail("read error");
        if (quoted) fail("unterminated quoted field");
        if (!started && cells.empty() && field.text.empty()) { verify(); return false; }
        push(); ++row_; verify(); return true;
      }
      started = true;
      if (++bytes > MAX_RECORD) fail("record exceeds size limit");
      if (c == '\n') ++line_;
      if (quoted) {
        if (c == options_.escape && options_.escape != options_.quote) {
          int next = std::fgetc(file_);
          if (next == EOF) fail("unfinished escape");
          if (++bytes > MAX_RECORD) fail("record exceeds size limit");
          if (next == '\n') ++line_;
          field.text.push_back(static_cast<char>(next));
        } else if (c == options_.quote) {
          int next = std::fgetc(file_);
          if (next == options_.quote && options_.escape == options_.quote) { field.text.push_back(static_cast<char>(c)); if(++bytes>MAX_RECORD)fail("record exceeds size limit"); }
          else { if (next != EOF) std::ungetc(next, file_); quoted = false; after_quote = true; }
        } else field.text.push_back(static_cast<char>(c));
      } else if (c == options_.delimiter) {
        push(); after_quote = false;
      } else if (c == '\n' || c == '\r') {
        if (c == '\r') { int next=std::fgetc(file_); if (next=='\n') ++line_; else { ++line_; if(next!=EOF) std::ungetc(next,file_); } }
        push(); ++row_; verify(); return true;
      } else if (after_quote) fail("unexpected character after closing quote");
      else if (c == options_.quote) {
        if (!field.text.empty()) fail("quote inside unquoted field");
        quoted = true; field.quoted = true;
      } else field.text.push_back(static_cast<char>(c));
    }
  }
  bool json(std::vector<std::string> &names, std::vector<Cell> &cells) {
    std::string line;
    while (true) {
      line.clear(); int c;
      while ((c=std::fgetc(file_)) != EOF && c!='\n') {
        if (line.size() >= MAX_RECORD) fail("JSONL record exceeds size limit");
        line.push_back(static_cast<char>(c));
      }
      if (std::ferror(file_)) fail("read error");
      if (c=='\n') ++line_;
      if (line.find_first_not_of(" \r\t") != std::string::npos) break;
      if (c==EOF) { verify(); return false; }
    }
    if (!valid_utf8(line) || line.find('\0')!=std::string::npos) fail("invalid UTF-8 or NUL in JSONL");
    // Reject excessive nesting before the recursive JSON parser runs.
    size_t depth=0;bool in_string=false,escaped=false;
    for(char ch:line){
      if(in_string){if(escaped)escaped=false;else if(ch=='\\')escaped=true;else if(ch=='"')in_string=false;}
      else if(ch=='"')in_string=true;
      else if(ch=='{' || ch=='['){if(++depth>16)fail("JSONL nesting limit");}
      else if((ch=='}' || ch==']') && depth)--depth;
    }
    rapidjson::Document doc;
    doc.Parse<rapidjson::kParseFullPrecisionFlag | rapidjson::kParseValidateEncodingFlag>(line.data(), line.size());
    if (doc.HasParseError() || !doc.IsObject()) fail("JSONL requires one flat object per line");
    names.clear(); cells.clear();
    for (auto it=doc.MemberBegin(); it!=doc.MemberEnd(); ++it) {
      if (names.size()>=MAX_COLUMNS) fail("too many JSONL fields");
      std::string name(it->name.GetString(), it->name.GetStringLength());
      if(name.empty() || name.size()>256 || name.find('\0')!=std::string::npos ||
          std::find(names.begin(),names.end(),name)!=names.end()) fail("invalid or duplicate JSONL field name");
      names.push_back(std::move(name)); Cell cell; auto &v=it->value;
      if (!v.IsNull()) {
        cell.null=false;
        if (v.IsString()) { cell.text.assign(v.GetString(),v.GetStringLength()); cell.quoted=true; }
        else if (v.IsObject() || v.IsArray()) fail("nested JSONL values are unsupported");
        else { rapidjson::StringBuffer out; rapidjson::Writer<rapidjson::StringBuffer> writer(out); v.Accept(writer); cell.text=out.GetString(); }
      }
      cells.push_back(std::move(cell));
    }
    ++row_; verify(); return true;
  }
  [[noreturn]] void fail(const char *message) const {
    throw Error(std::string(message)+" (line "+std::to_string(line_)+", record "+std::to_string(row_+1)+")");
  }
private:
  Options options_; std::string path_; FILE *file_=nullptr;
  Signature signature_; size_t line_=1, row_=0;
};
struct Schema { std::vector<Column> columns; Signature signature; };
#include "file_scan_parquet.h"
inline std::vector<std::string> discover(const Options &options) {
  if(options.path.find('\0')!=std::string::npos || options.path.size()>=4096) throw Error("invalid glob path");
  // Check the non-pattern directory before enumerating any entries.
  const size_t wildcard=options.path.find_first_of("*?[");
  const size_t slash=options.path.rfind('/',wildcard==std::string::npos?options.path.size():wildcard);
  std::string prefix=slash==std::string::npos?".":options.path.substr(0,slash+1);
  const std::string root=canonical(options.secure_root);
  const std::string directory=canonical(prefix);
  check_path(options.secure_root,directory==root?directory+"/.":directory);
  DIR *stream=opendir(directory.c_str());if(!stream)throw Error("cannot enumerate glob directory");
  struct Guard {DIR *p;~Guard(){closedir(p);}} guard{stream};
  const std::string pattern=slash==std::string::npos?options.path:options.path.substr(slash+1);
  std::vector<std::string> files;size_t entries=0;
  while(auto *entry=readdir(stream)) {
    if(++entries>MAX_FILES)throw Error("glob directory entry limit exceeded");
    if(entry->d_name[0]=='.' && (!entry->d_name[1] || (entry->d_name[1]=='.' && !entry->d_name[2])))continue;
    if(fnmatch(pattern.c_str(),entry->d_name,FNM_PERIOD)!=0)continue;
    const std::string path=canonical(directory+"/"+entry->d_name);check_path(options.secure_root,path);
    struct stat st{};
    if(::stat(path.c_str(),&st)) throw Error("glob entry changed");
    if(S_ISREG(st.st_mode)) files.push_back(path);
  }
  std::sort(files.begin(),files.end());return files;
}

inline Schema infer(const Options &options) {
  if(options.format=="parquet") return Parquet(options).schema();
  if(options.format=="glob") {discover(options);Schema s;s.columns.push_back({"file",Type::Varchar,false});return s;}
  Reader reader(options); reader.open(); Schema schema; schema.signature=reader.signature();
  std::vector<Cell> row; std::vector<std::string> names;
  size_t rows=0;
  if (options.format=="csv") {
    if (reader.csv(row)) {
      std::set<std::string> used;
      for (size_t i=0;i<row.size();++i) {
        std::string name=options.header ? row[i].text : "column"+std::to_string(i);
        if (name.empty()) name="column"+std::to_string(i);
        if(name.size()>256) reader.fail("column name exceeds limit");
        std::string base=name;
        for (size_t suffix=2;used.count(folded(name));++suffix) name=base+"__"+std::to_string(suffix);
        used.insert(folded(name)); schema.columns.push_back({name,Type::Null,false});
      }
      if (!options.header) {
        for(size_t i=0;i<row.size();++i) { schema.columns[i].type=classify(row[i]); schema.columns[i].nullable=row[i].null;if(classify(row[i])==Type::Datetime && row[i].text.size()>19)schema.columns[i].scale=row[i].text.size()-20; }
        ++rows;
      }
    }
    while(reader.csv(row)) {
      if(row.size()!=schema.columns.size()) reader.fail("CSV field count mismatch");
      for(size_t i=0;i<row.size();++i) {
        auto &col=schema.columns[i]; col.type=widen(col.type,classify(row[i])); col.nullable |= row[i].null;
        if(classify(row[i])==Type::Datetime && row[i].text.size()>19)col.scale=std::max(col.scale,static_cast<int>(row[i].text.size()-20));
      }
      ++rows;
    }
  } else if(options.format=="jsonl") {
    while(reader.json(names,row)) {
      for(auto &c:schema.columns) if(std::find(names.begin(),names.end(),c.name)==names.end()) c.nullable=true;
      for(size_t i=0;i<names.size();++i) {
        auto it=std::find_if(schema.columns.begin(),schema.columns.end(),[&](const Column &c){return c.name==names[i];});
        Type type=classify(row[i]);
        if(row[i].quoted && type!=Type::Date && type!=Type::Datetime) type=Type::Varchar;
        if(it==schema.columns.end()) {
          if(schema.columns.size()>=MAX_COLUMNS) reader.fail("too many union schema fields");
          for(const auto &c:schema.columns)if(folded(c.name)==folded(names[i]))reader.fail("case-insensitive JSONL column name collision");
          schema.columns.push_back({names[i],type,rows>0 || row[i].null,type==Type::Datetime && row[i].text.size()>19?static_cast<int>(row[i].text.size()-20):0});
        } else { it->type=widen(it->type,type); it->nullable |= row[i].null;if(type==Type::Datetime && row[i].text.size()>19)it->scale=std::max(it->scale,static_cast<int>(row[i].text.size()-20)); }
      }
      ++rows;
    }
  } else throw Error("unsupported file format");
  reader.verify();
  if(schema.columns.empty()) throw Error("cannot infer schema of empty file");
  return schema;
}

inline std::string encode(const Options &o, const Schema &s) {
  rapidjson::StringBuffer out; rapidjson::Writer<rapidjson::StringBuffer> w(out);
  w.StartObject();
  auto str=[&](const char *k,const std::string &v){w.Key(k);w.String(v.data(),v.size());};
  str("path",o.path); str("format",o.format); str("nullstr",o.nullstr);
  w.Key("header");w.Bool(o.header);
  w.Key("delimiter");w.Int(static_cast<unsigned char>(o.delimiter));
  w.Key("quote");w.Int(static_cast<unsigned char>(o.quote));
  w.Key("escape");w.Int(static_cast<unsigned char>(o.escape));
  w.Key("signature");w.StartArray();
  w.Uint64(s.signature.device);w.Uint64(s.signature.inode);w.Uint64(s.signature.size);
  w.Int64(s.signature.mtime);w.Int64(s.signature.mtime_ns);w.Int64(s.signature.ctime);w.Int64(s.signature.ctime_ns);w.EndArray();
  w.Key("columns");w.StartArray();
  for(const auto &c:s.columns){w.StartArray();w.String(c.name.data(),c.name.size());w.Int(static_cast<int>(c.type));w.Bool(c.nullable);w.Int(c.scale);w.EndArray();}
  w.EndArray();w.EndObject();return std::string(out.GetString(),out.GetSize());
}
inline void decode(const std::string &text, Options &o, Schema &s) {
  rapidjson::Document d;d.Parse(text.data(),text.size());
  if(d.HasParseError() || !d.IsObject() || !d.HasMember("columns") || !d.HasMember("signature")) throw Error("invalid file scan plan");
  o.path=d["path"].GetString();o.format=d["format"].GetString();o.nullstr=d["nullstr"].GetString();o.header=d["header"].GetBool();
  o.delimiter=static_cast<char>(d["delimiter"].GetInt());o.quote=static_cast<char>(d["quote"].GetInt());o.escape=static_cast<char>(d["escape"].GetInt());
  auto &sig=d["signature"];s.signature.device=sig[0].GetUint64();s.signature.inode=sig[1].GetUint64();s.signature.size=sig[2].GetUint64();
  s.signature.mtime=sig[3].GetInt64();s.signature.mtime_ns=sig[4].GetInt64();s.signature.ctime=sig[5].GetInt64();s.signature.ctime_ns=sig[6].GetInt64();
  for(auto &v:d["columns"].GetArray()) s.columns.push_back({v[0].GetString(),static_cast<Type>(v[1].GetInt()),v[2].GetBool(),v[3].GetInt()});
}

class Scan {
public:
  Scan(const Options &options, const Schema &schema) : options_(options), schema_(schema), reader_(options) { reset(); }
  void reset() {
    if(options_.format=="parquet") {parquet_=std::make_unique<Parquet>(options_,&schema_.signature);return;}
    if(options_.format=="glob") {files_=discover(options_);file_index_=0;return;}
    reader_.open(&schema_.signature);
    if(options_.format=="csv" && options_.header) reader_.csv(row_);
  }
  void project(const std::vector<size_t> &columns){if(parquet_)parquet_->project(columns);}
  bool next(std::vector<Cell> &out) {
    if(parquet_) return parquet_->next(out);
    if(options_.format=="glob") {if(file_index_==files_.size()) return false;out={{files_[file_index_++],false,false}};return true;}
    if(options_.format=="csv") {
      if(!reader_.csv(out)) return false;
      if(out.size()!=schema_.columns.size()) reader_.fail("CSV field count changed");
    } else {
      if(!reader_.json(names_,row_)) return false;
      out.assign(schema_.columns.size(),Cell{});
      for(size_t i=0;i<names_.size();++i) {
        auto it=std::find_if(schema_.columns.begin(),schema_.columns.end(),[&](const Column &c){return c.name==names_[i];});
        if(it==schema_.columns.end()) reader_.fail("JSONL schema changed");
        out[static_cast<size_t>(it-schema_.columns.begin())]=row_[i];
      }
    }
    return true;
  }
private:
  std::unique_ptr<Parquet> parquet_;
  std::vector<std::string> files_;size_t file_index_=0;
  Options options_; Schema schema_; Reader reader_; std::vector<Cell> row_; std::vector<std::string> names_;
};
}}} // namespace oceanbase::sql::file_sql
