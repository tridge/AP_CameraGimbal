/* Z1-Mini web controls use the same MAVLink service as flight clients. */
#ifndef Z1MINI_WEB_MAVLINK_H
#define Z1MINI_WEB_MAVLINK_H
#include <all/mavlink.h>

static int z1_web_socket(unsigned port)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct timeval timeout = {.tv_sec = 0, .tv_usec = 350000};
    struct sockaddr_in peer = {.sin_family = AF_INET, .sin_port = htons(port),
                              .sin_addr = {.s_addr = htonl(INADDR_LOOPBACK)}};
    if (fd >= 0 && (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ||
                   connect(fd, (struct sockaddr *)&peer, sizeof(peer)))) {
        close(fd); fd = -1;
    }
    return fd;
}
static bool z1_web_send(int fd, const mavlink_message_t *msg)
{
    uint8_t buffer[MAVLINK_MAX_PACKET_LEN];
    uint16_t n = mavlink_msg_to_send_buffer(buffer, msg);
    size_t sent = 0;
    while (sent < n) {
        ssize_t count = send(fd, buffer + sent, n - sent, MSG_NOSIGNAL);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        sent += (size_t)count;
    }
    return true;
}
static bool z1_web_attitude(unsigned port, float *roll, float *pitch, float *yaw)
{
    int fd = z1_web_socket(port);
    if (fd < 0) return false;
    mavlink_message_t message;
    mavlink_msg_command_long_pack(255, 191, &message, 0, 154,
        MAV_CMD_REQUEST_MESSAGE, 0, MAVLINK_MSG_ID_GIMBAL_DEVICE_ATTITUDE_STATUS, 0, 0, 0, 0, 0, 0);
    bool ok = false;
    mavlink_status_t status = {};
    if (!z1_web_send(fd, &message)) goto done;
    for (unsigned attempt = 0; attempt < 8; attempt++) {
        uint8_t buffer[MAVLINK_MAX_PACKET_LEN * 2];
        ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
        if (n <= 0) break;
        for (ssize_t i = 0; i < n; i++) {
            if (!mavlink_parse_char(0, buffer[i], &message, &status) ||
                message.msgid != MAVLINK_MSG_ID_GIMBAL_DEVICE_ATTITUDE_STATUS) continue;
            mavlink_gimbal_device_attitude_status_t a;
            mavlink_msg_gimbal_device_attitude_status_decode(&message, &a);
            const float *q = a.q;
            if (!isfinite(q[0]) || !isfinite(q[1]) || !isfinite(q[2]) || !isfinite(q[3])) goto done;
            *roll = atan2f(2 * (q[0]*q[1] + q[2]*q[3]), 1 - 2 * (q[1]*q[1] + q[2]*q[2]));
            *pitch = asinf(fminf(1, fmaxf(-1, 2 * (q[0]*q[2] - q[3]*q[1]))));
            *yaw = atan2f(2 * (q[0]*q[3] + q[1]*q[2]), 1 - 2 * (q[2]*q[2] + q[3]*q[3]));
            ok = true; goto done;
        }
    }
done:
    close(fd); return ok;
}
#endif
