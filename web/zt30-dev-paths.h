#ifndef APCAM_ZT30_DEV_PATHS_H
#define APCAM_ZT30_DEV_PATHS_H
/* Boot-volatile development services alongside the original camera app. */
#define APP_DIR "/tmp/apcg-zt30-dev"
#define MEDIA_ROOT APP_DIR "/media"
#define WEB_MEDIA_IS_MOUNT 0
#define REPLACEMENT_CONFIG_PATH APP_DIR "/camera.ini"
#define REPLACEMENT_CONFIG_BACKUP_PATH APP_DIR "/camera.ini.web.bak"
#define PASSWORD_PATH APP_DIR "/web.pass"
#define AUTHORIZED_KEYS_PATH APP_DIR "/auth/authorized_keys"
#define RUNTIME_AUTHORIZED_KEYS_PATH AUTHORIZED_KEYS_PATH
#define RUNTIME_AUTHORIZED_KEYS_DIR APP_DIR "/auth"
#define USER_LOCK_PATH APP_DIR "/users.lock"
#define UPGRADE_LOCK_PATH APP_DIR "/upgrade.lock"
#define RUNTIME_DIR APP_DIR "/run"
#define APP_REQUEST_PATH RUNTIME_DIR "/app-request"
#define APP_REQUEST_LOCK_PATH RUNTIME_DIR "/app-request.lock"
#define APP_STARTED_PATH RUNTIME_DIR "/app-started"
#define CAMERA_READY_PATH RUNTIME_DIR "/camera-app.ready"
#define REPLACEMENT_CAMERA_PATH APP_DIR "/camera-app"
#define WEB_PATH APP_DIR "/zt30-web"
#define REPLACEMENT_LOG_PATH RUNTIME_DIR "/camera-app.log"
#define REPLACEMENT_LOG_OLD_PATH RUNTIME_DIR "/camera-app.log.1"
/* No camera-app owns this port in the initial web/SSH-only load. */
#define CAMERA_API_PORT 37360U
#endif
