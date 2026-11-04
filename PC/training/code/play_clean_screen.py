"""Full-screen local video playback for camera tests; no on-screen controls."""
import argparse
from pathlib import Path
import time
import cv2

ROOT=Path(__file__).resolve().parents[1]


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source',type=Path,default=ROOT/'runs/single_screen_simulation_20261009/camera_load_4slots_10s.mp4')
    p.add_argument('--once',action='store_true',help='Stop at the end rather than loop')
    p.add_argument('--start-paused',action='store_true',help='Show frame 0 until SPACE; useful for camera alignment')
    p.add_argument('--windowed',action='store_true',help='Debug window; do not use for camera input tests')
    args=p.parse_args()
    cap=cv2.VideoCapture(str(args.source));fps=cap.get(cv2.CAP_PROP_FPS)
    if not cap.isOpened() or fps<=0:raise ValueError(f'Cannot read video: {args.source}')
    title='Clean camera input';cv2.namedWindow(title,cv2.WINDOW_NORMAL)
    cv2.setWindowProperty(title,cv2.WND_PROP_ASPECT_RATIO,cv2.WINDOW_KEEPRATIO)
    if not args.windowed:cv2.setWindowProperty(title,cv2.WND_PROP_FULLSCREEN,cv2.WINDOW_FULLSCREEN)
    ok,image=cap.read()
    if not ok:raise ValueError('Empty video')
    frame_id=0;paused=args.start_paused;deadline=time.perf_counter()+1/fps;display_changed=True
    print('SPACE: pause/resume; R: restart from frame 0 and pause; ESC: exit. Move mouse outside captured regions.')
    try:
        while True:
            if display_changed:
                cv2.imshow(title,image);display_changed=False
            key=cv2.waitKey(1)&255
            if key==27 or cv2.getWindowProperty(title,cv2.WND_PROP_VISIBLE)<1:break
            if key==32:
                paused=not paused;deadline=time.perf_counter()+1/fps
                print(f'paused={paused}, source_frame={frame_id}, source_time_s={frame_id/fps:.3f}')
            elif key in (ord('r'),ord('R')):
                cap.set(cv2.CAP_PROP_POS_FRAMES,0);ok,image=cap.read()
                if not ok:raise RuntimeError('Restart failed')
                frame_id=0;paused=True;display_changed=True;print('Restarted at source frame 0, paused.')
            if paused:
                time.sleep(.005);continue
            now=time.perf_counter()
            if now<deadline:
                time.sleep(min(.005,deadline-now));continue
            ok,next_image=cap.read()
            if not ok:
                if args.once:break
                cap.set(cv2.CAP_PROP_POS_FRAMES,0);ok,next_image=cap.read();frame_id=-1
                if not ok:raise RuntimeError('Loop restart failed')
                print('Loop restarted; reset recognition state between independent tests.')
            image=next_image;display_changed=True;frame_id+=1;deadline+=1/fps
            if now-deadline>1/fps:
                deadline=now+1/fps
                # Slow display is reported, never disguised as source-rate acquisition.
                print(f'Playback late at frame {frame_id}; recorded speed test needs actual source pacing.')
    finally:
        cap.release();cv2.destroyAllWindows()


if __name__=='__main__':main()
