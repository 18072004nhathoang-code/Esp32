"""Generate original 36px alpha icons; no font glyphs or external assets."""
from pathlib import Path
import math

def icon(kind):
    p = [0] * (36*36)
    def line(a,b,width=2):
        ax,ay=a; bx,by=b
        for y in range(36):
            for x in range(36):
                t=max(0,min(1,((x-ax)*(bx-ax)+(y-ay)*(by-ay))/max(1,(bx-ax)**2+(by-ay)**2)))
                if math.hypot(x-ax-t*(bx-ax),y-ay-t*(by-ay))<=width/2: p[y*36+x]=255
    def circle(cx,cy,r,width=2):
        for y in range(36):
            for x in range(36):
                if abs(math.hypot(x-cx,y-cy)-r)<=width/2:p[y*36+x]=255
    def path(points,width=2):
        for a,b in zip(points,points[1:]):line(a,b,width)
    if kind==1:
        path([(9,9),(27,9),(27,27),(9,27),(9,9)])
        for i in (12,18,24):
            for a,b in [((i,5),(i,9)),((i,27),(i,31)),((5,i),(9,i)),((27,i),(31,i))]:line(a,b)
        path([(14,14),(22,14),(22,22),(14,22),(14,14)])
    elif kind==3:
        for r in (8,15,22):
            for angle in range(228,313):
                t=math.radians(angle);x=round(18+r*math.cos(t));y=round(29+r*math.sin(t))
                line((x,y),(x,y),3)
        circle(18,29,1,3)
    elif kind==5:
        path([(6,9),(14,6),(23,10),(30,7),(30,27),(23,30),(14,26),(6,29),(6,9)])
        line((14,6),(14,26));line((23,10),(23,30))
        path([(10,22),(17,15),(23,20),(28,13)],3)
    elif kind==10:
        path([(5,12),(12,12),(14,8),(23,8),(25,12),(31,12),(31,28),(5,28),(5,12)])
        circle(18,20,6);circle(28,15,1)
    elif kind==11:
        for angle in range(-45,226):
            t=math.radians(angle);x=round(18+11*math.cos(t));y=round(19+11*math.sin(t));line((x,y),(x,y),3)
        line((18,5),(18,19),3)
    elif kind==8:
        path([(15,26),(15,10),(28,7),(28,24)],3);line((15,15),(28,12),3)
        circle(11,27,4,4);circle(24,25,4,4)
    elif kind==9:
        for x,h in ((6,4),(12,9),(18,14),(24,9),(30,4)):line((x,18-h),(x,18+h),3)
    else:
        circle(18,18,9,4);circle(18,18,3)
        for i in range(8):
            t=i*math.pi/4;line((18+10*math.cos(t),18+10*math.sin(t)),(18+14*math.cos(t),18+14*math.sin(t)),4)
    return p

def generate():
    out=['#pragma once','#include <lvgl.h>','namespace home_icons {']
    for k in (1,2,3,5,8,9,10,11):
        out.append('static const uint8_t pixels_%d[] = {%s};'%(k,','.join(map(str,icon(k)))))
    out.append('inline const lv_img_dsc_t *get(uintptr_t id) {')
    for k in (1,2,3,5,8,9,10,11):
        out.append('static const lv_img_dsc_t image_%d = [] { lv_img_dsc_t v = {}; v.header.cf=LV_IMG_CF_ALPHA_8BIT; v.header.w=36; v.header.h=36; v.data_size=1296; v.data=pixels_%d; return v; }();'%(k,k))
    out.append('switch(id) {')
    for k in (1,2,3,5,8,9,10,11):out.append('case %d: return &image_%d;'%(k,k))
    out.extend(['default: return &image_2;','}}','}'])
    return '\n'.join(out)+'\n'
if __name__=='__main__':
    path=Path(__file__).resolve().parents[1]/'src/ui/home_icons.h'
    import sys
    if '--check' in sys.argv:
        assert path.read_text()==generate(), 'Regenerate home icons'
    else: path.write_text(generate())
