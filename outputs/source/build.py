"""Build the delivered Windows source bundle. Python 3.11+ standard library.
Runtime is fully offline; only missing build toolchains may be downloaded.
Archive layout is preserved: outputs/source, outputs/app, work/downloads.
"""
import argparse, hashlib, json, os, pathlib, shutil, subprocess, tarfile, urllib.request, zipfile
p=argparse.ArgumentParser();p.add_argument('--runtime',type=pathlib.Path)
p.add_argument('--prepare-only',action='store_true');p.add_argument('--offline',action='store_true')
p.add_argument('--jobs',type=int,default=4);a=p.parse_args()
source=pathlib.Path(__file__).resolve().parent;root=source.parents[1];work=root/'work';app=root/'outputs/app'
lock=json.loads((source/'dependencies.lock.json').read_text(encoding='utf8'))['files']
app.mkdir(parents=True,exist_ok=True)
def sha(path):
    h=hashlib.sha256()
    with open(path,'rb') as f:
        while b:=f.read(1048576):h.update(b)
    return h.hexdigest()
def require(relative):
    e=next(x for x in lock if x['file']==relative);path=root/relative
    if not path.exists():
        if a.offline:raise RuntimeError('Missing offline dependency: '+relative)
        path.parent.mkdir(parents=True,exist_ok=True);tmp=path.with_suffix('.partial')
        urllib.request.urlretrieve(e['url'],tmp);tmp.replace(path)
    if sha(path)!=e['sha256']:raise RuntimeError('SHA256 mismatch: '+relative)
    return path
def extract(file,destination,expected):
    archive=require('work/downloads/'+file)
    if (destination/expected).exists():return
    destination.mkdir(parents=True,exist_ok=True)
    if file.endswith('.zip'):
        with zipfile.ZipFile(archive) as z:z.extractall(destination)
    else:
        with tarfile.open(archive) as t:t.extractall(destination,filter='data')
    if not (destination/expected).exists():raise RuntimeError('Unexpected archive layout: '+file)
for file,folder in [('llama.zip','llama.cpp-0.6.0'),('whisper.zip','whisper.cpp-1.9.5'),
 ('pjsip.zip','pjproject-2.17'),('sqlite.zip','sqlite-amalgamation-3530000'),
 ('gammu-source.zip','Gammu-1.45.0'),('normalizer.zip','normalizer-tr-0.4.0'),
 ('ort.zip','onnxruntime-win-x64-1.31.0'),('sherpa.tar.bz2','sherpa-onnx-v1.13.8-win-x64-shared-MD-Release')]:
    extract(file,work/'deps',folder)
header=require('work/deps/include/nlohmann/json.hpp')
for replacement in (source/'patches/files').rglob('*'):
    if replacement.is_file():
        target=work/'deps'/replacement.relative_to(source/'patches/files')
        shutil.copy2(replacement,target)
print('Pinned native dependencies and patches prepared',flush=True)
if a.prepare_only:raise SystemExit(0)
extract('llvm.zip',work/'tools','llvm-mingw-20261006-ucrt-x86_64')
extract('cmake.zip',work/'tools','cmake-4.4.4-windows-x86_64')
extract('ninja.zip',work/'tools/ninja','ninja.exe')
clang=work/'tools/llvm-mingw-20261006-ucrt-x86_64/bin'
cmake=work/'tools/cmake-4.4.4-windows-x86_64/bin/cmake.exe';ninja=work/'tools/ninja/ninja.exe'
env=os.environ.copy();env['PATH']=str(clang)+';'+str(ninja.parent)+';'+env['PATH']
env['RUSTUP_HOME']=str(work/'rustup');env['CARGO_HOME']=str(work/'cargo')
env['CARGO_TARGET_X86_64_PC_WINDOWS_GNULLVM_LINKER']=str(clang/'x86_64-w64-mingw32-clang.exe')
env['CARGO_TARGET_DIR']=str(work/'rust-build')
def run(cmd,cwd=root):
    print('RUN',str(cmd[0]),flush=True)
    subprocess.run([str(x) for x in cmd],cwd=cwd,env=env,check=True)
rustup=work/'cargo/bin/rustup.exe';cargo=work/'cargo/bin/cargo.exe'
if not cargo.exists():
    if a.offline:raise RuntimeError('Rust toolchain not installed in work/cargo; prepare Rust 1.99.0 first')
    installer=require('work/tools/rustup-init.exe')
    run([installer,'-y','--no-modify-path','--default-host','x86_64-pc-windows-gnu','--default-toolchain','1.99.0','--profile','minimal'])
run([rustup,'target','add','x86_64-pc-windows-gnullvm','--toolchain','1.99.0'])
cargo_config=source/'normalizer/.cargo/config.toml';cargo_config.parent.mkdir(exist_ok=True)
cargo_config.write_text('[source.crates-io]\nreplace-with="vendored-sources"\n[source.vendored-sources]\ndirectory="vendor"\n',encoding='utf8')
run([cargo,'+1.99.0','build','--release','--offline','--locked','--target','x86_64-pc-windows-gnullvm'],source/'normalizer')
shutil.copy2(work/'rust-build/x86_64-pc-windows-gnullvm/release/ajan_normalizer.dll',app/'ajan_normalizer.dll')
common=['-G','Ninja','-DCMAKE_BUILD_TYPE=Release','-DCMAKE_MAKE_PROGRAM='+str(ninja),
        '-DCMAKE_C_COMPILER='+str(clang/'clang.exe')]
run([cmake,'-S',work/'deps/Gammu-1.45.0','-B',work/'gammubuild',*common,
     '-DBUILD_SHARED_LIBS=ON','-DBUILD_TESTING=OFF','-DAJAN_BUILD_SMSD=OFF','-DWITH_BLUETOOTH=OFF'])
run([cmake,'--build',work/'gammubuild','--target','libGammu','--parallel',a.jobs])
run([cmake,'-S',source,'-B',work/'build',*common,'-DCMAKE_CXX_COMPILER='+str(clang/'clang++.exe')])
run([cmake,'--build',work/'build','--parallel',a.jobs])
for file in ['libc++.dll','libunwind.dll']:shutil.copy2(clang/file,app/file)
if a.runtime:
    runtime=a.runtime.resolve()
    for name in ['models','profiles','licenses','prerequisites','plugins']:
        shutil.copytree(runtime/name,app/name,dirs_exist_ok=True)
    for name in ['config.yaml','models.lock.json','benchmark.ps1','README.md']:
        shutil.copy2(runtime/name,app/name)
    run([app/'ajan-tests.exe'])
    run([app/'YerelAjan.exe','--doctor'])
print('Built executables in '+str(app)+'. Models are supplied by the offline installer.',flush=True)
