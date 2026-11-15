"""Real seekdb SQL tests; requires a disposable DB and explicit secure_file_priv."""
import argparse
import base64
import json
import subprocess
import tempfile
from pathlib import Path

p=argparse.ArgumentParser()
p.add_argument('--client',required=True)
p.add_argument('--port',default='2884')
p.add_argument('--socket',required=True)
a=p.parse_args()
repo=Path(__file__).resolve().parents[4]
data=repo/'tools/deploy/mysql_test/test_suite/file_sql/data'
checks=[]
def sql(query, expected=None, error=False):
    result=subprocess.run([a.client,'--no-defaults','-h127.0.0.1','-P'+a.port,'-uroot','-Dtest','-NB','--raw','-e',query],text=True,capture_output=True,timeout=120)
    print(query,flush=True)
    if error:
        assert result.returncode!=0,(query,result.stdout)
    else:
        assert result.returncode==0,(query,result.stderr)
        if expected is not None:assert result.stdout.strip()==expected,(query,result.stdout,expected)
    checks.append({'sql':query,'exit_code':result.returncode,'expected_error':error,'stdout':result.stdout,'stderr':result.stderr})
    return result.stdout.strip()
with tempfile.TemporaryDirectory(prefix='seekdb-file-e2e-') as folder:
    d=Path(folder)
    for name in ['basic.csv','basic.jsonl','invalid.txt']:(d/name).write_bytes((data/name).read_bytes())
    (d/'simple.parquet').write_bytes(base64.b64decode((data/'simple.parquet.b64').read_bytes()))
    sql('SELECT @@session.enable_file_sql','0')
    sql(f"SELECT * FROM read_csv('{d}/basic.csv')",error=True)
    # Only the disposable instance is configured by this test.
    subprocess.run([a.client,'--no-defaults','--protocol=SOCKET','--socket='+a.socket,'-uroot','-e',f"SET GLOBAL secure_file_priv='{d}'"],check=True,text=True,capture_output=True)
    prefix='SET SESSION enable_file_sql=ON; '
    def q(statement,expected=None,error=False):return sql(prefix+statement,expected,error)
    q(f"SELECT COUNT(*),SUM(id),ROUND(AVG(score),2),SUM(active) FROM read_csv('{d}/basic.csv',header=true,sample_size=-1)",'3\t6\t91.92\t2')
    q(f"SELECT id,name,id__2 FROM read_csv('{d}/basic.csv') f ORDER BY id",'1\tAlice\t101\n2\tBob\t102\n3\tChen\t103')
    q(f"SELECT COUNT(*),SUM(id) FROM read_json('{d}/basic.jsonl',format='newline_delimited')",'3\t6')
    q(f"SELECT id,day,note FROM read_ndjson('{d}/basic.jsonl') ORDER BY id",'1\tNULL\tNULL\n2\t2026-08-02\tNULL\n3\tNULL\tok')
    q(f"SELECT COUNT(*),MIN(c1),MAX(c1),SUM(c1) FROM read_parquet('{d}/simple.parquet')",'7\t0\t6\t21')
    q(f"SELECT a.id,b.name FROM read_csv('{d}/basic.csv') a JOIN read_json('{d}/basic.jsonl') b ON a.id=b.id ORDER BY a.id",'1\tAlice\n2\tBob\n3\tChen')
    q(f"SELECT COUNT(*) FROM read_csv('{d}/basic.csv') a LEFT JOIN read_parquet('{d}/simple.parquet') b ON a.id=b.c1",'3')
    q(f"SELECT active,COUNT(*),SUM(id) FROM read_csv('{d}/basic.csv') GROUP BY active HAVING COUNT(*)>1",'1\t2\t4')
    q(f"SELECT id+1 AS next_id FROM read_csv('{d}/basic.csv') WHERE score>90 ORDER BY id DESC LIMIT 1",'4')
    desc=q(f"DESCRIBE SELECT * FROM read_csv('{d}/basic.csv')")
    assert 'BIGINT' in desc and 'BOOLEAN' in desc and 'DATETIME' in desc,desc
    q(f"SELECT COUNT(*) FROM glob('{d}/*')",'4')
    q(f"SELECT * FROM read_csv('{d}/basic.csv',sample_size=10)",error=True)
    q("SELECT * FROM read_csv('/etc/passwd')",error=True)
    (d/'outside.csv').symlink_to('/etc/passwd')
    q(f"SELECT * FROM read_csv('{d}/outside.csv')",error=True)
    (d/'outside.csv').unlink()
    q(f"COPY (SELECT id,name,score FROM read_csv('{d}/basic.csv') ORDER BY id) TO '{d}/export.csv' (format csv,header,nullstr 'NULL')")
    q(f"SELECT id,name,score FROM read_csv('{d}/export.csv') ORDER BY id",'1\tAlice\t98.5\n2\tBob\t87\n3\tChen\t90.25')
    before=(d/'export.csv').read_bytes()
    q(f"COPY (SELECT 5) TO '{d}/export.csv' (header)",error=True)
    assert (d/'export.csv').read_bytes()==before
    assert not list(d.glob('.seekdb-copy-*'))
    # Additional correctness/security checks beyond the public smoke test.
    (d/'nulls.csv').write_text('n,x\n,9223372036854775807\n,-9223372036854775808\n')
    q(f"SELECT COUNT(*),COUNT(n),MIN(x),MAX(x) FROM read_csv('{d}/nulls.csv')",'2\t0\t-9223372036854775808\t9223372036854775807')
    q(f"SELECT a.id,b.name FROM read_csv('{d}/basic.csv') a LEFT JOIN read_json('{d}/basic.jsonl') b ON a.id+1=b.id ORDER BY a.id",'1\tBob\n2\tChen\n3\tNULL')
    q(f"SELECT a.id,b.id FROM read_csv('{d}/basic.csv') a JOIN read_csv('{d}/basic.csv') b ON a.id=b.id ORDER BY a.id",'1\t1\n2\t2\n3\t3')
    (d/'bad.csv').write_text('a,b\n1\n')
    q(f"SELECT * FROM read_csv('{d}/bad.csv')",error=True)
    (d/'nested.jsonl').write_text('{"a":[]}\n')
    q(f"SELECT * FROM read_json('{d}/nested.jsonl')",error=True)
    q(f"SELECT * FROM read_csv('{d}/../basic.csv')",error=True)
    q(f"COPY (SELECT id FROM read_csv('{d}/basic.csv') WHERE 1=0) TO '{d}/empty.csv' (header)")
    q(f"SELECT COUNT(*) FROM read_csv('{d}/empty.csv')",'0')
    q('CREATE TABLE IF NOT EXISTS file_sql_export_source (id INT PRIMARY KEY, value VARCHAR(50))')
    q('DELETE FROM file_sql_export_source')
    q("INSERT INTO file_sql_export_source VALUES (1,'one'),(2,NULL)")
    q(f"COPY file_sql_export_source TO '{d}/table.csv' (header, nullstr 'NULL')")
    q(f"SELECT id,value FROM read_csv('{d}/table.csv',nullstr='NULL') ORDER BY id",'1\tone\n2\tNULL')
    q('DROP TABLE file_sql_export_source')
    # A non-domain expression error after partial output must not publish a file.
    q(f"COPY (SELECT id, REGEXP_REPLACE(name, IF(id=3,'[','A'), 'x') AS value FROM read_csv('{d}/basic.csv') ORDER BY id) TO '{d}/query-failure.csv' (header)",error=True)
    assert not (d/'query-failure.csv').exists()
    assert not list(d.glob('.seekdb-copy-*'))
    # Stream a larger file through the same native aggregate executor.
    with (d/'large.csv').open('w') as f:
        f.write('id,value\n')
        for i in range(100000):f.write(f'{i},{i%17}\n')
    q(f"SELECT COUNT(*),SUM(id) FROM read_csv('{d}/large.csv')",'100000\t4999950000')
    q('SELECT 1','1')
Path('/home/xy/seekdb-ai-work/file-sql-e2e.json').write_text(json.dumps(checks,indent=2)+'\n')
print(f'{len(checks)} real SQL checks passed')
