#include "camera_app/overlay.h"
#include "apcam/target.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

void ca_overlay_geometry(struct ca_overlay_geometry *g, unsigned width, unsigned height,
                         bool cross, bool thermal_box, float rgb_hfov)
{
    ca_overlay_geometry_fov(g,width,height,cross,thermal_box,rgb_hfov,
#if APCAM_HAVE_THERMAL
                            APCAM_LENS3_FOV_H, float(APCAM_LENS3_WIDTH)/APCAM_LENS3_HEIGHT
#else
                            0, 1
#endif
    );
}
void ca_overlay_geometry_fov(struct ca_overlay_geometry *g,unsigned width,unsigned height,
                             bool cross,bool thermal_box,float rgb_hfov,float thermal_hfov,float thermal_aspect)
{
    memset(g, 0, sizeof(*g));
    g->scale = height / 720.0f;
    if (g->scale < 1) g->scale = 1;
    int cx = width / 2, cy = height / 2;
    /* Scale arm geometry before rounding; 1080p must not truncate to 720p. */
    int inner = (int)lround(height * (5.0 / 720.0));
    int outer = (int)lround(height * (20.0 / 720.0));
    if (inner < 1) inner = 1;
    if (outer <= inner) outer = inner + 1;
    if (cross) {
        for (int y = -1; y <= 1; y += 2)
            for (int x = -1; x <= 1; x += 2)
                g->lines[g->count++] = (struct ca_overlay_line){
                    cx + x*inner, cy + y*inner, cx + x*outer, cy + y*outer, 0, false};
    }
    if (thermal_box && isfinite(rgb_hfov) && rgb_hfov > 0 && rgb_hfov < 180 &&
        isfinite(thermal_hfov) && thermal_hfov > 0 && thermal_hfov < 180 && thermal_aspect > 0) {
        const double rad = 0.01745329251994329577;
        double half_w = width * .5 * tan(thermal_hfov * rad * .5) / tan(rgb_hfov * rad * .5);
        double half_h = half_w / thermal_aspect;
        /* Once completely outside the view, there is no visible boundary.
         * Bound the coordinate conversion even for unreasonable calibration. */
        if (half_w > width && half_h > height) return;
        int x0 = (int)lround(cx-half_w), x1 = (int)lround(cx+half_w);
        int y0 = (int)lround(cy-half_h), y1 = (int)lround(cy+half_h);
        g->lines[g->count++] = (struct ca_overlay_line){x0,y0,x1,y0,1,true};
        g->lines[g->count++] = (struct ca_overlay_line){x0,y1,x1,y1,2,true};
        g->lines[g->count++] = (struct ca_overlay_line){x0,y0,x0,y1,3,true};
        g->lines[g->count++] = (struct ca_overlay_line){x1,y0,x1,y1,4,true};
    }
}

void ca_overlay_free(struct ca_overlay_bitmap out[CA_OVERLAY_REGIONS])
{
    for (unsigned i=0; i<CA_OVERLAY_REGIONS; i++) { free(out[i].pixels); out[i].pixels=NULL; }
}

void ca_overlay_tracking(struct ca_overlay_geometry *g, unsigned width, unsigned height,
                         const float rect[4])
{
    if(g->count>CA_OVERLAY_LINES-4) return;
    for(unsigned i=0;i<4;i++) if(!isfinite(rect[i])) return;
    const int x0=lroundf(fmaxf(0,fminf(1,rect[0]))*(width-1)),
        y0=lroundf(fmaxf(0,fminf(1,rect[1]))*(height-1)),
        x1=lroundf(fmaxf(0,fminf(1,rect[2]))*(width-1)),
        y1=lroundf(fmaxf(0,fminf(1,rect[3]))*(height-1));
    g->lines[g->count++]={x0,y0,x1,y0,5,false};
    g->lines[g->count++]={x0,y1,x1,y1,6,false};
    g->lines[g->count++]={x0,y0,x0,y1,7,false};
    g->lines[g->count++]={x1,y0,x1,y1,8,false};
}

static int minimum(int a,int b) { return a<b?a:b; }
static int maximum(int a,int b) { return a>b?a:b; }
static void dot(struct ca_overlay_bitmap *b, int x, int y, int radius, uint16_t color)
{
    for (int dy=-radius; dy<=radius; dy++)
        for (int dx=-radius; dx<=radius; dx++) {
            int px=x+dx-(int)b->x, py=y+dy-(int)b->y;
            if (px>=0 && py>=0 && px<(int)b->width && py<(int)b->height)
                b->pixels[py*b->width+px]=color;
        }
}
int ca_overlay_bitmaps(struct ca_overlay_bitmap out[CA_OVERLAY_REGIONS],
                      unsigned width, unsigned height, const struct ca_overlay_geometry *g)
{
    memset(out,0,sizeof(*out)*CA_OVERLAY_REGIONS);
    int radius=(int)lroundf(2*g->scale), core=(int)lroundf(g->scale)-1;
    int dash=(int)lroundf(12*g->scale);
    for (unsigned region=0; region<CA_OVERLAY_REGIONS; region++) {
        int x0=width,y0=height,x1=-1,y1=-1;
        for (unsigned i=0; i<g->count; i++) {
            const struct ca_overlay_line *l=&g->lines[i];
            if (l->region!=region) continue;
            x0=minimum(x0,minimum(l->x0,l->x1)-radius);
            y0=minimum(y0,minimum(l->y0,l->y1)-radius);
            x1=maximum(x1,maximum(l->x0,l->x1)+radius);
            y1=maximum(y1,maximum(l->y0,l->y1)+radius);
        }
        if (x1<0 || y1<0 || x0>=(int)width || y0>=(int)height) continue;
        x0=maximum(0,x0)&~7; y0=maximum(0,y0)&~1;
        x1=minimum(width, (x1+8)&~7); y1=minimum(height, (y1+2)&~1);
        if (x1<=x0 || y1<=y0) continue;
        struct ca_overlay_bitmap *b=&out[region];
        *b=(struct ca_overlay_bitmap){(unsigned int)(x0),(unsigned int)(y0),(unsigned int)(x1-x0),(unsigned int)(y1-y0),NULL};
        b->pixels=(uint16_t*)(calloc((size_t)b->width*b->height,sizeof(uint16_t)));
        if (!b->pixels) { ca_overlay_free(out); return -1; }
        for (unsigned pass=0; pass<2; pass++) {
            for (unsigned i=0; i<g->count; i++) {
                const struct ca_overlay_line *l=&g->lines[i];
                if (l->region!=region) continue;
                int n=maximum(abs(l->x1-l->x0),abs(l->y1-l->y0));
                for (int k=0; k<=n; k++) {
                    if (l->dashed && (k/dash)%2) continue;
                    int x=l->x0+(n ? (l->x1-l->x0)*k/n : 0);
                    int y=l->y0+(n ? (l->y1-l->y0)*k/n : 0);
                    dot(b,x,y,pass ? core : radius,pass ? (region>=5?0x821f:0xffff) : 0x8000);
                }
            }
        }
    }
    return 0;
}
