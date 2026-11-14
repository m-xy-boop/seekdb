"""End-to-end product tests against seekdb and mock_ai_service.py.

Start a disposable seekdb on port 2884 and the mock service on port 8000.
Run: python3 unittest/sql/engine/expr/test_ai_functions.py --client PATH
The fixture deliberately reorders responses. No real model or API key is used.
"""
import argparse
import json
import os
import subprocess
import unittest
from pathlib import Path

PARSER = argparse.ArgumentParser()
PARSER.add_argument('--client', required=True)
PARSER.add_argument('--port', default='2884')
ARGS, REST = PARSER.parse_known_args()

class AIFunctionTests(unittest.TestCase):
    @classmethod
    def sql(cls, statement, success=True):
        client = Path(ARGS.client).resolve()
        env = os.environ.copy()
        env['LD_LIBRARY_PATH'] = str(client.parent.parent / 'lib') + ':' + env.get('LD_LIBRARY_PATH', '')
        result = subprocess.run([str(client), '--no-defaults', '-h127.0.0.1', '-P'+ARGS.port,
                                 '-uroot', '-Dtest', '-N', '-B', '--raw', '-e', statement],
                                text=True, capture_output=True, env=env, timeout=90)
        if success and result.returncode:
            raise AssertionError(statement + '\n' + result.stderr)
        if not success and not result.returncode:
            raise AssertionError('Expected SQL error: ' + statement)
        return result.stdout.strip()

    @classmethod
    def setUpClass(cls):
        models = [('comp', 'completion', 'fixture-echo', 'openai', '/v1/chat/completions'),
                  ('embed', 'dense_embedding', 'fixture-vector', 'openai', '/v1/embeddings'),
                  ('ollama_comp', 'completion', 'fixture-echo', 'oLlAmA', '/api/generate'),
                  ('ollama_embed', 'dense_embedding', 'fixture-vector', 'OLLAMA', '/api/embed'),
                  ('rank', 'rerank', 'fixture-rank', 'siliconflow', '/v1/rerank'),
                  ('bad_embed', 'dense_embedding', 'fixture-invalid-vector', 'openai', '/v1/embeddings'),
                  ('bad_rank', 'rerank', 'fixture-invalid-rank', 'siliconflow', '/v1/rerank'),
                  ('empty_comp', 'completion', 'fixture-empty', 'openai', '/v1/chat/completions'),
                  ('http_error', 'completion', 'fixture-http-error', 'openai', '/v1/chat/completions'),
                  ('http_error_embed', 'dense_embedding', 'fixture-http-error', 'openai', '/v1/embeddings'),
                  ('wrong_kind_rank', 'completion', 'fixture-rank', 'siliconflow', '/v1/rerank'),
                  ('duplicate_embed', 'dense_embedding', 'fixture-duplicate-index', 'openai', '/v1/embeddings')]
        cls.names = []
        for name, kind, model, provider, route in models:
            name = 'enh_' + name
            cls.sql("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL('%s','%s')" % (name, json.dumps({'type':kind,'model_name':model})))
            cls.names.append(name)
            config = {'ai_model_name':name,'url':'http://127.0.0.1:8000'+route,
                      'provider':provider,'access_key':'' if provider.upper()=='OLLAMA' else 'local-fixture'}
            if name == 'enh_ollama_embed':
                config.pop('access_key')
            cls.sql("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT('%s_ep','%s')" % (name,json.dumps(config)))
        cls.sql('CREATE TABLE enh_rows (id INT PRIMARY KEY, value LONGTEXT)')
        cls.sql("INSERT INTO enh_rows VALUES " + ','.join("(%d,%s)" % (i,'NULL' if i%7==0 else "'%s'" % ('x'*(i%9+1))) for i in range(1,621)))

    @classmethod
    def tearDownClass(cls):
        cls.sql('DROP TABLE IF EXISTS enh_rows')
        for name in reversed(cls.names):
            cls.sql("CALL DBMS_AI_SERVICE.DROP_AI_MODEL_ENDPOINT('%s_ep')" % name)
            cls.sql("CALL DBMS_AI_SERVICE.DROP_AI_MODEL('%s')" % name)

    def test_prompt_json_and_nested(self):
        prompt = json.loads(self.sql("SELECT AI_PROMPT('{0} {1}', JSON_ARRAY('a',2), JSON_OBJECT('k','v'))"))
        self.assertEqual([['a',2], {'k':'v'}], prompt['args'])
        self.assertEqual('nested:value', self.sql("SELECT AI_COMPLETE('enh_comp', AI_PROMPT('nested:{0}', AI_PROMPT('{0}', 'value')))"))
        text = self.sql("SELECT AI_COMPLETE('enh_comp', AI_PROMPT('{0}', JSON_OBJECT('hello','世界')))")
        self.assertEqual({'hello':'世界'}, json.loads(text))

    def test_prompt_repeated_and_unicode(self):
        self.assertEqual('世界'*30, self.sql("SELECT AI_COMPLETE('enh_comp',AI_PROMPT(REPEAT('{0}',30),'世界'))"))
        self.sql("SELECT AI_COMPLETE('enh_comp',AI_PROMPT('{99999999999999999999999999}','x'))", False)
        self.sql("SELECT AI_COMPLETE('enh_comp',AI_PROMPT('{1}','x'))", False)

    def test_prompt_longtext_and_config(self):
        self.assertEqual('x'*70000, self.sql("SELECT AI_COMPLETE('enh_comp', AI_PROMPT('{0}', REPEAT('x',70000)), JSON_OBJECT('temperature',0.2,'max_tokens',100))"))
        self.assertEqual('abc', self.sql("SELECT AI_COMPLETE('enh_comp','abc',NULL)"))
        self.assertEqual('abc', self.sql("SELECT AI_COMPLETE('enh_comp','abc')"))
        self.assertEqual('x'*70001,self.sql("SELECT AI_COMPLETE('enh_comp',AI_PROMPT(CONCAT(REPEAT('x',70000),'{0}'),'x'))"))
        log=Path(os.environ.get('AI_MOCK_LOG','/tmp/seekdb-ai-mock-requests.jsonl'))
        configured=[json.loads(line)['body'] for line in log.read_text().splitlines() if 'max_tokens' in json.loads(line)['body']]
        self.assertTrue(any(body['temperature']==0.2 and body['max_tokens']==100 for body in configured))
        self.sql("SELECT AI_COMPLETE('enh_empty_comp','abc')",False)
        self.sql("SELECT AI_COMPLETE('enh_http_error','abc')",False)
        self.assertEqual('1',self.sql('SELECT 1'))

    def test_embed_dimensions_and_validation(self):
        self.assertEqual([3.0,1.0,2.0], json.loads(self.sql("SELECT AI_EMBED('enh_embed','abc')")))
        self.assertEqual([3.0,1.0,2.0,3.0,4.0], json.loads(self.sql("SELECT AI_EMBED('enh_embed','abc',5)")))
        self.assertEqual('1', self.sql("SELECT AI_EMBED('enh_embed',NULL) IS NULL"))
        for dim in ['0','-1','1.5','NULL']:
            self.sql("SELECT AI_EMBED('enh_embed','abc',%s)" % dim,False)
        self.sql("SELECT AI_EMBED('enh_bad_embed','abc')",False)

    def test_ollama(self):
        self.assertEqual('你好',self.sql("SELECT AI_COMPLETE('enh_ollama_comp','你好')"))
        self.assertEqual([3.0,1.0,2.0], json.loads(self.sql("SELECT AI_EMBED('enh_ollama_embed','abc')")))
        self.sql("CALL DBMS_AI_SERVICE.ALTER_AI_MODEL_ENDPOINT('enh_ollama_comp_ep', '{\"provider\":\"ollama\"}')")
        self.assertEqual('changed',self.sql("SELECT AI_COMPLETE('enh_ollama_comp','changed')"))
        self.assertEqual('0',self.sql("SELECT ACCESS_KEY IS NULL FROM oceanbase.DBA_OB_AI_MODEL_ENDPOINTS WHERE ENDPOINT_NAME='enh_ollama_comp_ep'"))
        self.sql("CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT('enh_empty_key_ep','{\"ai_model_name\":\"enh_comp\",\"url\":\"http://127.0.0.1:8000/v1/chat/completions\",\"provider\":\"openai\",\"access_key\":\"\"}')",False)

    def test_rerank(self):
        result=json.loads(self.sql("SELECT AI_RERANK('enh_rank','query',JSON_ARRAY('a','bb','ccc','dd'))"))
        self.assertEqual([1,3,0,2],[r['index'] for r in result])
        strings=['x'*(i%5+1) for i in range(47)]
        ranked=json.loads(self.sql("SELECT AI_RERANK('enh_rank','query',CAST('%s' AS JSON))" % json.dumps(strings)))
        self.assertEqual(sorted(range(47),key=lambda i: (-(len(strings[i])%3/2.0),i)),[r['index'] for r in ranked])
        docs=[{'id':i,'body':'x'*(i%5+1)} for i in range(47)]
        result=json.loads(self.sql("SELECT AI_RERANK('enh_rank','query',CAST('%s' AS JSON),'body')" % json.dumps(docs)))
        expected=sorted(docs,key=lambda d: (-(len(d['body'])%3/2.0),d['id']))
        self.assertEqual([d['id'] for d in expected],[d['id'] for d in result])
        self.sql("SELECT AI_RERANK('enh_bad_rank','q',JSON_ARRAY('a'))",False)
        self.sql("SELECT AI_RERANK('enh_wrong_kind_rank','q',JSON_ARRAY('a'))",False)
        self.sql("SELECT AI_RERANK('enh_wrong_kind_rank','q',JSON_ARRAY(JSON_OBJECT('text','a')),'text')",False)
        self.assertEqual('1',self.sql('SELECT 1'))

    def test_embed_varying_models_and_dimensions(self):
        rows = self.sql("SELECT id, AI_EMBED(CASE WHEN MOD(id,2)=0 THEN 'enh_embed' ELSE 'enh_ollama_embed' END,value,MOD(id,3)+1) FROM enh_rows WHERE id<=80 ORDER BY id").splitlines()
        self.assertEqual(80,len(rows))
        for row in rows:
            index,vector=row.split('\t',1);index=int(index)
            if index%7==0:
                self.assertEqual('NULL',vector)
            else:
                self.assertEqual([float(index%9+1)]+[float(i) for i in range(1,index%3+1)],json.loads(vector))
        self.assertEqual('1',self.sql("SELECT AI_EMBED(NULL,'abc') IS NULL"))

    def test_embed_rows_mapping(self):
        log = Path(os.environ.get('AI_MOCK_LOG','/tmp/seekdb-ai-mock-requests.jsonl'))
        before = len(log.read_text().splitlines())
        rows=self.sql("SELECT id, AI_EMBED('enh_embed',value,4) FROM enh_rows WHERE id>4 ORDER BY id LIMIT 600").splitlines()
        self.assertEqual(600,len(rows))
        for row in rows:
            index,vector=row.split('\t',1);index=int(index)
            if index%7==0:
                self.assertEqual('NULL',vector)
            else:
                self.assertEqual([float(index%9+1),1.0,2.0,3.0],json.loads(vector))
        requests = [json.loads(line) for line in log.read_text().splitlines()[before:]]
        batches = [r['body']['input'] for r in requests if r['path'].endswith('/embeddings')]
        self.assertTrue(any(len(batch)>1 for batch in batches), 'No cross-row batch request observed')
        self.assertTrue(all(None not in batch for batch in batches))
        self.sql("SELECT AI_EMBED('enh_duplicate_embed',value) FROM enh_rows WHERE value IS NOT NULL",False)
        self.sql("SELECT AI_EMBED('enh_http_error_embed',value) FROM enh_rows WHERE value IS NOT NULL LIMIT 40",False)
        self.assertEqual('1',self.sql('SELECT 1'))

if __name__ == '__main__':
    unittest.main(argv=[__file__]+REST)
