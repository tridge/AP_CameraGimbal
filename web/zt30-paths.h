#pragma once
/* Persistent AP-only layout; runtime files stay on tmpfs. */
#ifndef APP_DIR
#define APP_DIR "/customer/camera-app"
#endif
#ifndef MEDIA_ROOT
#define MEDIA_ROOT "/mnt"
#endif
#ifndef REPLACEMENT_CONFIG_PATH
#define REPLACEMENT_CONFIG_PATH APP_DIR "/camera.ini"
#endif
#ifndef REPLACEMENT_CONFIG_BACKUP_PATH
#define REPLACEMENT_CONFIG_BACKUP_PATH APP_DIR "/camera.ini.web.bak"
#endif
#ifndef PASSWORD_PATH
#define PASSWORD_PATH APP_DIR "/web.pass"
#endif
#ifndef AUTHORIZED_KEYS_PATH
#define AUTHORIZED_KEYS_PATH APP_DIR "/auth/authorized_keys"
#endif
#ifndef RUNTIME_AUTHORIZED_KEYS_PATH
#define RUNTIME_AUTHORIZED_KEYS_PATH AUTHORIZED_KEYS_PATH
#endif
#ifndef RUNTIME_AUTHORIZED_KEYS_DIR
#define RUNTIME_AUTHORIZED_KEYS_DIR APP_DIR "/auth"
#endif
#ifndef USER_LOCK_PATH
#define USER_LOCK_PATH "/tmp/apcg-zt30/apcg-users.lock"
#endif
#ifndef UPGRADE_LOCK_PATH
#define UPGRADE_LOCK_PATH "/tmp/apcg-zt30/apcg-upgrade.lock"
#endif
#ifndef RUNTIME_DIR
#define RUNTIME_DIR "/tmp/apcg-zt30"
#endif
#ifndef APP_REQUEST_PATH
#define APP_REQUEST_PATH "/tmp/apcg-zt30/apcg-app-request"
#endif
#ifndef APP_REQUEST_LOCK_PATH
#define APP_REQUEST_LOCK_PATH "/tmp/apcg-zt30/apcg-app-request.lock"
#endif
#ifndef APP_STARTED_PATH
#define APP_STARTED_PATH "/tmp/apcg-zt30/apcg-app-started"
#endif
#ifndef CAMERA_READY_PATH
#define CAMERA_READY_PATH "/tmp/apcg-zt30/camera-app.ready"
#endif
#ifndef REPLACEMENT_CAMERA_PATH
#define REPLACEMENT_CAMERA_PATH APP_DIR "/camera-app"
#endif
#ifndef WEB_PATH
#define WEB_PATH APP_DIR "/zt30-web"
#endif
#ifndef REPLACEMENT_LOG_PATH
#define REPLACEMENT_LOG_PATH "/tmp/apcg-zt30/camera-app.log"
#endif
#ifndef REPLACEMENT_LOG_OLD_PATH
#define REPLACEMENT_LOG_OLD_PATH "/tmp/apcg-zt30/camera-app.log.1"
#endif
