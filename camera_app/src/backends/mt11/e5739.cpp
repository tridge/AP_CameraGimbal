#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "e5739.h"

#include "camera_app/log.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/spi/spidev.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <sys/mman.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define E5739_SPI_DEVICE "/dev/spidev2.0"
#define E5739_GPIO_DEVICE "/dev/ot_gpio"
#define E5739_SPI_MODE 0x0bU
#define E5739_SPI_BITS 8U
#define E5739_SPI_SPEED 4000000U
#define E5739_GPIO_BANK 5U
#define E5739_ZOOM_STOP_PIN 2U
#define E5739_FOCUS_STOP_PIN 3U
#define E5739_RESET_PIN 4U
#define E5739_AUX_PIN 5U
#define E5739_GATE_PIN 6U
#define E5739_HOME_POSITION 230
#define E5739_FOCUS_HOME_POSITION 427
#define E5739_HOME_COARSE_STEPS 6U
#define E5739_HOME_LIMIT 501U
#define E5739_MAX_TABLE_ROWS 2048U
#define E5739_CALIBRATION_COLUMNS 14U

/* These ioctl values and the three-word argument reproduce HAL_GPIO_*(). */
#define OT_GPIO_SET_DIR 0xc0087704UL
#define OT_GPIO_GET_VALUE 0xc0087206UL
#define OT_GPIO_SET_VALUE 0xc0087707UL

struct ot_gpio_argument {
    uint32_t bank;
    uint32_t pin;
    uint32_t value;
};

struct e5739_calibration {
    float factor;
    int zoom_position;
    int focus_position;
    int focus_minimum;
    int focus_maximum;
};

struct ca_e5739 {
    int spi_fd;
    int gpio_fd;
    struct e5739_calibration *table;
    size_t table_length;
    size_t current_row;
    int zoom_position;
    int focus_position;
    float optical_zoom;
};

static int gpio_direction(struct ca_e5739 *motor, unsigned pin, unsigned output)
{
    struct ot_gpio_argument argument = {
        .bank = E5739_GPIO_BANK,
        .pin = pin,
        .value = output,
    };
    return ioctl(motor->gpio_fd, OT_GPIO_SET_DIR, &argument);
}

static int gpio_set(struct ca_e5739 *motor, unsigned pin, unsigned value)
{
    struct ot_gpio_argument argument = {
        .bank = E5739_GPIO_BANK,
        .pin = pin,
        .value = value,
    };
    return ioctl(motor->gpio_fd, OT_GPIO_SET_VALUE, &argument);
}

static int gpio_get(struct ca_e5739 *motor, unsigned pin, unsigned *value)
{
    struct ot_gpio_argument argument = {
        .bank = E5739_GPIO_BANK,
        .pin = pin,
    };
    if (ioctl(motor->gpio_fd, OT_GPIO_GET_VALUE, &argument) < 0) return -1;
    *value = argument.value != 0U;
    return 0;
}

static int spi_write_register(struct ca_e5739 *motor, uint8_t address,
                              uint16_t value)
{
    uint8_t transmit[3] = {
        address,
        (uint8_t)value,
        (uint8_t)(value >> 8),
    };
    uint8_t receive[3] = {};
    struct spi_ioc_transfer transfer = {
        .tx_buf = (uintptr_t)transmit,
        .rx_buf = (uintptr_t)receive,
        .len = sizeof(transmit),
        .speed_hz = E5739_SPI_SPEED,
        .bits_per_word = E5739_SPI_BITS,
        .cs_change = 1,
    };
    int result = ioctl(motor->spi_fd, SPI_IOC_MESSAGE(1), &transfer);
    if (result != (int)sizeof(transmit)) {
        if (result >= 0) errno = EIO;
        return -1;
    }
    return 0;
}

static int gate_batch(struct ca_e5739 *motor)
{
    if (gpio_set(motor, E5739_GATE_PIN, 1U) < 0) return -1;
    if (usleep(9000U) < 0) return -1;
    return gpio_set(motor, E5739_GATE_PIN, 0U);
}

static int motor_step(struct ca_e5739 *motor, bool zoom, unsigned steps,
                      unsigned direction)
{
    uint16_t pulse_units;
    uint16_t period;
    uint16_t control;

    if (steps == 0U || steps > E5739_HOME_COARSE_STEPS || direction > 1U) {
        errno = EINVAL;
        return -1;
    }
    pulse_units = (uint16_t)(steps * 8U);
    period = (uint16_t)(224100U / ((unsigned)pulse_units * 24U));
    control = (uint16_t)(0x0c00U | (direction << 8U) | pulse_units);

    if (zoom) {
        if (spi_write_register(motor, 0x29U, control) < 0 ||
            spi_write_register(motor, 0x2aU, period) < 0 ||
            spi_write_register(motor, 0x24U, 0x0c00U) < 0 ||
            spi_write_register(motor, 0x25U, 0U) < 0) return -1;
        motor->zoom_position += direction != 0U ? (int)steps : -(int)steps;
    } else {
        if (spi_write_register(motor, 0x24U, control) < 0 ||
            spi_write_register(motor, 0x25U, period) < 0 ||
            spi_write_register(motor, 0x29U, 0x0c00U) < 0 ||
            spi_write_register(motor, 0x2aU, 0U) < 0) return -1;
        /* The focus channel's position polarity is opposite to zoom. */
        motor->focus_position += direction == 0U ? (int)steps : -(int)steps;
    }
    return gate_batch(motor);
}

// Writing 0x0c00 leaves both stepper outputs enabled, so the coils draw
// holding current indefinitely. Disable them once a move has finished; the
// lens gearing holds position.
static int release_outputs(struct ca_e5739 *motor)
{
    if (usleep(20000U) < 0 ||
        spi_write_register(motor, 0x24U, 0U) < 0 ||
        spi_write_register(motor, 0x25U, 0U) < 0 ||
        spi_write_register(motor, 0x29U, 0U) < 0 ||
        spi_write_register(motor, 0x2aU, 0U) < 0) return -1;
    return gate_batch(motor);
}

static int move_zoom_to(struct ca_e5739 *motor, int target)
{
    if (motor->zoom_position == target) return 0;
    while (motor->zoom_position != target) {
        int difference = target - motor->zoom_position;
        unsigned steps = (unsigned)abs(difference);
        if (steps > E5739_HOME_COARSE_STEPS) steps = E5739_HOME_COARSE_STEPS;
        if (motor_step(motor, true, steps, difference > 0 ? 1U : 0U) < 0) {
            return -1;
        }
    }
    return release_outputs(motor);
}

static int move_focus_to(struct ca_e5739 *motor, int target)
{
    if (motor->focus_position == target) return 0;
    while (motor->focus_position != target) {
        int difference = target - motor->focus_position;
        unsigned steps = (unsigned)abs(difference);
        if (steps > E5739_HOME_COARSE_STEPS) steps = E5739_HOME_COARSE_STEPS;
        if (motor_step(motor, false, steps, difference > 0 ? 0U : 1U) < 0) {
            return -1;
        }
    }
    return release_outputs(motor);
}

static int home_channel(struct ca_e5739 *motor, bool zoom, unsigned stop_pin,
                        int home_position, const char *name)
{
    unsigned initial;
    unsigned approach_direction;
    unsigned value;
    unsigned count;

    if (gpio_get(motor, stop_pin, &initial) < 0) return -1;
    /* On the SIYI board branch the two motor channels have opposite polarity. */
    approach_direction = zoom ? initial : (initial == 0U ? 1U : 0U);
    value = initial;
    for (count = 0; count < E5739_HOME_LIMIT && value == initial; count++) {
        if (motor_step(motor, zoom, E5739_HOME_COARSE_STEPS,
                       approach_direction) < 0 ||
            gpio_get(motor, stop_pin, &value) < 0) return -1;
    }
    if (value == initial) {
        ca_log("E5739 %s homing: no end-stop edge (input=%u direction=%u)", name, value, approach_direction);
        errno = ETIMEDOUT;
        return -1;
    }

    /* Cross cleanly, then approach the switch edge from the other direction. */
    for (count = 0; count < 5U; count++) {
        if (motor_step(motor, zoom, 1U, approach_direction) < 0) return -1;
    }
    for (count = 0; count < E5739_HOME_LIMIT && value != initial; count++) {
        if (motor_step(motor, zoom, 1U,
                       approach_direction == 0U ? 1U : 0U) < 0 ||
            gpio_get(motor, stop_pin, &value) < 0) return -1;
    }
    if (value != initial) {
        ca_log("E5739 %s homing: no return edge (input=%u)", name, value);
        errno = ETIMEDOUT;
        return -1;
    }
    if (zoom) motor->zoom_position = home_position;
    else motor->focus_position = home_position;
    ca_log("E5739 %s homed at position=%d switch=%u", name, home_position,
           value);
    return 0;
}

static int load_calibration(const char *path,
                            struct e5739_calibration **table_result,
                            size_t *length_result)
{
    struct e5739_calibration *table;
    FILE *file;
    size_t length = 0;

    file = fopen(path, "r");
    if (file == NULL) return -1;
    table = (e5739_calibration*)(calloc(E5739_MAX_TABLE_ROWS, sizeof(*table)));
    if (table == NULL) {
        fclose(file);
        return -1;
    }
    while (length < E5739_MAX_TABLE_ROWS) {
        float column[E5739_CALIBRATION_COLUMNS];
        unsigned parsed = 0;
        for (; parsed < E5739_CALIBRATION_COLUMNS; parsed++) {
            if (fscanf(file, "%f", &column[parsed]) != 1) break;
        }
        if (parsed == 0U) break;
        if (parsed != E5739_CALIBRATION_COLUMNS || !isfinite(column[0]) ||
            column[0] < 1.0f || !isfinite(column[3]) ||
            column[3] < 0.0f || column[3] > 1200.0f ||
            !isfinite(column[10]) || column[10] < 20.0f ||
            column[10] > 788.0f) {
            free(table);
            fclose(file);
            errno = EINVAL;
            return -1;
        }
        table[length].factor = column[0];
        table[length].zoom_position = (int)lroundf(column[3]);
        table[length].focus_position = (int)lroundf(column[10]);
        table[length].focus_minimum = table[length].focus_position;
        table[length].focus_maximum = table[length].focus_position;
        for (unsigned focus_column = 4U; focus_column <= 10U;
             focus_column++) {
            int position;
            if (!isfinite(column[focus_column]) ||
                column[focus_column] < 20.0f ||
                column[focus_column] > 788.0f) {
                free(table);
                fclose(file);
                errno = EINVAL;
                return -1;
            }
            position = (int)lroundf(column[focus_column]);
            if (position < table[length].focus_minimum) {
                table[length].focus_minimum = position;
            }
            if (position > table[length].focus_maximum) {
                table[length].focus_maximum = position;
            }
        }
        if (length != 0U &&
            (table[length].factor < table[length - 1U].factor ||
             table[length].zoom_position <= table[length - 1U].zoom_position)) {
            free(table);
            fclose(file);
            errno = EINVAL;
            return -1;
        }
        length++;
    }
    fclose(file);
    if (length == 0U) {
        free(table);
        errno = EINVAL;
        return -1;
    }
    *table_result = table;
    *length_result = length;
    return 0;
}

static size_t calibration_row(const struct e5739_calibration *table,
                              size_t length, float requested_zoom)
{
    size_t row = 0;
    while (row + 1U < length && table[row].factor < requested_zoom) row++;
    return row;
}

int ca_e5739_lookup_calibration(const char *path, float requested_zoom,
                                float *factor, int *zoom_position,
                                int *infinity_focus_position)
{
    struct e5739_calibration *table;
    size_t length;
    size_t row;
    if (path == NULL || factor == NULL || zoom_position == NULL ||
        infinity_focus_position == NULL || !isfinite(requested_zoom) ||
        requested_zoom < 1.0f) {
        errno = EINVAL;
        return -1;
    }
    if (load_calibration(path, &table, &length) < 0) return -1;
    row = calibration_row(table, length, requested_zoom);
    *factor = table[row].factor;
    *zoom_position = table[row].zoom_position;
    *infinity_focus_position = table[row].focus_position;
    free(table);
    return 0;
}

int ca_e5739_lookup_focus_range(const char *path, float requested_zoom,
                                int *minimum, int *maximum)
{
    struct e5739_calibration *table;
    size_t length;
    size_t row;

    if (path == NULL || minimum == NULL || maximum == NULL ||
        !isfinite(requested_zoom) || requested_zoom < 1.0f) {
        errno = EINVAL;
        return -1;
    }
    if (load_calibration(path, &table, &length) < 0) return -1;
    row = calibration_row(table, length, requested_zoom);
    *minimum = table[row].focus_minimum;
    *maximum = table[row].focus_maximum;
    free(table);
    return 0;
}

/* The boot image leaves these pads in peripheral mode. Configure the lens
 * SPI bus, motor GPIOs and end-stop inputs before attempting to home. */
static int configure_pins(struct ca_e5739 *motor)
{
    uint8_t address[2] = {}, revision = 0xff;
    struct i2c_msg messages[] = {
        {.addr = 0x50, .len = sizeof(address), .buf = address},
        {.addr = 0x50, .flags = I2C_M_RD, .len = 1, .buf = &revision},
    };
    struct i2c_rdwr_ioctl_data transfer = {.msgs = messages, .nmsgs = 2};
    int i2c = open("/dev/i2c-3", O_RDWR | O_CLOEXEC);
    bool board2 = i2c >= 0 && ioctl(i2c, I2C_RDWR, &transfer) == 2 && revision != 0xff;
    if (i2c >= 0) close(i2c);
    int fd = open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
    if (fd < 0) return -1;
    volatile uint32_t *pads = (volatile uint32_t*)(mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x102f0000));
    if (pads == MAP_FAILED) { int saved = errno; close(fd); errno = saved; return -1; }
    pads[0x50 / 4] = pads[0x54 / 4] = board2 ? 0 : 0x1200;
    pads[0x60 / 4] = 0x1250;
    pads[0x64 / 4] = pads[0x68 / 4] = 0x1200;
    pads[0x70 / 4] = pads[0x74 / 4] = pads[0x78 / 4] = 0x1253;
    pads[0x7c / 4] = 0x1053;
    (void)pads[0x7c / 4];
    munmap((void *)pads, 4096);
    volatile uint32_t *clock = (volatile uint32_t*)(mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x110d2000));
    int saved = errno;
    close(fd);
    if (clock == MAP_FAILED) { errno = saved; return -1; }
    clock[0x100 / 4] |= 0x10;
    (void)clock[0x100 / 4];
    munmap((void *)clock, 4096);
    struct ot_gpio_argument power = {.bank = 11, .pin = 0, .value = 1};
    if (ioctl(motor->gpio_fd, OT_GPIO_SET_DIR, &power) < 0 ||
        ioctl(motor->gpio_fd, OT_GPIO_SET_VALUE, &power) < 0) return -1;
    ca_log("E5739 lens pinmux and power configured board=%u", board2 ? 2U : 1U);
    return 0;
}

static int configure_controller(struct ca_e5739 *motor)
{
    uint8_t mode = E5739_SPI_MODE;
    uint8_t bits = E5739_SPI_BITS;
    uint32_t speed = E5739_SPI_SPEED;
    struct {
        uint8_t address;
        uint16_t value;
    } registers[] = {
        {0x20U, 0x3c02U}, {0x22U, 0x0001U}, {0x27U, 0x0001U},
        {0x23U, 0xc8c8U}, {0x24U, 0x0c00U}, {0x25U, 0x0000U},
        {0x28U, 0xc8c8U}, {0x29U, 0x0c00U}, {0x2aU, 0x0000U},
    };

    if (configure_pins(motor) < 0) return -1;
    struct ot_gpio_argument drive = {.bank = 12, .pin = 2};
    if (ioctl(motor->gpio_fd, OT_GPIO_GET_VALUE, &drive) < 0) return -1;
    if (!drive.value) registers[3].value = registers[6].value = 0xffff;
    if (ioctl(motor->spi_fd, SPI_IOC_WR_MODE, &mode) < 0 ||
        ioctl(motor->spi_fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
        ioctl(motor->spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0 ||
        gpio_direction(motor, E5739_ZOOM_STOP_PIN, 0U) < 0 ||
        gpio_direction(motor, E5739_FOCUS_STOP_PIN, 0U) < 0 ||
        gpio_direction(motor, E5739_RESET_PIN, 1U) < 0 ||
        gpio_direction(motor, E5739_AUX_PIN, 1U) < 0 ||
        gpio_direction(motor, E5739_GATE_PIN, 1U) < 0 ||
        gpio_set(motor, E5739_GATE_PIN, 0U) < 0 ||
        gpio_set(motor, E5739_RESET_PIN, 0U) < 0 || usleep(10000U) < 0 ||
        gpio_set(motor, E5739_RESET_PIN, 1U) < 0 || usleep(10000U) < 0 ||
        gpio_set(motor, E5739_GATE_PIN, 1U) < 0) return -1;
    for (size_t index = 0; index < sizeof(registers) / sizeof(registers[0]);
         index++) {
        if (spi_write_register(motor, registers[index].address,
                               registers[index].value) < 0) return -1;
    }
    return gpio_set(motor, E5739_GATE_PIN, 0U);
}

int ca_e5739_open(struct ca_e5739 **result, const char *calibration_path)
{
    struct ca_e5739 *motor;
    size_t first_row;

    if (result == NULL || calibration_path == NULL) {
        errno = EINVAL;
        return -1;
    }
    motor = (struct ca_e5739*)(calloc(1, sizeof(*motor)));
    if (motor == NULL) return -1;
    motor->spi_fd = -1;
    motor->gpio_fd = -1;
    if (load_calibration(calibration_path, &motor->table,
                         &motor->table_length) < 0) goto fail;
    motor->spi_fd = open(E5739_SPI_DEVICE, O_RDWR | O_CLOEXEC);
    if (motor->spi_fd < 0) goto fail;
    motor->gpio_fd = open(E5739_GPIO_DEVICE, O_RDWR | O_CLOEXEC);
    if (motor->gpio_fd < 0) goto fail;
    if (configure_controller(motor) < 0 ||
        home_channel(motor, false, E5739_FOCUS_STOP_PIN,
                     E5739_FOCUS_HOME_POSITION, "focus") < 0 ||
        sleep(3U) != 0U ||
        home_channel(motor, true, E5739_ZOOM_STOP_PIN,
                     E5739_HOME_POSITION, "zoom") < 0) goto fail;

    first_row = calibration_row(motor->table, motor->table_length, 1.0f);
    if (move_zoom_to(motor, motor->table[first_row].zoom_position) < 0 ||
        move_focus_to(motor, motor->table[first_row].focus_position) < 0) {
        goto fail;
    }
    motor->current_row = first_row;
    motor->optical_zoom = motor->table[first_row].factor;
    ca_log("E5739 ready calibration_rows=%zu optical=%.2fx zoom_position=%d",
           motor->table_length, motor->optical_zoom, motor->zoom_position);
    *result = motor;
    return 0;
fail:
    ca_log("E5739 initialization failed: %s", strerror(errno));
    ca_e5739_close(motor);
    return -1;
}

int ca_e5739_set_zoom(struct ca_e5739 *motor, float requested_zoom,
                       float *optical_zoom)
{
    size_t row;
    int focus_delta;
    int focus_target;

    if (motor == NULL || optical_zoom == NULL || !isfinite(requested_zoom) ||
        requested_zoom < 1.0f) {
        errno = EINVAL;
        return -1;
    }
    row = calibration_row(motor->table, motor->table_length, requested_zoom);
    focus_delta = motor->table[row].focus_position -
                  motor->table[motor->current_row].focus_position;
    focus_target = motor->focus_position + focus_delta;
    if (focus_target < motor->table[row].focus_minimum) {
        focus_target = motor->table[row].focus_minimum;
    }
    if (focus_target > motor->table[row].focus_maximum) {
        focus_target = motor->table[row].focus_maximum;
    }
    if (move_zoom_to(motor, motor->table[row].zoom_position) < 0) return -1;

    /* Preserve the prior focus distance while following the factory curve. */
    if (motor->focus_position != focus_target &&
        move_focus_to(motor, focus_target) < 0) {
        return -1;
    }
    motor->current_row = row;
    motor->optical_zoom = motor->table[row].factor;
    *optical_zoom = motor->optical_zoom;
    ca_log("E5739 optical zoom=%.2fx requested=%.2fx zoom_position=%d "
           "focus_delta=%d", motor->optical_zoom, requested_zoom,
           motor->zoom_position, focus_delta);
    return 0;
}

float ca_e5739_zoom(const struct ca_e5739 *motor)
{
    return motor != NULL ? motor->optical_zoom : 1.0f;
}

int ca_e5739_focus_range(const struct ca_e5739 *motor, int *minimum,
                         int *maximum, int *current)
{
    const struct e5739_calibration *calibration;

    if (motor == NULL || minimum == NULL || maximum == NULL ||
        current == NULL || motor->current_row >= motor->table_length) {
        errno = EINVAL;
        return -1;
    }
    calibration = &motor->table[motor->current_row];
    *minimum = calibration->focus_minimum;
    *maximum = calibration->focus_maximum;
    *current = motor->focus_position;
    return 0;
}

int ca_e5739_set_focus_position(struct ca_e5739 *motor, int position)
{
    const struct e5739_calibration *calibration;

    if (motor == NULL || motor->current_row >= motor->table_length) {
        errno = EINVAL;
        return -1;
    }
    calibration = &motor->table[motor->current_row];
    if (position < calibration->focus_minimum ||
        position > calibration->focus_maximum) {
        errno = ERANGE;
        return -1;
    }
    return move_focus_to(motor, position);
}

void ca_e5739_close(struct ca_e5739 *motor)
{
    if (motor == NULL) return;
    if (motor->spi_fd >= 0 && motor->gpio_fd >= 0) (void)release_outputs(motor);
    if (motor->gpio_fd >= 0) (void)gpio_set(motor, E5739_GATE_PIN, 0U);
    if (motor->spi_fd >= 0) close(motor->spi_fd);
    if (motor->gpio_fd >= 0) close(motor->gpio_fd);
    free(motor->table);
    free(motor);
}
