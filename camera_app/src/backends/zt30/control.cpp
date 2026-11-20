/* Shared short-UART control; native DEV policy enables only qualified gimbal
 * commands. Other native features remain unavailable pending integration. */
#if !defined(CAMERA_APP_SITL) && !defined(CAMERA_APP_HOST) && !defined(CAMERA_APP_ZT30_DEV)
#error "ZT30 native control is not qualified; build zt30_sitl instead"
#endif
#include "../siyi/control.cpp"
