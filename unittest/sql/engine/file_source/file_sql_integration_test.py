"""Native SQL regression on a dedicated local test server (no external SQL engine).

Run with --client /path/to/obclient --socket /path/to/sql.sock.
Requires a socket administrator. Temporarily changes GLOBAL secure_file_priv,
restoring it in finally; do not run concurrently on a shared production server.
"""
from pathlib import Path
import argparse, subprocess, os, tempfile
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--client',required=True)
parser.add_argument('--socket',required=True)
parser.add_argument('--user',default='root')
args=parser.parse_args()
C=args.client
env=dict(os.environ)
workspace=tempfile.TemporaryDirectory(prefix='file-sql-integration-')
base=Path(workspace.name)
d=base/'allowed'
d.mkdir()
def literal(value):
 return "'"+str(value).replace('\\','\\\\').replace("'","''")+"'"
def sql(s,ok=True):
 p=subprocess.run([C,'--socket='+args.socket,'--user='+args.user,'--batch','--skip-column-names','--default-character-set=utf8mb4','-e',s],env=env,capture_output=True,text=True)
 assert (p.returncode==0)==ok,(s,p.stdout,p.stderr)
 return p.stdout.strip()
def q(s,ok=True):return sql('set session enable_file_sql=on; '+s,ok)
def put(name,data):
 (d/name).write_text(data,encoding='utf-8');return str(d/name)
x=put('widen.csv','x\ntrue\n1.5\nfalse\n')
assert q(f"select sum(x), count(*) from read_csv('{x}')")=='2.5\t3'
x=put('widen.jsonl','{"x":true}\n{"x":1.5}\n{"x":false}\n')
assert q(f"select sum(x), count(*) from read_ndjson('{x}')")=='2.5\t3'
x=put('empty.csv','x\n""\n1\n')
assert q(f"select count(*),sum(x=''),sum(x='1') from read_csv('{x}')")=='2\t1\t1'
x=put('unicode.jsonl','{"x":"\\uD83D\\uDE00"}\n')
assert q(f"select hex(x) from read_json('{x}')")=='F09F9880'
for n,data in [('badnumber.jsonl','{"x":1e}\n'),('nested.jsonl','{"x":[]}\n'),('badquote.csv','x\n"unterminated\n')]:
 x=put(n,data);q(f"select * from {'read_csv' if n.endswith('csv') else 'read_json'}('{x}')",False)
# Verify real SQL layer path restrictions including glob and COPY.
outside=base/'outside'
outside.mkdir(exist_ok=True)
(outside/'secret.csv').write_text('x\n9\n')
link=d/'escape'
if not link.exists():link.symlink_to(outside,target_is_directory=True)
old=sql('select @@global.secure_file_priv')
try:
 sql('set global secure_file_priv='+literal(d))
 x=put('allowed.csv','x\n1\n')
 assert q(f"select sum(x) from read_csv('{x}')")=='1'
 for s in [f"select * from read_csv('{outside}/secret.csv')",f"select * from read_csv('{d}/escape/secret.csv')",f"select * from glob('{d}/escape/*')",f"copy (select 1) to '{d}/escape/forbidden.csv'"]:
  q(s,False)
 assert not (outside/'forbidden.csv').exists()
 sql("set global secure_file_priv='' ")
 q(f"select * from read_csv('{x}')",False)
finally:sql('set global secure_file_priv='+literal(old))
print('SPECIAL_SQL_CHECKS: PASS (widening, empty-string, unicode, invalid records, secure root/symlink/glob/COPY/default-deny)')
