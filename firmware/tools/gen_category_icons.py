import math, sys

W=H=60; SS=4
GREY=(0x3A,0x3F,0x47); WHITE=(0xF2,0xF2,0xF2)

def rrect_inside(x,y,x0,y0,x1,y1,r):
    cx=(x0+x1)/2; cy=(y0+y1)/2; hw=(x1-x0)/2-r; hh=(y1-y0)/2-r
    qx=abs(x-cx)-hw; qy=abs(y-cy)-hh
    return (math.hypot(max(qx,0),max(qy,0))+min(max(qx,qy),0)-r)<=0
def disk(x,y,cx,cy,r): return (x-cx)**2+(y-cy)**2<=r*r
def segd(x,y,ax,ay,bx,by):
    dx,dy=bx-ax,by-ay; L=dx*dx+dy*dy
    t=0 if L==0 else max(0,min(1,((x-ax)*dx+(y-ay)*dy)/L))
    return math.hypot(x-(ax+t*dx),y-(ay+t*dy))
def cap(x,y,ax,ay,bx,by,r): return segd(x,y,ax,ay,bx,by)<=r
def tri(x,y,p0,p1,p2):
    def s(a,b,c): return (a[0]-c[0])*(b[1]-c[1])-(b[0]-c[0])*(a[1]-c[1])
    d1=s((x,y),p0,p1); d2=s((x,y),p1,p2); d3=s((x,y),p2,p0)
    neg=(d1<0)or(d2<0)or(d3<0); pos=(d1>0)or(d2>0)or(d3>0)
    return not(neg and pos)

def tile(x,y): return rrect_inside(x,y,0,0,60,60,13)

# ── glyphs: return True where WHITE ──
def g_email(x,y):
    body=rrect_inside(x,y,9,19,51,41,4)
    flap=(cap(x,y,9,20,30,33,1.6) or cap(x,y,51,20,30,33,1.6))
    return body and not flap
def g_schedule(x,y):
    tab=rrect_inside(x,y,18,11,23,19,1) or rrect_inside(x,y,37,11,42,19,1)
    body=rrect_inside(x,y,10,15,50,47,4)
    header=body and 22<=y<=25
    return (body or tab) and not header
def g_health(x,y):
    return disk(x,y,22,24,8) or disk(x,y,38,24,8) or tri(x,y,(14,26),(46,26),(30,47))
def g_location(x,y):
    out=disk(x,y,30,23,11) or tri(x,y,(20,29),(40,29),(30,49))
    return out and not disk(x,y,30,22,4.5)
def g_play(x,y):
    return tri(x,y,(23,17),(23,43),(45,30))
def g_finance(x,y):
    return (rrect_inside(x,y,15,33,23,45,1) or rrect_inside(x,y,26,24,34,45,1)
            or rrect_inside(x,y,37,15,45,45,1))
def g_news(x,y):
    body=rrect_inside(x,y,12,16,48,45,3)
    img=rrect_inside(x,y,16,21,27,31,1)
    lines=any(cap(x,y,30,yy,44,yy,1.0) for yy in (22,27)) or \
          any(cap(x,y,16,yy,44,yy,1.0) for yy in (35,39,43))
    return body and not (img or lines)
def g_social(x,y):
    body=rrect_inside(x,y,11,15,49,39,9) or tri(x,y,(18,36),(31,36),(17,48))
    dots=disk(x,y,22,27,2) or disk(x,y,30,27,2) or disk(x,y,38,27,2)
    return body and not dots
def g_call(x,y):
    grip=cap(x,y,21,21,39,39,4.5)
    ear=disk(x,y,20,20,7.5) and not disk(x,y,15,15,7.5)
    mouth=disk(x,y,40,40,7.5) and not disk(x,y,45,45,7.5)
    return grip or ear or mouth

GLYPHS=[("call",g_call),("social",g_social),("schedule",g_schedule),
        ("email",g_email),("news",g_news),("health",g_health),
        ("finance",g_finance),("location",g_location),("entertainment",g_play)]

def coverage(fn,px,py):
    th=tg=0
    for sy in range(SS):
        for sx in range(SS):
            x=px+(sx+0.5)/SS; y=py+(sy+0.5)/SS
            if tile(x,y):
                th+=1
                if fn(x,y): tg+=1
    return th/(SS*SS), (tg/th if th else 0)

def ascii_preview(name,fn):
    print(f"\n=== {name} ===")
    for py in range(0,60,2):
        row=""
        for px in range(0,60):
            tc,gc=coverage(fn,px,py)
            row += ("#" if gc>0.5 else ("." if tc>0.5 else " "))
        print(row)

def emit_c(name,fn):
    flat=[]
    for py in range(60):
        for px in range(60):
            tc,gc=coverage(fn,px,py)
            r=round(GREY[0]+(WHITE[0]-GREY[0])*gc); g=round(GREY[1]+(WHITE[1]-GREY[1])*gc)
            b=round(GREY[2]+(WHITE[2]-GREY[2])*gc); a=round(tc*255)
            flat+=[b,g,r,a]
    sym=f"cat_{name}"; up=sym.upper()
    L=["","#if defined(LV_LVGL_H_INCLUDE_SIMPLE)",'#include "lvgl.h"',
       "#elif defined(LV_LVGL_H_INCLUDE_SYSTEM)","#include <lvgl.h>",
       "#elif defined(LV_BUILD_TEST)",'#include "../lvgl.h"',"#else",
       '#include "lvgl/lvgl.h"',"#endif","",
       "#ifndef LV_ATTRIBUTE_MEM_ALIGN","#define LV_ATTRIBUTE_MEM_ALIGN","#endif","",
       f"#ifndef LV_ATTRIBUTE_{up}",f"#define LV_ATTRIBUTE_{up}","#endif","",
       "// Procedurally generated ANCS category fallback icon (white glyph on a",
       "// neutral tile). Regenerate via tools/gen_category_icons.py.","static const",
       f"LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST LV_ATTRIBUTE_{up}",
       f"uint8_t {sym}_map[] = {{"]
    per=60*4
    for i in range(0,len(flat),per):
        L.append("    "+",".join("0x%02x"%v for v in flat[i:i+per])+",")
    L+=["","};","",f"const lv_image_dsc_t {sym} = {{","  .header = {",
        "    .magic = LV_IMAGE_HEADER_MAGIC,","    .cf = LV_COLOR_FORMAT_ARGB8888,",
        "    .flags = 0,","    .w = 60,","    .h = 60,","    .stride = 240,",
        "    .reserved_2 = 0,","  },",f"  .data_size = sizeof({sym}_map),",
        f"  .data = {sym}_map,","  .reserved = NULL,","};",""]
    with open(f"src/assets/ancs_{name}.c","w") as f: f.write("\n".join(L))

mode = sys.argv[1] if len(sys.argv)>1 else "preview"
for name,fn in GLYPHS:
    if mode=="preview": ascii_preview(name,fn)
    else: emit_c(name,fn)
if mode!="preview": print("wrote %d category icon .c files"%len(GLYPHS))
