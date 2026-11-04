"""Install into a new RK directory; keep the running RC2 product and its settings intact."""
from pathlib import Path
import hashlib,json,shutil
BASE=Path('/userdata/test_auto_drive/rk3568_original_switch_20261101_rc2')
TARGET=BASE.parent/'rk3568_startup_guard_20261102_rc3'
SERVER_SHA='56da4aee439863b93222ab2fade9e52a446bc139773b2d0cda108434e6221ca6'
BINARY_SHA='e32e34fd8b52ab595540f7fde626cca50d55552d1c4200374bcb402e6d249207'
IMPORT=b'from process_tree import descendants,wait_exited'
START=b"        self.drive=self.launch(command,'drive');self.update(drive_running=True)"
def replacements(newline):
    return (IMPORT+newline+b'from startup_planes import launch_once as launch_startup_planes',
            START+newline+b"        launch_startup_planes(self.root,self.drive,self.drive_started,self.stop,ROOT/'rk_signal_control',self.args.drive_args)")
def sha(data):return hashlib.sha256(data).hexdigest()
def patched_server(original):
    if sha(original)!=SERVER_SHA:raise RuntimeError('Unexpected RC2 server; preserve it and inspect before patching')
    if original.count(IMPORT)!=1 or original.count(START)!=1:raise RuntimeError('Unexpected RC2 supervisor structure')
    newline=b'\r\n' if b'\r\n' in original else b'\n'
    import_new,start_new=replacements(newline)
    return original.replace(IMPORT,import_new).replace(START,start_new)
def main():
    assert sha((BASE/'rk_signal_control').read_bytes())==BINARY_SHA
    binary=Path(__file__).resolve().parent/'rk_signal_control'
    assert sha(binary.read_bytes())=='817b33b72f8992d8017ac47449620cdd2ca750aab79a3c8a53b86399798c02d7'
    server=patched_server((BASE/'product/server.py').read_bytes())
    overlay=Path(__file__).resolve().parent/'product/startup_planes.py';assert overlay.is_file()
    TARGET.mkdir(exist_ok=False)
    for pattern in ['*.sh','camera_*.json','model_contract.json']:
        for p in BASE.glob(pattern):shutil.copy2(p,TARGET/p.name)
    for name in ['configs','scripts','all_player','product']:
        shutil.copytree(BASE/name,TARGET/name,ignore=shutil.ignore_patterns('__pycache__','*.pyc'))
    for name in ['lib','models','parking_model.rknn','player','data','video','rk_mode_hub','rk_panorama_view','rk_park_stream']:
        (TARGET/name).symlink_to(BASE/name)
    shutil.copy2(binary,TARGET/'rk_signal_control');(TARGET/'rk_signal_control').chmod(0o755)
    (TARGET/'product/server.py').write_bytes(server);shutil.copy2(overlay,TARGET/'product/startup_planes.py')
    state=TARGET/'results/product_runtime';state.mkdir(parents=True)
    restored={}
    for name in ['risk_profile.json','drive_near_rules.json','parking_rules.json']:
        prior=BASE/'results/product_runtime'/name
        if prior.is_file():shutil.copy2(prior,state/name);restored[name]=sha(prior.read_bytes())
    result=dict(target=str(TARGET),base=str(BASE),binary_sha256=sha(binary.read_bytes()),server_before_sha256=SERVER_SHA,
                server_after_sha256=sha(server),overlay_sha256=sha(overlay.read_bytes()),restored_original_product_parameters=restored,
                old_runtime_modified=False,models_libraries_videos_reused=True,old_tokens_and_runtime_logs_copied=False)
    (TARGET/'installation_startup_guard.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
if __name__=='__main__':main()
