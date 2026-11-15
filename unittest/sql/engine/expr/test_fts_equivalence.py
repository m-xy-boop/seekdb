# Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
"""Requires PyMySQL and the unmodified fts_large_bench dataset and indexes.
Run capture before replacing the server, compare on the same indexes to isolate engine behavior.
This checks full token frequencies, document lengths, row IDs and MATCH scores.
It does not measure performance or change any official benchmark settings.
"""
from pathlib import Path
import json,random,argparse
import pymysql
parser=argparse.ArgumentParser(description='Capture and compare complete FTS results on a dedicated benchmark instance.')
parser.add_argument('--mode',choices=['capture','compare'],required=True)
parser.add_argument('--snapshot',type=Path,required=True)
parser.add_argument('--host',default='127.0.0.1')
parser.add_argument('--port',type=int,default=2884)
args=parser.parse_args()
mode=args.mode
c=pymysql.connect(host=args.host,port=args.port,user='root',autocommit=True,charset='utf8mb4')
texts=['','hello WORLD hello','The quick brown fox jumps over the lazy dog.','ABC abc AbC 123,456__test','中文全文索引测试，数据库性能优化。','OceanBase 稳定 database 全文索引','ＡＢＣ１２３ full-width café naïve Straße İΣσς','日本語 カタカナ 한글 테스트 😀𠀀','a\x00b\tfoo\nbar\r\nEND','中华人民共和国一百二十三公斤2026年11月','!!! … — _ @ # $ %','x'*1024,'数据'*600+'ABC xyz']
rng=random.Random(941)
for _ in range(100):texts.append(''.join(rng.choice('abcABC012 中文索引éß😀\t\n,.!?_') for _ in range(rng.randint(1,240))))
results=[]
with c.cursor() as q:
 for text in texts:
  for parser in ['ik','beng','space','ngram']:
   q.execute("SELECT TOKENIZE(%s,%s,'[{\"output\":\"all\"}]') AS tokens",(text,parser))
   value=q.fetchone()[0]
   try:
    value=json.loads(value)
    if isinstance(value,dict) and isinstance(value.get('tokens'),list): value['tokens']=sorted(value['tokens'],key=lambda x:json.dumps(x,sort_keys=True,ensure_ascii=False))
   except (ValueError,TypeError):pass
   results.append({'text':text,'parser':parser,'value':value})
 # Exercise stopword conversion fallback as well as the common UTF-8 path.
 for charset in ['latin1','gbk','utf16','utf16le']:
  for parser_name in ['beng','space']:
   sql=f"SELECT TOKENIZE(CONVERT(%s USING {charset}),%s,'[{{\"output\":\"all\"}}]') AS tokens"
   try:
    q.execute(sql,('The DATABASE and database audit 123',parser_name))
    value=q.fetchone()[0]
    try:
     value=json.loads(value)
     if isinstance(value,dict) and isinstance(value.get('tokens'),list):value['tokens']=sorted(value['tokens'],key=lambda x:json.dumps(x,sort_keys=True))
    except (ValueError,TypeError):pass
    results.append({'charset':charset,'parser':parser_name,'value':value})
   except pymysql.MySQLError as error:
    results.append({'charset':charset,'parser':parser_name,'error':list(error.args)})
 for fields,term in [('title,content','全文索引 分词器'),('title_en,content_en','database performance audit'),('title,content','OceanBase 稳定 database'),('content','倒排索引 tokenizer')]:
  q.execute(f'SELECT id,MATCH({fields}) AGAINST(%s) FROM fts_large_bench.docs WHERE MATCH({fields}) AGAINST(%s) ORDER BY id',(term,term))
  results.append({'fields':fields,'term':term,'rows':q.fetchall()})
c.close()
# Normalize tuples and JSON object ordering through roundtrip.
results=json.loads(json.dumps(results,ensure_ascii=False))
if mode=='capture':
 args.snapshot.write_text(json.dumps(results,ensure_ascii=False,indent=2)+'\n');print('Baseline captured:',len(texts)*4+8,'tokenize cases; 4 complete MATCH row/score sets')
else:
 expected=json.loads(args.snapshot.read_text());assert len(expected)==len(results)
 for i,(a,b) in enumerate(zip(expected,results)):
  if a != b:
   if 'rows' in a:
    first=next(((x,y) for x,y in zip(a['rows'],b['rows']) if x!=y),None)
    raise AssertionError(('MATCH results differ',i,len(a['rows']),len(b['rows']),'first differing row',first))
   raise AssertionError(('TOKENIZE differs',i,a,b))

 print('Exact before/after results match:',len(texts)*4+8,'tokenize cases; 4 complete MATCH row/score sets')
