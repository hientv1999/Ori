import math

W=H=60
SS=4  # supersample factor per axis

# Colors (R,G,B)
GREY=(0x3A,0x3F,0x47)
WHITE=(0xF2,0xF2,0xF2)

def rrect_inside(x,y,half=30.0,r=13.0):
    # signed distance <=0 inside, for rounded square centered at (30,30)
    qx=abs(x-30.0)-(half-r)
    qy=abs(y-30.0)-(half-r)
    ax=max(qx,0.0); ay=max(qy,0.0)
    outside=math.hypot(ax,ay)+min(max(qx,qy),0.0)-r
    return outside<=0.0

def circle(x,y,cx,cy,rad):
    return (x-cx)**2+(y-cy)**2 <= rad*rad

def bell_inside(x,y):
    # top knob
    if circle(x,y,30,14,3.0): return True
    # dome (rounded top)
    if circle(x,y,30,26,12.0) and y<=26: return True
    # body: flaring trapezoid
    if 26.0<=y<=43.0:
        half=12.0+(y-26.0)/(43.0-26.0)*5.0
        if abs(x-30.0)<=half: return True
    # rim
    if 43.0<=y<=47.0 and abs(x-30.0)<=17.0: return True
    # clapper
    if circle(x,y,30,50,3.2): return True
    return False

def lerp(a,b,t): return a+(b-a)*t

out=[]
for py in range(H):
    for px in range(W):
        tile_hits=0; bell_hits=0; n=SS*SS
        for sy in range(SS):
            for sx in range(SS):
                x=px+(sx+0.5)/SS
                y=py+(sy+0.5)/SS
                ti=rrect_inside(x,y)
                if ti: tile_hits+=1
                if ti and bell_inside(x,y): bell_hits+=1
        tcov=tile_hits/n
        bcov=bell_hits/n if tile_hits else 0.0
        bf=(bcov/tcov) if tcov>0 else 0.0
        r=int(round(lerp(GREY[0],WHITE[0],bf)))
        g=int(round(lerp(GREY[1],WHITE[1],bf)))
        b=int(round(lerp(GREY[2],WHITE[2],bf)))
        a=int(round(tcov*255))
        out.append((b,g,r,a))  # ARGB8888 little-endian byte order: B,G,R,A

# emit C
lines=[]
lines.append("")
lines.append("#if defined(LV_LVGL_H_INCLUDE_SIMPLE)")
lines.append('#include "lvgl.h"')
lines.append("#elif defined(LV_LVGL_H_INCLUDE_SYSTEM)")
lines.append("#include <lvgl.h>")
lines.append("#elif defined(LV_BUILD_TEST)")
lines.append('#include "../lvgl.h"')
lines.append("#else")
lines.append('#include "lvgl/lvgl.h"')
lines.append("#endif")
lines.append("")
lines.append("#ifndef LV_ATTRIBUTE_MEM_ALIGN")
lines.append("#define LV_ATTRIBUTE_MEM_ALIGN")
lines.append("#endif")
lines.append("")
lines.append("#ifndef LV_ATTRIBUTE_UNKNOWN_APP")
lines.append("#define LV_ATTRIBUTE_UNKNOWN_APP")
lines.append("#endif")
lines.append("")
lines.append("// Procedurally generated generic-app fallback icon (bell on a neutral")
lines.append("// rounded tile). Shown for ANCS notifications from apps with no brand")
lines.append("// asset. 60x60 ARGB8888, matching the other ancs_*.c assets.")
lines.append("// Regenerate via tools/gen_unknown_icon.py.")
lines.append("static const")
lines.append("LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST LV_ATTRIBUTE_UNKNOWN_APP")
lines.append("uint8_t unknown_app_map[] = {")
row=[]
flat=[]
for (b,g,r,a) in out:
    flat += [b,g,r,a]
per=60*4  # one image row per text line (matches stride)
for i in range(0,len(flat),per):
    chunk=flat[i:i+per]
    lines.append("    "+",".join("0x%02x"%v for v in chunk)+",")
lines.append("")
lines.append("};")
lines.append("")
lines.append("const lv_image_dsc_t unknown_app = {")
lines.append("  .header = {")
lines.append("    .magic = LV_IMAGE_HEADER_MAGIC,")
lines.append("    .cf = LV_COLOR_FORMAT_ARGB8888,")
lines.append("    .flags = 0,")
lines.append("    .w = 60,")
lines.append("    .h = 60,")
lines.append("    .stride = 240,")
lines.append("    .reserved_2 = 0,")
lines.append("  },")
lines.append("  .data_size = sizeof(unknown_app_map),")
lines.append("  .data = unknown_app_map,")
lines.append("  .reserved = NULL,")
lines.append("};")
lines.append("")

with open("src/assets/ancs_unknown.c","w") as f:
    f.write("\n".join(lines))
print("wrote src/assets/ancs_unknown.c bytes=",len(flat))
