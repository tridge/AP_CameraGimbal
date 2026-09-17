#include "camera_app/overlay.h"
#include "apcam/target.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

void ca_overlay_geometry(struct ca_overlay_geometry *g, unsigned width, unsigned height,
                         bool cross, bool thermal_box, float rgb_hfov)
{
    memset(g, 0, sizeof(*g));
    g->scale = height / 720;
    if (!g->scale) g->scale = 1;
    int cx = width / 2, cy = height / 2, s = g->scale;
    if (cross) {
        for (int y = -1; y <= 1; y += 2)
            for (int x = -1; x <= 1; x += 2)
                g->lines[g->count++] = (struct ca_overlay_line){
                    cx + x*5*s, cy + y*5*s, cx + x*20*s, cy + y*20*s, 0, false};
    }
#if APCAM_HAVE_THERMAL
    if (thermal_box && isfinite(rgb_hfov) && rgb_hfov > 0 && rgb_hfov < 180) {
        const double rad = 0.01745329251994329577;
        double half_w = width * .5 * tan(APCAM_LENS3_FOV_H * rad * .5) / tan(rgb_hfov * rad * .5);
        double half_h = half_w * APCAM_LENS3_HEIGHT / APCAM_LENS3_WIDTH;
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
#else
    (void)thermal_box; (void)rgb_hfov;
#endif
}

void ca_overlay_free(struct ca_overlay_bitmap out[CA_OVERLAY_REGIONS])
{
    for (unsigned i=0; i<CA_OVERLAY_REGIONS; i++) { free(out[i].pixels); out[i].pixels=NULL; }
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
    for (unsigned region=0; region<CA_OVERLAY_REGIONS; region++) {
        int x0=width,y0=height,x1=-1,y1=-1, radius=2*g->scale;
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
        *b=(struct ca_overlay_bitmap){x0,y0,x1-x0,y1-y0,NULL};
        b->pixels=calloc((size_t)b->width*b->height,sizeof(uint16_t));
        if (!b->pixels) { ca_overlay_free(out); return -1; }
        for (unsigned pass=0; pass<2; pass++) {
            for (unsigned i=0; i<g->count; i++) {
                const struct ca_overlay_line *l=&g->lines[i];
                if (l->region!=region) continue;
                int n=maximum(abs(l->x1-l->x0),abs(l->y1-l->y0));
                for (int k=0; k<=n; k++) {
                    if (l->dashed && (k/(12*g->scale))%2) continue;
                    int x=l->x0+(n ? (l->x1-l->x0)*k/n : 0);
                    int y=l->y0+(n ? (l->y1-l->y0)*k/n : 0);
                    dot(b,x,y,pass ? (int)g->scale-1 : radius,pass ? 0xffff : 0x8000);
                }
            }
        }
    }
    return 0;
}
