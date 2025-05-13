/*
 * License: Apache-2.0.
 */

#ifndef DIGIT_RECOGNIZER_NANO_USBCAM_INTERFACE_H
#define DIGIT_RECOGNIZER_NANO_USBCAM_INTERFACE_H

#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 1000 msec = 1 sec */
#define SLEEP_TIME_MS	 1000
#define ZEPHYR_USER_NODE DT_PATH(zephyr_user)
#define SPI_NODE	 DT_NODELABEL(spi2)

/* Capture JPEG image from camera into buffer.
 * Returns 0 on success, or negative error code.
 */
int capture_jpeg_frame(uint8_t *buffer, size_t max_size, size_t *actual_size);

#ifdef __cplusplus
}
#endif

#endif /* DIGIT_RECOGNIZER_NANO_USBCAM_INTERFACE_H */
