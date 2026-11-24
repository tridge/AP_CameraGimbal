#include "camera_app/survey.h"
#include "camera_app/targeting.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static constexpr double R = 6378137.0, rad = 0.017453292519943295;
static double wrap(double a) { return remainder(a, 2*M_PI); }

bool ca_survey_path::update(const ca_survey_pose &p, float bearing, unsigned distance,
                            float error, uint64_t now)
{
    // Integer bearing/range and asynchronous telemetry are ill-conditioned
    // near the endpoint or abeam it. Do not clamp impossible geometry into a leg.
    if (!isfinite(bearing) || fabs(bearing)>180 || !isfinite(error) ||
        distance<50 || distance>=UINT16_MAX || fabs(error)>.8*distance ||
        !isfinite(p.vn) || !isfinite(p.ve) || hypot(p.vn,p.ve)<1) {
        updated_ms=stable_ms=0;
        return false;
    }
    const double direction=wrap(bearing*rad-asin(error/distance));
    const double motion=atan2(p.ve,p.vn);
    // Select the forward solution, rejecting passage beyond the waypoint and
    // large intercepts. Actual turning is checked separately by the scheduler.
    if (fabs(wrap(direction-motion))>60*rad || fabs(wrap(bearing*rad-motion))>80*rad) {
        updated_ms=stable_ms=0;
        return false;
    }
    ca_survey_pose point=p;
    if (!ca_targeting_predict_position(&point.lat,&point.lon,&point.alt,
            -error*sin(direction),error*cos(direction),0,1)) {
        updated_ms=stable_ms=0;
        return false;
    }
    // A waypoint replacement/jump can retain its sequence number. A range jump
    // larger than possible aircraft motion must not blend two different legs.
    const bool fresh=updated_ms && now>=updated_ms && now-updated_ms<=1500 &&
        fabs(double(distance)-last_distance)<=20+2*hypot(p.vn,p.ve)*(now-updated_ms)*.001;
    ca_survey_pose previous{};
    if (fresh && fabs(wrap(direction-course))<15*rad) {
        // Transport the old line to this sample before blending; averaging
        // moving aircraft positions would introduce an along-track lag.
        const double north=(double(point.lat)-anchor.lat)*1e-7*rad*R;
        const double east=remainder((double(point.lon)-anchor.lon)*1e-7,360.0)*rad*R*cos(point.lat*1e-7*rad);
        const double cross=-north*sin(course)+east*cos(course);
        const double dt=(now-updated_ms)*.001;
        const double alpha=dt/(1+dt);
        previous=point;
        if (fabs(cross)<100 && ca_targeting_predict_position(&previous.lat,&previous.lon,&previous.alt,
                (1-alpha)*cross*sin(course),-(1-alpha)*cross*cos(course),0,1)) {
            point=previous;
            course=wrap(course+alpha*wrap(direction-course));
        } else {
            stable_ms=now;
            course=direction;
        }
    } else {
        stable_ms=now;
        course=direction;
    }
    anchor=point;
    updated_ms=now;
    last_distance=distance;
    return true;
}

bool ca_survey_path::project(const ca_survey_pose &p, uint64_t now, ca_survey_pose &out) const
{
    if (!updated_ms || now<updated_ms || now-updated_ms>1500 ||
        now<stable_ms || now-stable_ms<1000) return false;
    const double north=(double(p.lat)-anchor.lat)*1e-7*rad*R;
    const double east=remainder((double(p.lon)-anchor.lon)*1e-7,360.0)*rad*R*cos(p.lat*1e-7*rad);
    const double cross=-north*sin(course)+east*cos(course);
    const double speed=p.vn*cos(course)+p.ve*sin(course);
    if (!isfinite(speed) || speed<1 || fabs(wrap(atan2(p.ve,p.vn)-course))>60*rad) return false;
    out=p;
    out.vn=speed*cos(course); out.ve=speed*sin(course);
    return ca_targeting_predict_position(&out.lat,&out.lon,&out.alt,
        cross*sin(course),-cross*cos(course),0,1);
}

static int view_row(unsigned view, int fore_rows, int aft_rows)
{
    // IDs 0..8 retain the nine-view grid; 9..11 are far/middle/near forward.
    if(view>=9) return (12-int(view))*fore_rows;
    return view<3?fore_rows:view<6?0:-aft_rows;
}

static int view_column(unsigned view)
{
    const int columns[9]={-1,0,1,1,0,-1,-1,0,1};
    return view<9?columns[view]:0;
}

bool ca_survey_grid::init(const ca_survey_pose &p, const ca_survey_config &c,
                          float hfov, float aspect, float rate, uint64_t now)
{
    height = p.alt-p.ground;
    speed = hypot(p.vn,p.ve);
    if (!isfinite(height) || height < 5 || speed < c.min_speed ||
        !isfinite(hfov) || hfov <= 0 || hfov >= 150 || aspect <= 0 || rate <= 0) return false;
    slot_count=ca_survey_positions(c.pattern);
    if(!slot_count) return false;
    for(unsigned k=0;k<slot_count;k++) slots[k]=k;
    if(c.pattern==CA_SURVEY_LEFT_RIGHT) { slots[0]=5; slots[1]=3; }
    if(c.pattern==CA_SURVEY_FORE_AFT) { slots[0]=1; slots[1]=7; }
    if(c.pattern==CA_SURVEY_FORE_ONLY) { slots[0]=9; slots[1]=10; slots[2]=11; }
    if(c.pattern==CA_SURVEY_FORE_WIDE) { slots[0]=0; slots[1]=1; slots[2]=2; }
    origin=p; course=atan2(p.ve,p.vn); start_ms=now;
    const double width=2*height*tan(hfov*rad/2), length=width/aspect;
    const double overlap=c.overlap*.01;
    cross_spacing=fmin(width,length)*(1-overlap);
    spacing=length*(1-overlap);
    // Budget only selected views, including the last-to-first slew and
    // acceleration to/from the configured rate limit.
    for (unsigned pass=0;pass<4;pass++) {
        fore_rows=fmax(1,round(c.fore_pct*.01*height/spacing));
        aft_rows=fmax(1,round(c.aft_pct*.01*height/spacing));
        double yaw[9],pitch[9];
        for (unsigned k=0;k<slot_count;k++) {
            unsigned view=slots[k];
            double x=view_row(view,fore_rows,aft_rows)*spacing;
            double y=view_column(view)*cross_spacing;
            yaw[k]=(x==0 && y==0)?0:atan2(y,x);
            pitch[k]=atan2(-height,hypot(x,y));
        }
        cycle_s=0;
        for (unsigned k=0;k<slot_count;k++) {
            unsigned prev=(k+slot_count-1)%slot_count;
            double slew=fmax(fabs(wrap(yaw[k]-yaw[prev])),fabs(pitch[k]-pitch[prev]))/rad/rate;
            budget[k]=slew+1.0+c.dwell_ms*.001+(c.burst-1)*c.burst_ms*.001;
            cycle_s+=budget[k];
        }
        spacing=fmax(length*(1-overlap),cycle_s*speed);
    }
    nominal_overlap=100*(1-spacing/length); // negative means a nominal gap
    return true;
}

void ca_survey_grid::target(int64_t cycle, unsigned slot, int32_t &lat,
                            int32_t &lon, int64_t &row, int &column) const
{
    slot=slots[slot];
    row=cycle+view_row(slot,fore_rows,aft_rows);
    column=view_column(slot);
    const double x=row*spacing,y=column*cross_spacing;
    const float north=x*cos(course)-y*sin(course), east=x*sin(course)+y*cos(course);
    lat=origin.lat; lon=origin.lon; float alt=origin.ground;
    (void)ca_targeting_predict_position(&lat,&lon,&alt,north,east,0,1);
}

// Aim relative to where the aircraft will be at capture instead of a shared
// ground row. lead_m is the expected along-track travel before capture. From
// there, fore views look fore_m ahead, rear views aft_m behind, down views
// straight below, and forward pitch views 3x/2x/1x fore_m ahead.
void ca_survey_grid::target_ahead(const ca_survey_pose &p, double lead_m, double fore_m,
                                  double aft_m, unsigned slot, int32_t &lat, int32_t &lon,
                                  int64_t &row, int &column) const
{
    slot=slots[slot];
    column=view_column(slot);
    const int step=view_row(slot,1,1); // +1 fore, 0 down, -1 rear; 3/2/1 forward pitch
    const double x=along(p)+lead_m+(step>0?step*fore_m:step*aft_m), y=column*cross_spacing;
    row=int64_t(floor(x/spacing+.5)); // nearest row, for metadata only
    const float north=x*cos(course)-y*sin(course), east=x*sin(course)+y*cos(course);
    lat=origin.lat; lon=origin.lon; float alt=origin.ground;
    (void)ca_targeting_predict_position(&lat,&lon,&alt,north,east,0,1);
}

double ca_survey_grid::along(const ca_survey_pose &p) const
{
    double north=(double(p.lat)-origin.lat)*1e-7*rad*R;
    double east=remainder((double(p.lon)-origin.lon)*1e-7,360.0)*rad*R*cos(origin.lat*1e-7*rad);
    return north*cos(course)+east*sin(course);
}

double ca_survey_grid::cross_track(const ca_survey_pose &p) const
{
    double north=(double(p.lat)-origin.lat)*1e-7*rad*R;
    double east=remainder((double(p.lon)-origin.lon)*1e-7,360.0)*rad*R*cos(origin.lat*1e-7*rad);
    return -north*sin(course)+east*cos(course);
}

double ca_survey_grid::alignment_error(const ca_survey_pose &p) const
{
    // Include the centreline at the furthest selected view, so even a small
    // course change cannot send long forward views outside the flight swath.
    // Intentional left/right columns are not alignment errors.
    double reach=0;
    for(unsigned k=0;k<slot_count;k++)
        reach=fmax(reach,fabs(view_row(slots[k],fore_rows,aft_rows)*spacing));
    double delta=wrap(atan2(p.ve,p.vn)-course);
    return fabs(cross_track(p))+(reach+spacing/2)*fabs(sin(delta));
}

double ca_survey_grid::alignment_limit(const ca_survey_pose &p) const
{
    // Half a cross-grid interval reserves overlap for tracking variations.
    // Scale for current AGL, with a 5m floor to avoid reacting to GPS noise.
    return fmax(5.0,cross_spacing*.5*(p.alt-p.ground)/(origin.alt-origin.ground));
}

uint64_t ca_survey_grid::deadline(uint64_t start, unsigned slot) const
{
    double seconds=0;
    for(unsigned k=0;k<=slot && k<slot_count;k++) seconds+=budget[k];
    return start+uint64_t(seconds*1000);
}

bool ca_survey_footprint(const ca_metadata &m, float ground, float hfov,
                         float aspect, double points[4][2])
{
    if (!m.have_position || !m.have_vehicle_attitude || !m.have_gimbal_attitude ||
        !isfinite(ground) || m.alt_amsl_m<=ground || !isfinite(hfov) || hfov<=0 || hfov>=150 || aspect<=0) return false;
    double yaw=m.vehicle_yaw_rad+m.gimbal_yaw_rad, pitch=m.gimbal_pitch_rad, roll=m.gimbal_roll_rad;
    if(!isfinite(yaw)||!isfinite(pitch)||!isfinite(roll)) return false;
    double cp=cos(pitch),sp=sin(pitch),cy=cos(yaw),sy=sin(yaw),cr=cos(roll),sr=sin(roll);
    const double h=tan(hfov*rad/2),v=h/aspect;
    const int xs[4]={-1,1,1,-1},ys[4]={-1,-1,1,1};
    for(unsigned i=0;i<4;i++) {
        // Camera x points along the boresight, y right, z down. Rotate to NED.
        double right=cr*xs[i]*h-sr*ys[i]*v,down=sr*xs[i]*h+cr*ys[i]*v;
        double n=cy*(cp+sp*down)-sy*right;
        double e=sy*(cp+sp*down)+cy*right;
        double d=-sp+cp*down;
        if(d<.02) return false;
        double scale=(m.alt_amsl_m-ground)/d;
        if(hypot(n,e)*scale>20000) return false;
        int32_t lat=m.lat_e7,lon=m.lon_e7; float alt=m.alt_amsl_m;
        if(!ca_targeting_predict_position(&lat,&lon,&alt,n*scale,e*scale,0,1)) return false;
        points[i][0]=lat*1e-7;points[i][1]=lon*1e-7;
    }
    return true;
}

bool ca_survey_json(const ca_survey_result &r, char *out, unsigned size)
{
    double p[4][2];
    bool valid=ca_survey_footprint(r.pose,r.request.target_alt,r.request.hfov,r.request.aspect,p);
    char footprint[256]="null";
    if(valid) snprintf(footprint,sizeof(footprint),"[[%.7f,%.7f],[%.7f,%.7f],[%.7f,%.7f],[%.7f,%.7f]]",
        p[0][0],p[0][1],p[1][0],p[1][1],p[2][0],p[2][1],p[3][0],p[3][1]);
    const auto &q=r.request;
    int n=snprintf(out,size,
        "{\"schema\":\"apcg.survey.v1\",\"session\":\"%016llx\",\"image_index\":%d,"
        "\"leg\":%u,\"cycle\":%lld,\"row\":%lld,\"column\":%d,\"slot\":%u,\"burst\":%u,\"lens\":%u,\"pattern\":%u,\"position\":%u,\"positions\":%u,"
        "\"capture_monotonic_ms\":%llu,\"time_utc_us\":%llu,\"timestamp_source\":\"frame_receive\","
        "\"latency_unverified\":true,\"lat\":%.7f,\"lon\":%.7f,\"alt_amsl_m\":%.3f,"
        "\"ground_amsl_m\":%.3f,\"target\":[%.7f,%.7f,%.3f],\"hfov_deg\":%.4f,"
        "\"settled\":%s,\"pointing_error_deg\":%.4f,\"residual_rate_deg_s\":%.4f,"
        "\"stream_queued\":%s,\"target_terrain_source\":\"%s\",\"footprint_ground_amsl_m\":%.3f,\"footprint\":%s}",
        (unsigned long long)q.session,q.index,q.leg,(long long)q.cycle,(long long)q.row,q.column,q.slot,q.burst,q.lens,q.pattern,q.position,q.positions,
        (unsigned long long)r.frame_ms,(unsigned long long)(uint64_t(r.captured_at.tv_sec)*1000000+r.captured_at.tv_nsec/1000),
        r.pose.lat_e7*1e-7,r.pose.lon_e7*1e-7,double(r.pose.alt_amsl_m),double(q.ground),
        q.target_lat*1e-7,q.target_lon*1e-7,double(q.target_alt),double(q.hfov),q.settled?"true":"false",
        double(q.pointing_error_deg),double(q.residual_rate_deg_s),r.stream_queued?"true":"false",q.target_terrain_uploaded?"uploaded":"local plane",double(q.target_alt),footprint);
    return n>0 && unsigned(n)<size;
}

bool ca_survey_write_metadata(const ca_survey_result &r)
{
    char path[600],data[2048];
    if(!ca_survey_json(r,data,sizeof(data))) return false;
    snprintf(path,sizeof(path),"%s.json",r.path);
    FILE *f=fopen(path,"w");
    if(!f) return false;
    bool ok=fprintf(f,"%s\n",data)>0;
    if(fclose(f)) ok=false;
    if(!ok) unlink(path);
    return ok;
}
