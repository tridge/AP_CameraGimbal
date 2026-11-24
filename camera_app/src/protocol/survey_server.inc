// Survey orchestration shares the MAVLink server's telemetry and ROI controller.
// Geometry and media work live in separately testable modules.
struct survey_runtime {
    ca_survey_grid grid{};
    ca_survey_request request{};
    uint64_t session=0, generation=1, claim_ms=0, manager_ms=0, terrain_ms=0, rejected_ms=0;
    uint64_t cycle_start=0, steady_ms=0, next_image_ms=0, straight_ms=0, status_ms=0;
    uint64_t last_course_ms=0;
    float last_course=0, course_rate=0;
    ca_survey_path path{};
    uint64_t mission_ms=0, mission_request_ms=0, mission_item_ms=0, auto_ms=0;
    uint16_t mission_seq=UINT16_MAX, mission_command=0;
    bool mission_active=false, auto_mode=false, grid_navigation=false;
    bool vtol_type=false;
    uint8_t vtol_state=MAV_VTOL_STATE_UNDEFINED;
    uint64_t vtol_ms=0;
    uint32_t leg=0, captured=0, skipped=0, unsettled=0;
    int64_t cycle=0;
    uint64_t hold_ms=0; // ahead aiming: earliest first frame of the current view
    unsigned slot=0, burst=0;
    int32_t remaining=-1;
    unsigned interval_ms=0;
    bool local_terrain=false;
    bool paused=true, owned=false, grid_valid=false, target_valid=false, pending=false, acquired=false;
    mavlink_terrain_report_t terrain{};
    const char *state="idle";
};

static bool survey_running(const ca_mavlink_server *s)
{ return s->survey && s->camera_mode==CAMERA_MODE_IMAGE_SURVEY && !s->survey->paused; }

bool ca_mavlink_server_survey_owns_gimbal(const ca_mavlink_server *s)
{ return s && survey_running(s) && s->survey->owned; }

static bool survey_navigation_allowed(const survey_runtime &v,uint64_t now)
{
    // QuadPlane can use a different cross-track convention while hovering.
    // Plain fixed-wing senders may omit EXTENDED_SYS_STATE entirely.
    const bool fixed_wing=(!v.vtol_type && v.vtol_state==MAV_VTOL_STATE_UNDEFINED) ||
        (v.vtol_state==MAV_VTOL_STATE_FW && v.vtol_ms && now-v.vtol_ms<=2500);
    return fixed_wing && v.auto_mode && now-v.auto_ms<=3000 && v.mission_active &&
        now-v.mission_ms<=2500 && v.mission_command==MAV_CMD_NAV_WAYPOINT &&
        v.mission_item_ms && now-v.mission_item_ms<=2500;
}

static bool survey_acquiring(ca_mavlink_server *s,float pitch,float yaw,const ca_gimbal_attitude &a)
{
    if(!survey_running(s) || s->survey->acquired) return false;
    float error=acosf(fmaxf(-1,fminf(1,sinf(pitch)*sinf(a.pitch_rad)+cosf(pitch)*cosf(a.pitch_rad)*cosf(yaw-a.yaw_rad))));
    if(error<2*PI_F/180) { s->survey->acquired=true; return false; }
    return true;
}

static void survey_claim(ca_mavlink_server *s, bool release)
{
    if(!s->have_flight_controller_route) return;
    mavlink_command_long_t c{};
    c.target_system=s->autopilot_system_id; c.target_component=s->autopilot_component_id;
    c.command=MAV_CMD_DO_GIMBAL_MANAGER_CONFIGURE;
    c.param1=release?-3:s->survey->owned?-1:-2; c.param2=-1; c.param3=-1; c.param4=-1;
    c.param7=s->gimbal_component_id;
    mavlink_message_t m;
    mavlink_msg_command_long_encode_status(s->system_id,s->gimbal_component_id,&s->encode_status,&m,&c);
    send_message(s,&s->flight_controller_route,&m);
}

static void survey_pause(ca_mavlink_server *s, const char *reason)
{
    if(!s->survey || s->survey->paused) return;
    auto &v=*s->survey;
    v.paused=true; v.owned=false; v.grid_valid=false; v.target_valid=false;
    ++v.generation; v.state=reason;
    ca_media_survey_cancel(s->media);
    s->target_location_active=false; stop_tracking_rate(s);
    survey_claim(s,true);
    parameter_status(s,reason);
}

// the survey starts from its own targets, not an earlier ROI, rate or tracker
static void survey_clear_external(ca_mavlink_server *s)
{
    ca_media_tracking_stop(s->media);
    s->target_location_active=false; stop_tracking_rate(s);
    // Direct MAVLink rates bypass the ROI controller's tracking_rate_active
    // flag. Cancel those latched rates before survey ownership blocks stops.
    (void)ca_backend_set_gimbal_rates(s->backend,0,0);
}

static uint8_t set_camera_mode(ca_mavlink_server *s, unsigned mode)
{
    if(mode>2 || (!APCAM_HAVE_PHOTO && mode!=1)) return MAV_RESULT_UNSUPPORTED;
    if(mode==2) {
        if(s->manual_control && *s->manual_control) return MAV_RESULT_TEMPORARILY_REJECTED;
        if(!s->settings.position_targeting || !ca_media_survey_available(s->media,s->settings.survey.lens)) return MAV_RESULT_UNSUPPORTED;
        auto &v=*s->survey;
        if(!survey_running(s)) {
            survey_clear_external(s);
            v.paused=false; v.owned=false; v.grid_valid=false; v.target_valid=false;
            v.claim_ms=0; v.manager_ms=0; v.straight_ms=0; v.rejected_ms=0; ++v.generation;
            v.remaining=-1; v.interval_ms=0; v.state="waiting for gimbal ownership";
            s->captures_remaining=0; s->capture_interval_s=0;
        }
    } else survey_pause(s,"survey stopped");
    s->camera_mode=mode;
    send_camera_settings(s,nullptr);
    broadcast_capture_status(s);
    return MAV_RESULT_ACCEPTED;
}

static void survey_message(ca_mavlink_server *s, const mavlink_message_t *m)
{
    if(!s->survey || m->sysid!=s->autopilot_system_id || m->compid!=s->autopilot_component_id) return;
    auto &v=*s->survey;
    const uint64_t now=monotonic_ms();
    if(m->msgid==MAVLINK_MSG_ID_HEARTBEAT) {
        mavlink_heartbeat_t h; mavlink_msg_heartbeat_decode(m,&h);
        // ArduPlane deliberately never sets MAV_MODE_FLAG_AUTO_ENABLED.
        // The reconstruction uses ArduPilot L1's signed error convention.
        const bool plane=h.type==MAV_TYPE_FIXED_WING || h.type==MAV_TYPE_VTOL_DUOROTOR ||
            h.type==MAV_TYPE_VTOL_QUADROTOR || h.type==MAV_TYPE_VTOL_TILTROTOR;
        v.vtol_type=h.type!=MAV_TYPE_FIXED_WING;
        v.auto_mode=plane && h.autopilot==MAV_AUTOPILOT_ARDUPILOTMEGA &&
            (h.base_mode&MAV_MODE_FLAG_CUSTOM_MODE_ENABLED) && h.custom_mode==10;
        v.auto_ms=now;
        if(!v.auto_mode) v.path={};
    } else if(m->msgid==MAVLINK_MSG_ID_EXTENDED_SYS_STATE) {
        mavlink_extended_sys_state_t state; mavlink_msg_extended_sys_state_decode(m,&state);
        v.vtol_state=state.vtol_state; v.vtol_ms=now;
        if(v.vtol_state!=MAV_VTOL_STATE_UNDEFINED && v.vtol_state!=MAV_VTOL_STATE_FW) v.path={};
    } else if(m->msgid==MAVLINK_MSG_ID_MISSION_CURRENT) {
        mavlink_mission_current_t c; mavlink_msg_mission_current_decode(m,&c);
        if(c.seq!=v.mission_seq) {
            v.path={}; v.mission_seq=c.seq; v.mission_command=0;
            v.mission_item_ms=0; v.mission_request_ms=0;
        }
        // Zero extensions mean an older sender; heartbeat still gates AUTO.
        v.mission_active=c.mission_mode!=2 && (c.mission_state==0 || c.mission_state==MISSION_STATE_ACTIVE);
        v.mission_ms=now;
        if(!v.mission_active) v.path={};
    } else if(m->msgid==MAVLINK_MSG_ID_MISSION_ITEM_INT) {
        mavlink_mission_item_int_t item; mavlink_msg_mission_item_int_decode(m,&item);
        if(item.target_system!=s->system_id || item.target_component!=s->camera_component_id ||
            item.mission_type!=MAV_MISSION_TYPE_MISSION || item.seq!=v.mission_seq ||
            !v.mission_request_ms || now-v.mission_request_ms>2000) return;
        if(item.command!=v.mission_command) v.path={};
        v.mission_command=item.command; v.mission_item_ms=now;
    } else if(m->msgid==MAVLINK_MSG_ID_NAV_CONTROLLER_OUTPUT) {
        mavlink_nav_controller_output_t nav; mavlink_msg_nav_controller_output_decode(m,&nav);
        ca_survey_pose p{};
        if(survey_navigation_allowed(v,now) && now-s->vehicle_position_updated_ms<=500 &&
            current_vehicle_position(s,&p.lat,&p.lon,&p.alt)) {
            p.vn=s->vehicle_vn_m_s; p.ve=s->vehicle_ve_m_s;
            v.path.update(p,nav.target_bearing,nav.wp_dist,nav.xtrack_error,now);
        } else v.path={};
    } else if(m->msgid==MAVLINK_MSG_ID_TERRAIN_REPORT) {
        mavlink_msg_terrain_report_decode(m,&v.terrain); v.terrain_ms=monotonic_ms();
    } else if(m->msgid==MAVLINK_MSG_ID_COMMAND_ACK) {
        mavlink_command_ack_t a; mavlink_msg_command_ack_decode(m,&a);
        if(a.command!=MAV_CMD_DO_GIMBAL_MANAGER_CONFIGURE || !survey_running(s) ||
           a.target_system!=s->system_id || a.target_component!=s->gimbal_component_id) return;
        // ArduPilot denies a renewal once the pilot or GCS has taken control
        if(a.result==MAV_RESULT_DENIED) survey_pause(s,"survey paused: gimbal takeover");
        else if(a.result==MAV_RESULT_TEMPORARILY_REJECTED && !v.owned) v.rejected_ms=now;
    } else if(m->msgid==MAVLINK_MSG_ID_GIMBAL_MANAGER_STATUS) {
        mavlink_gimbal_manager_status_t g; mavlink_msg_gimbal_manager_status_decode(m,&g);
        if(g.gimbal_device_id!=s->gimbal_component_id) return;
        bool ours=g.primary_control_sysid==s->system_id && g.primary_control_compid==s->gimbal_component_id;
        if(survey_running(s) && v.owned && !ours) survey_pause(s,"survey paused: gimbal takeover");
        if(ours && survey_running(s)) {
            if(!v.owned) survey_clear_external(s);
            v.owned=true; v.manager_ms=monotonic_ms();
        }
    }
}

static void survey_request_mission_item(ca_mavlink_server *s,uint64_t now)
{
    auto &v=*s->survey;
    if(!s->have_flight_controller_route || !v.auto_mode || now-v.auto_ms>3000 ||
        !v.mission_active || !v.mission_ms || now-v.mission_ms>2500 || v.mission_seq==UINT16_MAX ||
        (v.mission_item_ms && now-v.mission_item_ms<1000) ||
        (v.mission_request_ms && now-v.mission_request_ms<1000)) return;
    // Fetch just the active item's type to exclude loiters, takeoff and landing.
    // Periodic refresh also notices an in-place mission edit at the same index.
    mavlink_mission_request_int_t r{};
    r.target_system=s->autopilot_system_id; r.target_component=s->autopilot_component_id;
    r.seq=v.mission_seq; r.mission_type=MAV_MISSION_TYPE_MISSION;
    mavlink_message_t m;
    mavlink_msg_mission_request_int_encode_status(s->system_id,s->camera_component_id,&s->encode_status,&m,&r);
    send_message(s,&s->flight_controller_route,&m);
    v.mission_request_ms=now;
}

static void survey_capture_notice(ca_mavlink_server *s,const ca_survey_result &r)
{
    const auto &p=r.pose;
    mavlink_message_t m;
    float q[4]={NAN,NAN,NAN,NAN};
    if(p.have_vehicle_attitude && p.have_gimbal_attitude)
        euler_to_quaternion(p.gimbal_roll_rad,p.gimbal_pitch_rad,p.vehicle_yaw_rad+p.gimbal_yaw_rad,q);
    uint32_t timestamp=r.frame_ms>=s->started_ms?r.frame_ms-s->started_ms:0;
    // Pair FOV geometry and capture by identical boot timestamp. This works
    // without a video viewer or a camera-specific MAVLink dialect.
    mavlink_camera_fov_status_t f{};
    f.time_boot_ms=timestamp; f.lat_camera=p.lat_e7; f.lon_camera=p.lon_e7;
    f.alt_camera=p.alt_amsl_m*1000;
    f.lat_image=INT32_MAX; f.lon_image=INT32_MAX;
    const float down=-sinf(p.gimbal_pitch_rad);
    if(p.have_position && p.have_vehicle_attitude && p.have_gimbal_attitude && down>0.02f) {
        f.lat_image=p.lat_e7; f.lon_image=p.lon_e7;
        float alt=p.alt_amsl_m;
        const float distance=(alt-r.request.target_alt)*cosf(p.gimbal_pitch_rad)/down;
        const float heading=p.vehicle_yaw_rad+p.gimbal_yaw_rad;
        ca_targeting_predict_position(&f.lat_image,&f.lon_image,&alt,
            distance*cosf(heading),distance*sinf(heading),0,1);
    }
    f.alt_image=r.request.target_alt*1000;
    memcpy(f.q,q,sizeof(q)); f.hfov=r.request.hfov;
    f.vfov=2*atan(tan(f.hfov*PI_F/360)/r.request.aspect)*180/PI_F;
    mavlink_msg_camera_fov_status_encode_status(s->system_id,s->camera_component_id,&s->encode_status,&m,&f);
    broadcast_message(s,&m);
    mavlink_camera_image_captured_t c{};
    c.time_boot_ms=timestamp;
    c.time_utc=uint64_t(r.captured_at.tv_sec)*1000000+r.captured_at.tv_nsec/1000;
    c.image_index=r.request.index; c.capture_result=r.error?0:1;
    c.lat=p.lat_e7; c.lon=p.lon_e7; c.alt=p.alt_amsl_m*1000;
    c.relative_alt=(p.alt_amsl_m-r.request.ground)*1000; memcpy(c.q,q,sizeof(q));
    const char *relative=strncmp(r.path,s->capture_root,strlen(s->capture_root))==0?r.path+strlen(s->capture_root):nullptr;
    if(relative) snprintf(c.file_url,sizeof(c.file_url),"mavlinkftp://%u:%u/capture%s",s->system_id,s->camera_component_id,relative);
    mavlink_msg_camera_image_captured_encode_status(s->system_id,s->camera_component_id,&s->encode_status,&m,&c);
    broadcast_message(s,&m); broadcast_capture_status(s);
}

static void survey_status_file(ca_mavlink_server *s,uint64_t now)
{
    auto &v=*s->survey;
    if(now-v.status_ms<500) return;
    v.status_ms=now;
    char command[4096]; snprintf(command,sizeof(command),"%s.survey.command",s->config_path);
    FILE *input=fopen(command,"r");
    if(input) {
        int mode=-1; int count=fscanf(input,"%d",&mode); fclose(input); unlink(command);
        if(count==1 && (mode==0 || mode==2) && set_camera_mode(s,unsigned(mode))!=MAV_RESULT_ACCEPTED)
            v.state="survey unavailable for selected camera";
    }
    char path[4096],tmp[4100];
    if (!apcam_survey_status_path(s->config_path, path, sizeof(path))) return;
    snprintf(tmp,sizeof(tmp),"%s.tmp",path);
    FILE *f=fopen(tmp,"w"); if(!f) return;
    fprintf(f,"{\"mode\":%u,\"state\":\"%s\",\"leg\":%u,\"cycle\":%lld,\"slot\":%u,\"positions\":%u,\"pattern\":%u,"
        "\"captured\":%u,\"skipped\":%u,\"unsettled\":%u,\"cycle_seconds\":%.2f,"
        "\"row_spacing_m\":%.2f,\"nominal_overlap_pct\":%.1f,\"path_source\":\"%s\",\"terrain_source\":\"%s\",\"terrain_age_ms\":%llu}\n",
        s->camera_mode,v.state,v.leg,(long long)v.cycle,v.grid_valid?v.slot+1:0,
        ca_survey_positions(s->settings.survey.pattern),s->settings.survey.pattern,v.captured,v.skipped,v.unsettled,
        v.grid_valid?v.grid.cycle_s:0,v.grid_valid?v.grid.spacing:0,v.grid_valid?v.grid.nominal_overlap:0,
        v.grid_valid?(v.grid_navigation?"navigation":"ground track"):"none",
        v.local_terrain?"uploaded":"flight controller",
        (unsigned long long)(v.terrain_ms?now-v.terrain_ms:0));
    if(fclose(f)==0) rename(tmp,path); else unlink(tmp);
}

static void update_survey(ca_mavlink_server *s,uint64_t now)
{
    if(!s->survey) return;
    auto &v=*s->survey;
    ca_survey_result result{};
    if(ca_media_survey_poll(s->media,result)) {
        v.pending=false;
        if(!result.error) { ++v.captured; ++s->image_count; if(!result.request.settled) ++v.unsettled; }
        else { ++v.skipped; ca_log("Survey image %d failed: %s",result.request.index,strerror(result.error)); }
        survey_capture_notice(s,result);
        if(result.error && result.error!=ECANCELED && result.error!=ETIMEDOUT)
            survey_pause(s,"survey paused: capture/storage failure");
        if(!result.error && result.request.generation==v.generation && v.remaining>0 && --v.remaining==0)
            survey_pause(s,"survey complete: image count reached");
    }
    survey_status_file(s,now);
    if(!survey_running(s)) return;
    survey_request_mission_item(s,now);
    const auto tracking=ca_media_tracking_status(s->media);
    if(tracking.state==CA_TRACK_ACQUIRING || tracking.state==CA_TRACK_ACTIVE || tracking.state==CA_TRACK_COASTING) {
        survey_pause(s,"survey paused: image tracking"); return;
    }
    if(s->manual_control && *s->manual_control) { survey_pause(s,"survey paused: manual control"); return; }
    if(v.owned && now-v.manager_ms>3000) { survey_pause(s,"survey paused: ownership timeout"); return; }
    if(!v.claim_ms || now-v.claim_ms>=1000) { survey_claim(s,false); v.claim_ms=now; }
    if(!v.owned) {
        v.state=v.rejected_ms && now-v.rejected_ms<2000 ?
            "waiting for gimbal ownership: mount retracted or not ready" : "waiting for gimbal ownership";
        return;
    }
    ca_survey_pose pose{};
    float yaw=0,yaw_rate=0;
    const auto &cfg=s->settings.survey;
    bool valid=current_vehicle_position(s,&pose.lat,&pose.lon,&pose.alt) && current_vehicle_attitude(s,&yaw,&yaw_rate);
    pose.vn=s->vehicle_vn_m_s; pose.ve=s->vehicle_ve_m_s; pose.ground=v.terrain.terrain_height;
    double terrain_distance=hypot((pose.lat-double(v.terrain.lat))*1e-7*111319.5,
        remainder((pose.lon-double(v.terrain.lon))*1e-7,360.0)*111319.5*cos(pose.lat*1e-7*PI_F/180));
    bool report_valid=v.terrain_ms && now-v.terrain_ms<=cfg.terrain_ms && v.terrain.spacing &&
        isfinite(pose.ground) && isfinite(v.terrain.current_height) && pose.alt-pose.ground>5 &&
        fabs((pose.alt-pose.ground)-v.terrain.current_height)<=cfg.terrain_tol && terrain_distance<=cfg.terrain_m;
    v.local_terrain=ca_terrain_height(pose.lat,pose.lon,pose.ground)==CA_TERRAIN_READY;
    valid=valid && (v.local_terrain || report_valid) && pose.alt-pose.ground>5;
    float speed=hypotf(pose.vn,pose.ve),course=atan2f(pose.ve,pose.vn);
    ca_survey_pose path_pose=pose;
    const bool navigation=survey_navigation_allowed(v,now) &&
        now-s->vehicle_position_updated_ms<=500 && v.path.project(pose,now,path_pose);
    if(!navigation) path_pose=pose;
    const float path_course=atan2f(path_pose.ve,path_pose.vn);
    // Differentiate new position samples, not the faster control-loop ticks.
    // Smooth GPS velocity quantisation before deciding a straight leg ended.
    uint64_t sample_ms=s->vehicle_position_updated_ms;
    if(sample_ms>v.last_course_ms) {
        if(v.last_course_ms && sample_ms-v.last_course_ms<1000) {
            float dt=(sample_ms-v.last_course_ms)*.001f;
            float rate=wrap_pi_f(course-v.last_course)*180/PI_F/dt;
            v.course_rate+=(rate-v.course_rate)*dt/(.5f+dt);
        } else v.course_rate=0;
        v.last_course=course; v.last_course_ms=sample_ms;
    }
    float course_rate=fabsf(v.course_rate);
    if(!valid || speed<cfg.min_speed || course_rate>5 ||
        (navigation && fabs(wrap_pi_f(course-path_course))*180/PI_F>cfg.turn_deg) ||
        (v.grid_valid && fabs(wrap_pi_f(path_course-v.grid.course))*180/PI_F>cfg.turn_deg)) {
        if(v.grid_valid || v.target_valid) { ++v.generation; ca_media_survey_cancel(s->media); }
        v.grid_valid=false; v.target_valid=false; v.straight_ms=0;
        s->target_location_active=false; stop_tracking_rate(s);
        v.state=!valid?"waiting for fresh terrain and telemetry":"waiting for straight flight"; return;
    }
    if(v.grid_valid && fabsf(ca_media_survey_hfov(s->media,cfg.lens)-v.request.hfov)>.01f) {
        survey_pause(s,"survey paused: lens field of view changed"); return;
    }
    // A small heading difference accumulates arbitrarily large lateral error
    // on long legs. Reanchor future rows, but never move a burst's fixed target
    // or replace geometry while a frame is pending in the media worker.
    bool realigning=false;
    if(v.grid_valid && !v.pending &&
        (!v.burst || v.burst>=cfg.burst || now>=v.grid.deadline(v.cycle_start,v.slot)) &&
        (navigation!=v.grid_navigation || v.grid.alignment_error(path_pose)>v.grid.alignment_limit(path_pose))) {
        ca_log("Survey grid realign: cross %.1fm, alignment %.1fm, limit %.1fm",
            v.grid.cross_track(path_pose),v.grid.alignment_error(path_pose),v.grid.alignment_limit(path_pose));
        if(v.target_valid && v.burst<cfg.burst) v.skipped+=cfg.burst-v.burst;
        ++v.generation; v.grid_valid=false; v.target_valid=false;
        // Telemetry and turn checks already passed; avoid another startup gap
        // when the aircraft has merely departed a stale grid centreline.
        v.straight_ms=now>=2000?now-2000:0;
        s->target_location_active=false; stop_tracking_rate(s);
        realigning=true;
    }
    if(!v.grid_valid) {
        if(speed<cfg.min_speed+2 || course_rate>2) { v.straight_ms=0; return; }
        if(!v.straight_ms) v.straight_ms=now;
        if(now-v.straight_ms<2000) return;
        float hfov=ca_media_survey_hfov(s->media,cfg.lens);
        float aspect=cfg.lens==0?1.25f:16.0f/9;
        if(!v.grid.init(path_pose,cfg,hfov,aspect,APCAM_GIMBAL_RATE_MAX,now)) return;
        v.grid_navigation=navigation;
        ++v.leg; v.grid_valid=true; v.cycle=0; v.slot=0;
        v.burst=0; v.steady_ms=0; v.next_image_ms=0;
        // Centre the first sweep on where the aircraft will be halfway
        // through it. Start tracking immediately instead of waiting half a sweep.
        float ground=v.grid.origin.ground;
        ca_targeting_predict_position(&v.grid.origin.lat,&v.grid.origin.lon,&ground,
            path_pose.vn,path_pose.ve,0,v.grid.cycle_s/2);
        v.cycle_start=now;
        v.request.hfov=hfov; v.request.aspect=aspect;
        v.state="survey leg started";
        if(!realigning) parameter_status(s,"Survey started; see achieved overlap and coverage");
    }
    if(now<v.cycle_start) { v.state="waiting for next ground row"; return; }
    uint64_t deadline=v.grid.deadline(v.cycle_start,v.slot);
    if(now>=deadline) {
        if(v.burst<cfg.burst) v.skipped+=cfg.burst-v.burst;
        v.target_valid=false; v.burst=0; v.steady_ms=0;
        if(++v.slot==v.grid.slot_count) {
            v.slot=0; ++v.cycle;
            double along=v.grid.along(path_pose);
            const float along_speed=fmaxf(cfg.min_speed,path_pose.vn*cos(v.grid.course)+path_pose.ve*sin(v.grid.course));
            int64_t next=int64_t(floor((along+along_speed*v.grid.cycle_s/2)/v.grid.spacing));
            if(next>v.cycle) { v.skipped+=unsigned(next-v.cycle)*v.grid.slot_count*cfg.burst; v.cycle=next; }
            double dt=(v.cycle*v.grid.spacing-along)/along_speed-v.grid.cycle_s/2;
            v.cycle_start=now+uint64_t(fmax(0,dt)*1000);
            // Keep the ground rows fixed for repeat views. Terrain/AGL and
            // speed changes must not discard the lattice and add a startup gap.
            // Speed already enters the phase calculation above. Use fresh
            // terrain independently at every target, without moving a burst.
            v.grid.height=pose.alt-pose.ground;
            v.grid.nominal_overlap=100*(1-v.grid.spacing/
                (2*v.grid.height*tan(v.request.hfov*PI_F/360)/v.request.aspect));
        }
        deadline=v.grid.deadline(v.cycle_start,v.slot);
        if(now<v.cycle_start) return;
    }
    if(!v.target_valid) {
        auto &r=v.request;
        r.session=v.session; r.generation=v.generation; r.leg=v.leg; r.cycle=v.cycle; r.slot=v.grid.slots[v.slot];
        r.pattern=cfg.pattern; r.position=v.slot; r.positions=v.grid.slot_count;
        v.hold_ms=0;
        if(cfg.aim==CA_SURVEY_AIM_AHEAD) {
            // The target stays fixed for the burst. Plan the first frame for
            // after this view's slew allowance and settling, leaving room for the
            // burst before the deadline, and hold it until then. Place the target
            // where the aircraft will be at the middle of the burst. A gimbal
            // slower than the allowance captures late, as in rows mode.
            const double agl=pose.alt-pose.ground;
            // Frames are spaced by the burst interval or the requested capture
            // interval, whichever is longer.
            const double burst_s=(cfg.burst-1)*fmax(cfg.burst_ms,v.interval_ms)*.001;
            const double window_s=deadline>now?(deadline-now)*.001-burst_s-.15:0;
            const double first_s=fmax(0,fmin(v.grid.budget[v.slot]-cfg.dwell_ms*.001-burst_s+cfg.settle_ms*.001,window_s));
            v.hold_ms=now+uint64_t(first_s*1000);
            const double along_speed=fmax(0,path_pose.vn*cos(v.grid.course)+path_pose.ve*sin(v.grid.course));
            v.grid.target_ahead(path_pose,along_speed*(first_s+burst_s/2),cfg.fore_pct*.01*agl,cfg.aft_pct*.01*agl,
                                v.slot,r.target_lat,r.target_lon,r.row,r.column);
        } else
            v.grid.target(v.cycle,v.slot,r.target_lat,r.target_lon,r.row,r.column);
        r.target_alt=pose.ground; r.lens=cfg.lens;
        auto terrain=ca_terrain_height(r.target_lat,r.target_lon,r.target_alt);
        r.target_terrain_uploaded=terrain==CA_TERRAIN_READY;
        if(terrain==CA_TERRAIN_PENDING) { v.state="loading target terrain"; return; }
        // When only aircraft-local uploaded terrain is available, it is also
        // the best flat-plane fallback for a target outside the uploaded area.
        s->target_lat_e7=r.target_lat; s->target_lon_e7=r.target_lon; s->target_alt_amsl_m=r.target_alt;
        s->target_location_active=true; s->last_target_location_ms=0;
        memset(s->tracking_integral,0,sizeof(s->tracking_integral));
        v.acquired=false; v.target_valid=true;
    }
    v.state="slewing";
    ca_gimbal_attitude a{};
    float pitch,target_yaw,pr,yr;
    if(!ca_backend_gimbal_attitude(s->backend,&a) || now<a.timestamp_ms || now-a.timestamp_ms>300 ||
        !ca_targeting_global_angles(pose.lat,pose.lon,pose.alt,v.request.target_lat,v.request.target_lon,v.request.target_alt,&pitch,&target_yaw) ||
        !ca_targeting_global_rates(pose.lat,pose.lon,pose.alt,v.request.target_lat,v.request.target_lon,v.request.target_alt,
            pose.vn,pose.ve,s->vehicle_vd_m_s,&pr,&yr)) { v.steady_ms=0; return; }
    const float rad=PI_F/180;
    if(pitch<APCAM_GIMBAL_PITCH_MIN*rad || pitch>APCAM_GIMBAL_PITCH_MAX*rad ||
        fabs(yr-yaw_rate)>APCAM_GIMBAL_RATE_MAX*rad) { v.steady_ms=0; return; }
    float dot=sinf(pitch)*sinf(a.pitch_rad)+cosf(pitch)*cosf(a.pitch_rad)*cosf(wrap_pi_f(target_yaw-yaw-a.yaw_rad));
    float error=acosf(fmaxf(-1,fminf(1,dot)))/rad;
    float residual=hypotf(a.pitch_rate_rad_s-pr,cosf(pitch)*(a.yaw_rate_rad_s-(yr-yaw_rate)))/rad;
    bool steady=isfinite(error)&&isfinite(residual)&&error<=cfg.error_cd*.01f&&residual<=cfg.rate_cd*.01f;
    if(steady) { if(!v.steady_ms) v.steady_ms=now; } else v.steady_ms=0;
    bool settled=v.steady_ms && now-v.steady_ms>=cfg.settle_ms;
    v.state=settled?"capturing settled view":"settling";
    if(v.pending || v.burst>=cfg.burst || now<v.next_image_ms) return;
    // An ahead target was placed for a planned capture time; an earlier
    // first frame would land further ahead than intended.
    if(!v.burst && now<v.hold_ms) { if(settled) v.state="holding for capture point"; return; }
    uint64_t burst_reserve=uint64_t((cfg.burst-v.burst-1)*fmax(cfg.burst_ms,v.interval_ms))+150;
    if(!settled && now+burst_reserve<deadline) return;
    ca_metadata m; ca_metadata_snapshot(&m); double corners[4][2];
    if(!ca_survey_footprint(m,v.request.target_alt,v.request.hfov,v.request.aspect,corners)) return;
    auto &r=v.request;
    if(s->next_image_index==INT32_MAX) { s->next_image_index=1; ++v.session; r.session=v.session; }
    r.index=s->next_image_index; r.burst=v.burst; r.eligible_ms=now; r.deadline_ms=deadline;
    r.settled=settled; r.pointing_error_deg=error; r.residual_rate_deg_s=residual; r.ground=pose.ground;
    if(ca_media_survey_start(s->media,r)) {
        ++s->next_image_index; ++v.burst; v.pending=true;
        v.next_image_ms=now+fmax(cfg.burst_ms,v.interval_ms);
    }
}
