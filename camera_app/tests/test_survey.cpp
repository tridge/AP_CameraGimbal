#include "camera_app/survey.h"
#include "camera_app/targeting.h"
#include <assert.h>
#include <initializer_list>
#include <math.h>
#include <stdio.h>

int main()
{
    // L1 reports positive error LEFT of the line. Recover both the direction
    // and centreline across every quadrant, independent of wind correction.
    for(double heading: {0.0,45.0,90.0,179.0,-179.0,-90.0}) for(float error: {-100.0f,100.0f}) {
        const double course=heading*M_PI/180;
        ca_survey_pose p{-350000000,1490000000,1000,600,float(25*cos(course)),float(25*sin(course))};
        const auto centre=p;
        assert(ca_targeting_predict_position(&p.lat,&p.lon,&p.alt,error*sin(course),-error*cos(course),0,1));
        const double beta=remainder(course+asin(error/1000),2*M_PI)*180/M_PI;
        ca_survey_path path{};
        ca_survey_pose projected{};
        assert(path.update(p,beta,1000,error,1000));
        assert(!path.project(p,1000,projected)); // acquire a stable estimate first
        for(unsigned now=1200;now<=2400;now+=200) assert(path.update(p,beta,1000,error,now));
        assert(path.project(p,2400,projected));
        assert(abs(projected.lat-centre.lat)<4 && abs(projected.lon-centre.lon)<4);
        assert(fabs(remainder(path.course-course,2*M_PI))<1e-6);
        assert(!path.project(p,4001,projected));
        assert(!path.update(p,beta,10,error,4200));
        assert(!path.project(p,4200,projected));
        assert(!path.update(p,beta,UINT16_MAX,error,4400));
        assert(!path.update(p,NAN,1000,error,4600));
        assert(!path.update(p,beta,1000,NAN,4800));
        assert(!path.update(p,beta,1000,900,5000));
    }
    {
        ca_survey_path path{};
        ca_survey_pose p{-350000000,1490000000,1000,600,25,0}, projected{};
        assert(!path.update(p,180,1000,0,1000)); // waypoint already behind us
        for(unsigned now=2000;now<=6000;now+=200) {
            assert(path.update(p,now%400?0:1,1000,0,now)); // integer-degree steps
        }
        assert(path.project(p,6000,projected));
        assert(path.course>0 && path.course<M_PI/180);
        assert(!path.project(p,5999,projected));
        p.vn=0; p.ve=25;
        assert(path.update(p,90,1000,0,6200)); // new leg must reacquire stability
        assert(!path.project(p,6200,projected));
        for(unsigned now=6400;now<=7600;now+=200) assert(path.update(p,90,1000,0,now));
        assert(path.project(p,7600,projected));
        assert(path.update(p,90,2000,0,7800)); // same sequence, changed destination
        assert(!path.project(p,7800,projected));
    }
    ca_config config; ca_config_defaults(&config);
    for(float speed: {18.0f,25.0f}) {
        ca_survey_pose p{-350000000,1490000000,1000,600,speed,0};
        ca_survey_grid g{};
        assert(g.init(p,config.survey,24.2,1.25,60,1000));
        ca_survey_grid faster{};
        assert(faster.init(p,config.survey,24.2,1.25,100,1000));
        assert(faster.cycle_s<g.cycle_s && faster.spacing<g.spacing);
        assert(g.spacing>=speed*g.cycle_s-.01);
        for(unsigned slot=0;slot<3;slot++) {
            int32_t lat1,lon1,lat2,lon2; int64_t row1,row2; int col1,col2;
            g.target(10,slot,lat1,lon1,row1,col1);
            g.target(10+g.fore_rows,5-slot,lat2,lon2,row2,col2);
            assert(lat1==lat2 && lon1==lon2 && row1==row2 && col1==col2);
            g.target(10+g.fore_rows+g.aft_rows,6+slot,lat2,lon2,row2,col2);
            assert(lat1==lat2 && lon1==lon2 && row1==row2 && col1==col2);
        }
        for(unsigned slot=1;slot<9;slot++) assert(g.deadline(1000,slot)>g.deadline(1000,slot-1));
        printf("%.0f m/s: sweep %.2fs spacing %.1fm nominal same-view overlap %.1f%%\n",
            speed,g.cycle_s,g.spacing,g.nominal_overlap);
    }
    for(unsigned pattern: {unsigned(CA_SURVEY_LEFT_RIGHT),unsigned(CA_SURVEY_FORE_AFT)}) {
        ca_survey_pose p{-350000000,1490000000,1000,600,25,0};
        ca_survey_grid full{},g{};
        assert(full.init(p,config.survey,24.2,1.25,60,1000));
        config.survey.pattern=pattern;
        assert(g.init(p,config.survey,24.2,1.25,60,1000));
        assert(g.slot_count==2 && g.cycle_s<full.cycle_s && g.spacing<full.spacing);
        assert(g.deadline(1000,1)==1000+uint64_t(g.cycle_s*1000));
        int32_t lat[2],lon[2];int64_t row[2];int column[2];
        for(unsigned k=0;k<2;k++) g.target(10,k,lat[k],lon[k],row[k],column[k]);
        if(pattern==CA_SURVEY_LEFT_RIGHT) {
            assert(g.slots[0]==5 && g.slots[1]==3);
            assert(row[0]==10 && row[1]==10 && column[0]==-1 && column[1]==1);
            assert(lat[0]==lat[1] && lon[0]<p.lon && lon[1]>p.lon);
            auto original=g;
            config.survey.fore_pct=200; config.survey.aft_pct=10;
            assert(g.init(p,config.survey,24.2,1.25,60,1000));
            assert(g.cycle_s==original.cycle_s && g.spacing==original.spacing);
            config.survey.fore_pct=config.survey.aft_pct=50;
        } else {
            assert(g.slots[0]==1 && g.slots[1]==7);
            assert(column[0]==0 && column[1]==0 && lat[0]>lat[1]);
            int32_t lat2,lon2;int64_t row2;int col2;
            g.target(10+g.fore_rows+g.aft_rows,1,lat2,lon2,row2,col2);
            assert(lat[0]==lat2 && lon[0]==lon2 && row[0]==row2);
        }
        printf("pattern %u: %.2fs sweep, %.1fm row spacing\n",pattern,g.cycle_s,g.spacing);
        config.survey.pattern=CA_SURVEY_BOTH;
    }
    for(float speed: {18.0f,25.0f}) {
        ca_survey_pose p{-350000000,1490000000,1000,600,speed,0};
        ca_survey_grid g{},paired{};
        config.survey.pattern=CA_SURVEY_FORE_AFT;
        assert(paired.init(p,config.survey,24.2,1.25,100,1000));
        config.survey.pattern=CA_SURVEY_FORE_ONLY;
        assert(g.init(p,config.survey,24.2,1.25,100,1000));
        assert(g.slot_count==3 && g.cycle_s<paired.cycle_s);
        assert(g.deadline(1000,2)==1000+uint64_t(g.cycle_s*1000));
        int32_t lat[3],lon[3];int64_t row[3];int column[3];
        for(unsigned k=0;k<3;k++) {
            g.target(10,k,lat[k],lon[k],row[k],column[k]);
            assert(g.slots[k]==9+k && column[k]==0 && lon[k]==p.lon);
            assert(row[k]==10+(3-int(k))*g.fore_rows && lat[k]>p.lat);
            int32_t next_lat,next_lon;int64_t next_row;int next_column;
            g.target(10+k*g.fore_rows,k,next_lat,next_lon,next_row,next_column);
            assert(next_lat==lat[0] && next_lon==lon[0] && next_row==row[0]);
        }
        assert(lat[0]>lat[1] && lat[1]>lat[2]);
        auto original=g;
        config.survey.aft_pct=200;
        assert(g.init(p,config.survey,24.2,1.25,100,1000));
        assert(g.cycle_s==original.cycle_s && g.spacing==original.spacing);
        config.survey.aft_pct=50;
        printf("fore-only %.0fm/s: %.2fs sweep, %.1fm rows, near/middle/far %.1f/%.1f/%.1fm\n",
            speed,g.cycle_s,g.spacing,g.fore_rows*g.spacing,2*g.fore_rows*g.spacing,3*g.fore_rows*g.spacing);
    }
    {
        ca_survey_pose p{-350000000,1490000000,1000,600,25,0}; // flying north
        config.survey.pattern=CA_SURVEY_FORE_WIDE;
        ca_survey_grid g{};
        assert(g.init(p,config.survey,24.2,1.25,100,1000));
        assert(g.slot_count==3);
        const unsigned expect[3]={0,1,2};
        int32_t lat[3],lon[3]; int64_t row[3]; int column[3];
        for(unsigned k=0;k<3;k++) {
            assert(g.slots[k]==expect[k]);
            g.target(10,k,lat[k],lon[k],row[k],column[k]);
            assert(row[k]==10+g.fore_rows && lat[k]>p.lat);   // all ahead
        }
        assert(column[0]==-1 && column[1]==0 && column[2]==1);
        assert(lon[0]<p.lon && lon[1]==p.lon && lon[2]>p.lon);   // left, centre, right
        for(unsigned k=1;k<3;k++) assert(g.deadline(1000,k)>g.deadline(1000,k-1));
        assert(g.deadline(1000,2)==1000+uint64_t(g.cycle_s*1000));
        config.survey.pattern=CA_SURVEY_BOTH;
    }
    {
        // Ahead aiming: targets follow the aircraft along the leg, offset by the
        // capture lead, rather than staying on rows fixed at the start of the leg.
        ca_survey_pose p{-350000000,1490000000,1000,600,25,0}; // flying north
        ca_survey_grid g{};
        assert(g.init(p,config.survey,24.2,1.25,100,1000)); // Both: views 0..8
        ca_survey_pose q=p;
        assert(ca_targeting_predict_position(&q.lat,&q.lon,&q.alt,500,0,0,1)); // 500m along the leg
        assert(fabs(g.along(q)-500)<.5);
        const double m_per_e7=6378137.0*M_PI/180*1e-7;
        const double lead=50, fore=200, aft=100;
        const double expect_north[3]={lead+fore,lead,lead-aft}; // fore, down, rear rows
        const int expect_column[9]={-1,0,1,1,0,-1,-1,0,1};
        for(unsigned k=0;k<9;k++) {
            int32_t lat,lon; int64_t row; int column;
            g.target_ahead(q,lead,fore,aft,k,lat,lon,row,column);
            const double north=(lat-q.lat)*m_per_e7;
            const double east=(lon-q.lon)*m_per_e7*cos(q.lat*1e-7*M_PI/180);
            assert(fabs(north-expect_north[k/3])<.5);
            assert(column==expect_column[k]);
            assert(fabs(east-column*g.cross_spacing)<.5);
            assert(row==int64_t(floor((500+expect_north[k/3])/g.spacing+.5)));
        }
        config.survey.pattern=CA_SURVEY_FORE_ONLY;
        assert(g.init(p,config.survey,24.2,1.25,100,1000)); // far/middle/near: 3x/2x/1x
        for(unsigned k=0;k<3;k++) {
            int32_t lat,lon; int64_t row; int column;
            g.target_ahead(q,0,fore,aft,k,lat,lon,row,column);
            assert(fabs((lat-q.lat)*m_per_e7-(3-k)*fore)<.5 && column==0);
        }
        config.survey.pattern=CA_SURVEY_BOTH;
    }
    // A 5.4-degree departure never crosses the 15-degree turn threshold, but
    // previously left targets over 1km off a long leg. Both turn directions
    // and all patterns must reanchor before that error accumulates.
    for(unsigned pattern: {0U,1U,2U,3U,4U}) for(float sign: {-1.0f,1.0f}) {
        ca_survey_pose p{-350000000,1490000000,1000,600,25,0};
        config.survey.pattern=pattern;
        ca_survey_grid g{};
        assert(g.init(p,config.survey,24.2,1.25,100,1000));
        assert(g.alignment_error(p)==0 && g.alignment_limit(p)>30 && g.alignment_limit(p)<40);
        const double turn=sign*5.4*M_PI/180;
        p.vn=25*cos(turn); p.ve=25*sin(turn);
        unsigned realignments=0;
        for(unsigned second=1;second<=600;second++) {
            assert(ca_targeting_predict_position(&p.lat,&p.lon,&p.alt,p.vn,p.ve,0,1));
            if(g.alignment_error(p)>g.alignment_limit(p)) {
                assert(second<30); // once, early; the remaining straight leg stays fixed
                assert(g.init(p,config.survey,24.2,1.25,100,second*1000));
                ++realignments;
            }
            assert(fabs(g.cross_track(p))<40);
        }
        assert(realignments==1);
        // Parallel lateral drift must also trigger, even with no heading error.
        assert(ca_targeting_predict_position(&p.lat,&p.lon,&p.alt,-sin(turn)*100,cos(turn)*100,0,1));
        assert(g.alignment_error(p)>g.alignment_limit(p));
    }
    ca_metadata m{};
    m.have_position=m.have_vehicle_attitude=m.have_gimbal_attitude=true;
    m.lat_e7=-350000000; m.lon_e7=1490000000; m.alt_amsl_m=1000;
    m.gimbal_pitch_rad=-M_PI/2;
    double points[4][2];
    assert(ca_survey_footprint(m,600,24.2,1.25,points));
    double height=fabs(points[0][0]-points[2][0])*111319.5;
    double width=fabs(points[0][1]-points[2][1])*111319.5*cos(35*M_PI/180);
    assert(fabs(width-171.51)<.1 && fabs(height-137.20)<.1);
    m.gimbal_pitch_rad=0;
    assert(!ca_survey_footprint(m,600,24.2,1.25,points));
    m.gimbal_pitch_rad=NAN;
    assert(!ca_survey_footprint(m,600,24.2,1.25,points));
    puts("PASS survey ground-grid revisits, timing and actual footprint geometry");
}
