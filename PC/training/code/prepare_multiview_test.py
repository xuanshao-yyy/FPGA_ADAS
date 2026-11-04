"""Export raw nuScenes camera frames as timestamp-aligned local test videos."""
import argparse
from bisect import bisect_left
from collections import defaultdict
import hashlib
import json
import math
from pathlib import Path
import cv2
import numpy as np

ROOT=Path(__file__).resolve().parents[1]
CHANNELS=('CAM_FRONT','CAM_FRONT_LEFT','CAM_FRONT_RIGHT','CAM_BACK_LEFT','CAM_BACK_RIGHT','CAM_BACK')
BOARD_CHANNELS=('CAM_FRONT','CAM_BACK_LEFT','CAM_BACK_RIGHT','CAM_BACK')
REGIONS=((0,180,1280,720),(1280,0,640,360),(1280,360,640,360),(1280,720,640,360))


def read_table(root,name):
    return json.loads((root/'v1.0-mini'/f'{name}.json').read_text(encoding='utf-8'))


def sha256(path):
    digest=hashlib.sha256()
    with path.open('rb') as source:
        for chunk in iter(lambda:source.read(1024*1024),b''):digest.update(chunk)
    return digest.hexdigest()


def camera_heading(rotation):
    # Camera optical axis is local +z; ego axes are +x forward, +y left.
    w,x,y,z=rotation
    forward_x=2*(x*z+w*y);forward_y=2*(y*z-w*x)
    return math.degrees(math.atan2(forward_y,forward_x))


def nearest_index(timestamps,target):
    index=bisect_left(timestamps,target)
    if index==0:return 0
    if index==len(timestamps):return index-1
    return index if timestamps[index]-target<target-timestamps[index-1] else index-1


def validate_video(path,count,size,fps):
    cap=cv2.VideoCapture(str(path));decoded=0
    if not cap.isOpened() or abs(cap.get(cv2.CAP_PROP_FPS)-fps)>.01:raise ValueError(f'Invalid encoded video: {path}')
    try:
        while True:
            ok,image=cap.read()
            if not ok:break
            if image.shape[:2]!=(size[1],size[0]):raise ValueError('Encoded frame dimensions differ')
            decoded+=1
    finally:cap.release()
    if decoded!=count:raise ValueError(f'Incomplete encoded video: {path}, {decoded}/{count}')
    return {'path':str(path),'bytes':path.stat().st_size,'sha256':sha256(path),'decoded_frames':decoded,'size':size,'fps':fps}


def export_scene(root,out,scene,groups,calibrations,fps):
    folder=out/scene['name'];folder.mkdir()
    channels={name:sorted(groups[(scene['token'],name)],key=lambda row:row['timestamp']) for name in CHANNELS}
    if any(not rows for rows in channels.values()):raise ValueError('Scene missing camera channel')
    times={name:[row['timestamp'] for row in rows] for name,rows in channels.items()}
    start=max(t[0] for t in times.values());end=min(t[-1] for t in times.values())
    count=math.floor((end-start)*fps/1e6)+1
    if count<2:raise ValueError('Camera streams have insufficient common coverage')
    writers={};board=None;records=[];previous={};repeats=defaultdict(int);max_error=defaultdict(int);max_span=0;max_board_span=0;board_over_100ms=0
    try:
        for name in CHANNELS:
            writers[name]=cv2.VideoWriter(str(folder/f'{name}.mp4'),cv2.VideoWriter_fourcc(*'mp4v'),fps,(1280,720))
            if not writers[name].isOpened():raise RuntimeError('Video encoder unavailable')
        board=cv2.VideoWriter(str(folder/'front_sideRear_back_4slots.mp4'),cv2.VideoWriter_fourcc(*'mp4v'),fps,(1920,1080))
        if not board.isOpened():raise RuntimeError('Board video encoder unavailable')
        for frame in range(count):
            target=start+round(frame*1e6/fps);images={};chosen={}
            for name in CHANNELS:
                row=channels[name][nearest_index(times[name],target)]
                error=row['timestamp']-target;max_error[name]=max(max_error[name],abs(error))
                if previous.get(name)==row['token']:repeats[name]+=1
                previous[name]=row['token']
                source=(root/row['filename']).resolve()
                if not source.is_relative_to(root.resolve()):raise ValueError('Source path outside dataset root')
                image=cv2.imread(str(source))
                if image is None:raise FileNotFoundError(source)
                if image.shape[1]*9!=image.shape[0]*16:raise ValueError('Unexpected camera aspect ratio; no stretching allowed')
                images[name]=cv2.resize(image,(1280,720),interpolation=cv2.INTER_AREA)
                writers[name].write(images[name])
                chosen[name]={'token':row['token'],'filename':row['filename'],'timestamp_us':row['timestamp'],'offset_us':error}
            span=max(x['timestamp_us'] for x in chosen.values())-min(x['timestamp_us'] for x in chosen.values());max_span=max(max_span,span)
            board_times=[chosen[name]['timestamp_us'] for name in BOARD_CHANNELS]
            board_span=max(board_times)-min(board_times);max_board_span=max(max_board_span,board_span)
            if board_span>100000:board_over_100ms+=1
            canvas=np.zeros((1080,1920,3),np.uint8)
            for name,(x,y,w,h) in zip(BOARD_CHANNELS,REGIONS):canvas[y:y+h,x:x+w]=cv2.resize(images[name],(w,h),interpolation=cv2.INTER_AREA)
            board.write(canvas);records.append({'frame':frame,'target_timestamp_us':target,'camera_span_us':span,'board_camera_span_us':board_span,'sources':chosen})
            if frame in {0,count//2,count-1}:
                if not cv2.imwrite(str(folder/f'layout_{frame:04d}.jpg'),canvas):raise OSError('Preview write failed')
                for name in CHANNELS:
                    if not cv2.imwrite(str(folder/f'{name}_{frame:04d}.jpg'),images[name]):raise OSError('Camera preview write failed')
            if frame%60==0:print(f"{scene['name']}: {frame}/{count} frames",flush=True)
    finally:
        for writer in writers.values():writer.release()
        if board is not None:board.release()
    (folder/'frame_timestamps.json').write_text(json.dumps(records,indent=2)+'\n',encoding='utf-8')
    headings={name:camera_heading(calibrations[rows[0]['calibrated_sensor_token']]['rotation']) for name,rows in channels.items()}
    stats={'scene':scene['name'],'scene_token':scene['token'],'description':scene['description'],'source_location':scene['source_location'],'output_frames':count,'output_fps':fps,
           'duration_s':count/fps,'start_timestamp_us':start,'end_timestamp_us':end,'camera_heading_degrees_left_positive':headings,
           'max_target_offset_us':dict(max_error),'max_pairwise_camera_span_us':max_span,'max_board_camera_span_us':max_board_span,
           'board_frames_over_100ms':board_over_100ms,'100ms_is_diagnostic_not_safety_acceptance_threshold':True,
           'reused_source_frames':{n:repeats[n] for n in CHANNELS},
           'board_channels':BOARD_CHANNELS,'board_regions_xywh':REGIONS,'exact_sensor_synchronization':False,
           'raw_images_no_boxes':True,'pixel_labels_or_safety_labels_exported':False,
           'timing_note':'Nearest recorded frame on a uniform 12Hz export clock; no motion interpolation. Camera offsets and repeats retained. Output encoding FPS is not board recognition FPS.'}
    stats['videos']=[validate_video(folder/f'{name}.mp4',count,(1280,720),fps) for name in CHANNELS]
    stats['videos'].append(validate_video(folder/'front_sideRear_back_4slots.mp4',count,(1920,1080),fps))
    (folder/'manifest.json').write_text(json.dumps(stats,indent=2)+'\n',encoding='utf-8')
    print(f"Verified {scene['name']}: {count} frames in each of 7 videos",flush=True)
    return stats


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dataset',type=Path,default=ROOT/'datasets/nuscenes_mini_v1')
    parser.add_argument('--output',type=Path,default=ROOT/'runs/multiview_test_data_20261009')
    parser.add_argument('--scenes',nargs='+',default=['scene-0061','scene-0796','scene-1077'])
    args=parser.parse_args();root=args.dataset.resolve();out=args.output.resolve()
    scenes=read_table(root,'scene');sample={row['token']:row for row in read_table(root,'sample')}
    sensors={row['token']:row for row in read_table(root,'sensor')};cal={row['token']:row for row in read_table(root,'calibrated_sensor')}
    groups=defaultdict(list)
    for row in read_table(root,'sample_data'):
        sensor=sensors[cal[row['calibrated_sensor_token']]['sensor_token']]
        if sensor['modality']=='camera':groups[(sample[row['sample_token']]['scene_token'],sensor['channel'])].append(row)
    logs={row['token']:row for row in read_table(root,'log')}
    selected={scene['name']:{**scene,'source_location':logs[scene['log_token']]['location']} for scene in scenes}
    for name in args.scenes:
        if name not in selected:raise ValueError(f'Unknown scene: {name}')
    if out.exists():raise FileExistsError(f'Preserving existing output: {out}')
    out.mkdir(parents=True);cv2.setNumThreads(2)
    summaries=[export_scene(root,out,selected[name],groups,cal,12) for name in args.scenes]
    receipt={'dataset_root':str(root),'output_root':str(out),'scenes':summaries,
             'source_url':'https://www.nuscenes.org/data/v1.0-mini.tgz',
             'license_terms_url':'https://www.nuscenes.org/terms-of-use','no_model_or_board_code_changed':True}
    (ROOT/'maintenance/multiview_test_data_20261009/export_receipt.json').write_text(json.dumps(receipt,indent=2)+'\n',encoding='utf-8')


if __name__=='__main__':main()
