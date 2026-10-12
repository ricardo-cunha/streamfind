"""Strict full-key chemical resolution; caches all retrieved evidence; never changes inputs."""
import csv, json, re, html, hashlib, time, concurrent.futures
from pathlib import Path
from datetime import datetime, timezone
import requests
from rdkit import Chem, rdBase, RDLogger
from rdkit.Chem import rdMolDescriptors
RDLogger.DisableLog('rdApp.*')
ROOT=Path(__file__).resolve().parents[2]
CACHE=ROOT/'tmp/fants_v2'; CACHE.mkdir(parents=True,exist_ok=True)
DATA=Path('H:/Project_261009_fa_nts_ring_trail')
FIELDS=['name','formula','mass','SMILES','InChI','InChIKey']
def read(p):
 with p.open(encoding='utf-8-sig',newline='') as f:return list(csv.DictReader(f))
def fetch(url):
 p=CACHE/(hashlib.sha256(url.encode()).hexdigest()+'.json')
 if p.exists():return json.loads(p.read_text(encoding='utf8'))
 try:
  r=requests.get(url,timeout=(10,35)); obj={'url':url,'status':r.status_code,'text':r.text,'retrieved_utc':datetime.now(timezone.utc).isoformat()}
 except Exception as e:obj={'url':url,'status':'error','text':str(e)}
 p.write_text(json.dumps(obj),encoding='utf8');return obj

def validate(row,smiles,inchi,key):
 issues=[];m=Chem.MolFromSmiles(smiles) if smiles else None
 if m is None:return None,'invalid_or_missing_SMILES'
 derived=Chem.MolToInchi(m); dk=Chem.InchiToInchiKey(derived)
 if dk!=key:issues.append('SMILES_full_key_mismatch:'+dk)
 if inchi and inchi not in ['—','-']:
  mi=Chem.MolFromInchi(inchi)
  if mi is None or Chem.InchiToInchiKey(inchi)!=key:issues.append('InChI_full_key_mismatch')
  if mi is not None and Chem.MolToInchi(mi)!=derived:issues.append('SMILES_InChI_identity_mismatch')
 else:inchi=derived
 formula=rdMolDescriptors.CalcMolFormula(m)
 if formula!=row['formula']:issues.append('formula_mismatch:original='+row['formula']+';structure='+formula)
 out={**{k:row.get(k,'') for k in FIELDS},'SMILES':smiles,'InChI':inchi,'mass':format(rdMolDescriptors.CalcExactMolWt(m),'.9f')}
 return out, ';'.join(issues) or 'validated_full_key_formula_and_structure'

def resolve(row):
 key=row['InChIKey']; evidence=[]; accepted=None
 if not re.fullmatch('[A-Z]{14}-[A-Z]{10}-[A-Z]',key):return row,None,[{'source':'input','status':'invalid_original_InChIKey_format'}]
 url='https://normansle.lcsb.uni.lu/compounds/?field=inchikey&q='+key
 r=fetch(url); links=list(dict.fromkeys(re.findall(r'href="(/compound/[^"/]+/)"',r['text'])))
 if not links:evidence.append({'source':'NORMAN-SLE','url':url,'status':'no_records' if r['status']==200 else str(r['status'])})
 for link in links[:10]:
  u='https://normansle.lcsb.uni.lu'+link;d=fetch(u)
  vals=dict((html.unescape(a.strip()),html.unescape(b.strip())) for a,b in re.findall(r'<div class="cmpd-id-label">(.*?)</div>\s*<div class="cmpd-id-value">\s*<span class="cmpd-id-text">(.*?)</span>',d['text'],re.S))
  title=re.search(r'<h1 class="cmpd-name">(.*?)</h1>',d['text'],re.S)
  ev={'source':'NORMAN-SLE','url':u,'record_id':vals.get('NSID',''),'source_name':html.unescape(title.group(1).strip()) if title else '', 'source_key':vals.get('InChIKey','')}
  if vals.get('InChIKey')!=key:ev['status']='source_full_key_mismatch'
  else:
   out,status=validate(row,vals.get('SMILES',''),vals.get('InChI',''),key);ev['status']=status
   if status.startswith('validated'):accepted=out
  evidence.append(ev)
  if accepted:break
 if not accepted:
  u='https://pubchem.ncbi.nlm.nih.gov/rest/pug/compound/inchikey/'+key+'/property/InChI,InChIKey,IsomericSMILES,MolecularFormula/JSON'
  d=fetch(u)
  try:props=json.loads(d['text']).get('PropertyTable',{}).get('Properties',[])
  except Exception:props=[]
  if not props:evidence.append({'source':'PubChem','url':u,'status':'no_records_or_access_failure:'+str(d['status'])})
  for p in props:
   ev={'source':'PubChem','url':u,'record_id':str(p['CID']),'source_key':p.get('InChIKey',''),'source_formula':p.get('MolecularFormula','')}
   if p.get('InChIKey')!=key:ev['status']='source_full_key_mismatch'
   else:
    out,status=validate(row,p.get('SMILES',p.get('IsomericSMILES','')),p.get('InChI',''),key);ev['status']=status
    if status.startswith('validated'):accepted=out
   evidence.append(ev)
   if accepted:break
 (CACHE/(key+'_result.json')).write_text(json.dumps({'row':row,'accepted':accepted,'evidence':evidence},indent=2),encoding='utf8')
 print(key, 'RESOLVED' if accepted else 'UNRESOLVED',flush=True)
 return row,accepted,evidence

def main():
 original=read(DATA/'suspect_list_fants.csv');old=read(DATA/'suspect_list_fants_conforming.csv');pending=read(DATA/'suspect_list_fants_unresolved.csv')
 manifest={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in [DATA/'suspect_list_fants.csv',DATA/'suspect_list_fants_conforming.csv',DATA/'suspect_list_fants_unresolved.csv']}
 (CACHE/'input_hashes.json').write_text(json.dumps(manifest,indent=2))
 index={r['InChiKey']:i+2 for i,r in enumerate(original)}
 assert len(index)==len(original)==300
 results=list(concurrent.futures.ThreadPoolExecutor(max_workers=3).map(resolve,pending))
 enriched=list(old); unresolved=[]; provenance=[]
 for row,out,evs in results:
  if out:enriched.append(out)
  else:unresolved.append({**row,'resolution_status':'; '.join(e['source']+':'+e['status'] for e in evs)})
  for e in evs:provenance.append({'source_row':index[row['InChIKey']],'original_name':row['name'],'original_formula':row['formula'],'original_InChIKey':row['InChIKey'],'disposition':'newly_resolved' if out else 'unresolved','name_validation':'source name recorded; not an exact-name equivalence assertion',**e})
 for row in old:
  _,status=validate(row,row['SMILES'],row['InChI'],row['InChIKey'])
  m=Chem.MolFromSmiles(row['SMILES'])
  if m is not None and abs(float(row['mass'])-rdMolDescriptors.CalcExactMolWt(m))>0.001:status+=';existing_mass_mismatch'
  provenance.append({'source_row':index[row['InChIKey']],'original_name':row['name'],'original_formula':row['formula'],'original_InChIKey':row['InChIKey'],'disposition':'existing_preserved','source':'existing_conforming_csv','url':str(DATA/'suspect_list_fants_conforming.csv'),'status':status})
 enriched.sort(key=lambda r:index[r['InChIKey']]);unresolved.sort(key=lambda r:index[r['InChIKey']])
 outputs=[('suspect_list_fants_enriched_v2.csv',enriched,FIELDS),('suspect_list_fants_unresolved_v2.csv',unresolved,FIELDS+['resolution_status','source_row']),('suspect_list_fants_resolution_provenance_v2.csv',provenance,['source_row','original_name','original_formula','original_InChIKey','disposition','source','url','record_id','source_name','source_key','source_formula','status','name_validation'])]
 for name,rows,fields in outputs:
  p=DATA/name
  if p.exists():raise FileExistsError(p)
  with p.open('x',encoding='utf-8',newline='') as f:
   w=csv.DictWriter(f,fieldnames=fields);w.writeheader();w.writerows(rows)
  assert read(p)==[{k:str(r.get(k,'')) for k in fields} for r in rows]
 e=read(DATA/outputs[0][0]);u=read(DATA/outputs[1][0]);keys=[r['InChIKey'] for r in e+u]
 assert len(keys)==len(set(keys))==300 and set(keys)==set(index)
 assert all(all(r[k] for k in FIELDS) and float(r['mass'])>0 for r in e)
 bykey={r['InChIKey']:r for r in e};assert all(bykey[r['InChIKey']]==r for r in old)
 assert all(hashlib.sha256((DATA/n).read_bytes()).hexdigest()==h for n,h in manifest.items())
 summary={'original':len(original),'preserved':len(old),'newly_resolved':len(e)-len(old),'enriched':len(e),'unresolved':len(u),'provenance_rows':len(provenance),'rdkit_version':rdBase.rdkitVersion,'existing_audit_issues':[r for r in provenance if r['disposition']=='existing_preserved' and r['status']!='validated_full_key_formula_and_structure'],'outputs':[str(DATA/n) for n,_,_ in outputs]}
 (CACHE/'verification.json').write_text(json.dumps(summary,indent=2),encoding='utf8');print(json.dumps(summary,indent=2))
if __name__=='__main__':main()
