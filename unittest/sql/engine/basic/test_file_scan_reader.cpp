/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#include "sql/engine/basic/file_scan_reader.h"
#include "sql/engine/basic/file_copy_writer.h"
#include <cassert>
#include <fstream>
#include <iostream>
using namespace oceanbase::sql::file_sql;
int main(int argc, char **argv) {
  if(argc>=2){Options fixture;fixture.format="parquet";fixture.path=argv[1];fixture.secure_root="/tmp";auto schema=infer(fixture);Scan scan(fixture,schema);std::vector<Cell> row;int n=0,sum=0;while(scan.next(row)){++n;sum+=std::stoi(row[0].text);}assert(n==7 && sum==21);std::cout<<"official Parquet fixture passed\n";}
  if(argc==3){Options fixture;fixture.format="parquet";fixture.path=argv[2];fixture.secure_root="/tmp";auto schema=infer(fixture);Scan scan(fixture,schema);std::vector<Cell> row;int n=0,sum=0,nulls=0;while(scan.next(row)){++n;if(row[0].null)++nulls;else sum+=std::stoi(row[0].text);}assert(n==6 && sum==13 && nulls==2);scan.reset();scan.project({});n=0;while(scan.next(row))++n;assert(n==6);std::cout<<"multigroup nullable Snappy projection passed\n";}
  char dir[]="/tmp/seekdb-file-reader-XXXXXX";
  assert(mkdtemp(dir));
  Options o; o.secure_root=dir; o.path=std::string(dir)+"/input.csv";
  {
    std::ofstream f(o.path); f<<"id,id,score,active,date,note\r\n1,8,1.5,true,2024-02-29,\"hello\nworld\"\r\n2,9,2,false,,\"say \"\"hi\"\"\"\r\n";
  }
  auto s=infer(o);
  assert(s.columns.size()==6 && s.columns[1].name=="id__2");
  assert(s.columns[2].type==Type::Double && s.columns[3].type==Type::Boolean);
  assert(s.columns[4].type==Type::Date && s.columns[4].nullable);
  Scan scan(o,s); std::vector<Cell> row;
  assert(scan.next(row) && row[5].text=="hello\nworld");
  assert(scan.next(row) && row[5].text=="say \"hi\"");
  assert(!scan.next(row)); scan.reset(); assert(scan.next(row));
  { std::ofstream f(o.path,std::ios::app); f<<"3,10,4,true,,x\n"; }
  bool changed=false;try { scan.next(row); } catch(const Error &) {changed=true;} assert(changed);
  o.format="jsonl"; o.path=std::string(dir)+"/input.jsonl";
  {std::ofstream f(o.path);f<<"{\"id\":1,\"a\":true}\n{\"id\":2.5,\"b\":\"x\"}\n";}
  s=infer(o); assert(s.columns.size()==3 && s.columns[0].type==Type::Double && s.columns[1].nullable);
  Scan jsonscan(o,s); assert(jsonscan.next(row) && row[2].null); assert(jsonscan.next(row) && row[1].null);
  {std::ofstream f(o.path);f<<"{\"a\":[]}\n";}
  bool nested=false;try {infer(o);}catch(const Error &){nested=true;}assert(nested);
  o.path="/etc/passwd";bool denied=false;try{infer(o);}catch(const Error &){denied=true;}assert(denied);
  assert(!date("2023-02-29",false) && date("2024-02-29",false));
  assert(classify({"9223372036854775808",false,false})==Type::Varchar);
  assert(!valid_utf8(std::string("\xc0\x80",2)));
  o.path=std::string(dir)+"/*.csv";o.format="glob";
  auto discovered=discover(o);assert(discovered.size()==1);
  auto gs=infer(o);Scan globscan(o,gs);assert(globscan.next(row) && row[0].text==discovered[0]);assert(!globscan.next(row));
  auto encoded=encode(o,gs);Options decoded;Schema decoded_schema;decode(encoded,decoded,decoded_schema);
  assert(decoded.path==o.path && decoded_schema.columns[0].name=="file");
  std::string link=std::string(dir)+"/escape.csv";assert(symlink("/etc/passwd",link.c_str())==0);
  bool symlink_denied=false;try{discover(o);}catch(const Error &){symlink_denied=true;}assert(symlink_denied);unlink(link.c_str());
  o.format="csv";o.path=std::string(dir)+"/input.csv";
  {std::ofstream f(o.path);f<<"a,b\n1\n";}
  bool bad_count=false;try{infer(o);}catch(const Error &e){bad_count=std::string(e.what()).find("record")!=std::string::npos;}assert(bad_count);
  {std::ofstream f(o.path);f<<"a\n\"unfinished\n";}
  bool bad_quote=false;try{infer(o);}catch(const Error &){bad_quote=true;}assert(bad_quote);
  Options out;out.secure_root=dir;out.path=std::string(dir)+"/export.csv";out.nullstr="NULL";
  {CopyWriter writer(out);writer.row({{"a",false,false},{"b",false,false}});writer.row({{"quoted \"text\"\nline",false,false},{"",true,false}});writer.finish();}
  auto exported=infer(out);Scan exported_scan(out,exported);assert(exported_scan.next(row) && row[1].null);
  bool exists=false;try{CopyWriter writer(out);writer.finish();}catch(const Error &){exists=true;}assert(exists);
  unlink(out.path.c_str());
  unlink((std::string(dir)+"/input.csv").c_str());unlink((std::string(dir)+"/input.jsonl").c_str());rmdir(dir);
  std::cout<<"file reader correctness tests passed\n";
}
