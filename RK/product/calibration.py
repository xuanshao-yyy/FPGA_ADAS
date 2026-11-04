"""Validate normalized convex ROIs and optional measured ground points."""
import math,struct
def polygon(points,normalized=True):
    if not isinstance(points,list) or not 3<=len(points)<=12:raise ValueError('区域需要3至12个点')
    for p in points:
        if not isinstance(p,list) or len(p)!=2 or any(type(x) not in (int,float) or not math.isfinite(x) for x in p):raise ValueError('坐标无效')
        if normalized and any(x<0 or x>1 for x in p):raise ValueError('坐标需位于原图范围内')
    cross=[]
    for i,p in enumerate(points):
        q=points[(i+1)%len(points)];r=points[(i+2)%len(points)]
        cross.append((q[0]-p[0])*(r[1]-q[1])-(q[1]-p[1])*(r[0]-q[0]))
    area=abs(sum(p[0]*points[(i+1)%len(points)][1]-p[1]*points[(i+1)%len(points)][0] for i,p in enumerate(points)))/2
    if not (all(c>1e-8 for c in cross) or all(c<-1e-8 for c in cross)) or area<(.005 if normalized else .05):raise ValueError('请沿边缘依次点击凸四边形，不要交叉')
    return points
def validate(profile):
    if not isinstance(profile,dict) or not isinstance(profile.get('id'),str) or not 1<=len(profile['id'])<=80:raise ValueError('标定ID无效')
    if not isinstance(profile.get('channels'),list) or len(profile['channels'])!=4:raise ValueError('需要四路标定')
    source=profile.get('front_source','')
    if not isinstance(source,str) or len(source)>4096:raise ValueError('画面源标识无效')
    clean={'id':profile['id'],'front_source':source,'channels':[]}
    for value in profile['channels']:
        if not isinstance(value,dict):raise ValueError('通道标定格式无效')
        z={'verified':int(value.get('verified',0)==1),'metric_verified':int(value.get('metric_verified',0)==1),'near_polygon':polygon(value['near_polygon'])}
        for key,default,low,high in [('area_vehicle',.08,.005,.6),('area_small',.025,.002,.4),('near_m',3,.01,100),('stop_m',1.5,.01,100)]:
            x=value.get(key,default)
            if type(x) not in (int,float) or not math.isfinite(x) or not low<=x<=high:raise ValueError('阈值无效: '+key)
            z[key]=x
        if z['near_m']<=z['stop_m']:raise ValueError('慢行距离必须大于停止距离')
        if z['metric_verified']:
            z['ground_image']=polygon(value.get('ground_image'));z['ground_world']=polygon(value.get('ground_world'),False)
            if len(z['ground_image'])!=4 or len(z['ground_world'])!=4 or any(abs(p[0])>100 or not 0<=p[1]<=100 for p in z['ground_world']):raise ValueError('米制标定需要四个实测地面点')
        clean['channels'].append(z)
    return clean
def bmp565(raw):
    if len(raw)!=1280*720*2:raise ValueError('Snapshot incomplete')
    size=66+len(raw)
    return struct.pack('<2sIHHI',b'BM',size,0,0,66)+struct.pack('<IiiHHIIiiII',40,1280,-720,1,16,3,len(raw),2835,2835,0,0)+struct.pack('<III',0xf800,0x07e0,0x001f)+raw
