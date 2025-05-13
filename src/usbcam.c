/*
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "usbcam.h"

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/video.h>
#include <zephyr/kernel.h>
#include <zephyr/usb/usb_device.h>

BUILD_ASSERT(DT_NODE_HAS_COMPAT(DT_CHOSEN(zephyr_console), zephyr_cdc_acm_uart),
	     "Console device is not ACM CDC UART device");

static const struct gpio_dt_spec cs_gpio = GPIO_DT_SPEC_GET(ZEPHYR_USER_NODE, cs_gpios);

static uint8_t data;

static int write_bytes(const struct device *spi, struct spi_config *spi_cfg, uint8_t addr,
		       uint8_t *data, uint32_t num_bytes)
{
	uint8_t cmd[] = {addr | 0x80, *data};
	const struct spi_buf tx_buf = {.buf = cmd, .len = sizeof(cmd)};
	const struct spi_buf_set tx = {.buffers = &tx_buf, .count = 1};

	return spi_write(spi, spi_cfg, &tx);
}

static int read_bytes(const struct device *spi, struct spi_config *spi_cfg, uint8_t addr,
		      uint8_t *data, uint32_t num_bytes)
{
	const struct spi_buf tx_buf = {.buf = &addr, .len = 1};
	const struct spi_buf_set tx = {.buffers = &tx_buf, .count = 1};
	struct spi_buf rx_buf[2];
	const struct spi_buf_set rx = {.buffers = rx_buf, .count = ARRAY_SIZE(rx_buf)};

	rx_buf[0].buf = NULL;
	rx_buf[0].len = 1;

	addr = addr & 0x7f;
	rx_buf[1].buf = &data[0];
	rx_buf[1].len = 1;

	return spi_transceive(spi, spi_cfg, &tx, &rx);
}

uint32_t read_fifo_length(const struct device *spi, struct spi_config *spi_cfg)
{
	uint32_t len1, len2, len3, length = 0;
	uint8_t data;

	read_bytes(spi, spi_cfg, 0x42, &data, 1);
	len1 = data;
	len2 = read_bytes(spi, spi_cfg, 0x43, &data, 1);
	len2 = data;
	len3 = read_bytes(spi, spi_cfg, 0x44, &data, 1);
	len3 = data;
	length = ((len3 << 16) | (len2 << 8) | len1) & 0x07fffff;

	return length;
}

int capture_jpeg_frame(uint8_t *buffer, size_t max_size, size_t *actual_size)
{
	int ret;
	int err;
	struct video_format fmt;
	struct video_caps caps;
	int i = 0;
	uint32_t length;
	uint8_t temp = 0, lasttemp = 0;
	int idx = 0;
	const struct device *spi = device_get_binding("SPI2");
	const struct device *dev1 = DEVICE_DT_GET_ONE(ovti_ov2640);
	const struct device *video;
	const struct device *dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
	uint32_t dtr = 0;

	// TODO: some of this should probably be split into a different init function,
	// which would be called from setup().

	if (usb_enable(NULL)) {
		return -ENODEV;
	}

	/* Poll if the DTR flag was set */
	while (!dtr) {
		uart_line_ctrl_get(dev, UART_LINE_CTRL_DTR, &dtr);
		/* Give CPU resources to low priority threads. */
		k_sleep(K_MSEC(100));
	}

	if (dev1 == NULL) {
		printk("\nError: no device found.\n");
		return -ENODEV;
	}

	if (!device_is_ready(dev1)) {
		printk("\nError: Device \"%s\" is not ready; "
		       "check the driver initialization logs for errors.\n",
		       dev1->name);
		return -ENODEV;
	}

	printk("Found device \"%s\", getting sensor data\n", dev1->name);
	video = dev1;
	struct spi_config spi_cfg = {0};
	if (!device_is_ready(spi)) {
		printk("SPI device %s is not ready\n", spi->name);
		return -ENODEV;
	}
	/* Get capabilities */
	if (video_get_caps(video, VIDEO_EP_OUT, &caps)) {
		printk("Unable to retrieve video capabilities");
		return -ENODEV;
	}

	printk("- Capabilities:\n");
	while (caps.format_caps[i].pixelformat) {
		const struct video_format_cap *fcap = &caps.format_caps[i];
		/* fourcc to string */
		printk("  %c%c%c%c width [%u; %u; %u] height [%u; %u; %u]\n",
		       (char)fcap->pixelformat, (char)(fcap->pixelformat >> 8),
		       (char)(fcap->pixelformat >> 16), (char)(fcap->pixelformat >> 24),
		       fcap->width_min, fcap->width_max, fcap->width_step, fcap->height_min,
		       fcap->height_max, fcap->height_step);
		i++;
	}

	/* set default/init format SVGA RGB565 */
	fmt.pixelformat = VIDEO_PIX_FMT_JPEG;
	fmt.width = 320;
	fmt.height = 240;
	fmt.pitch = 320 * 2;

	/*fmt.pixelformat = VIDEO_PIX_FMT_RGB565;
	fmt.width = 160;
	fmt.height = 120;
	fmt.pitch = 160*2;*/

	ret = video_set_format(video, VIDEO_EP_OUT, &fmt);
	if (ret) {
		printk("Unable to configure format");
		return -EIO;
	}
	/* Get default/native format */
	if (video_get_format(video, VIDEO_EP_OUT, &fmt)) {
		printk("Unable to retrieve video format");
		return -EIO;
	}

	printk("- Default format: %c%c%c%c %ux%u\n", (char)fmt.pixelformat,
	       (char)(fmt.pixelformat >> 8), (char)(fmt.pixelformat >> 16),
	       (char)(fmt.pixelformat >> 24), fmt.width, fmt.height);

	k_msleep(SLEEP_TIME_MS * 5);

	gpio_pin_configure_dt(&cs_gpio, GPIO_OUTPUT_ACTIVE);

	spi_cfg.operation = SPI_WORD_SET(8);
	spi_cfg.frequency = 4000000U;
	spi_cfg.slave = 0;
	struct spi_cs_control cs_ctrl = (struct spi_cs_control){
		.gpio = GPIO_DT_SPEC_GET(ZEPHYR_USER_NODE, cs_gpios),
		.delay = 0u,
	};
	spi_cfg.cs = &cs_ctrl;

	data = 0x55;
	write_bytes(spi, &spi_cfg, 0x00, &data, 1);
	read_bytes(spi, &spi_cfg, 0x00, &data, 1);
	if (data != 0x55) {
		printk("Error to writing and reading reg 0x00\n");
		return -EIO;
	}

	data = 0x01;
	write_bytes(spi, &spi_cfg, 0x04, &data, 1);

	data = 0x01;
	err = write_bytes(spi, &spi_cfg, 0x04, &data, 1);

	data = 0x02;
	err = write_bytes(spi, &spi_cfg, 0x04, &data, 1);

	data = 0;
	read_bytes(spi, &spi_cfg, 0x41, &data, 1);
	while (!(data & 0x08)) {
		read_bytes(spi, &spi_cfg, 0x41, &data, 1);
	}

	printk("Frame captured");

	length = read_fifo_length(spi, &spi_cfg);
	printk("Data length=%d\n", length);
	if (length > max_size) {
		printk("Frame too large: %u > %zu\n", length, max_size);
		return -ENOMEM;
	}

	// NOTE: previous loop threw out the 1st byte and didn't check for length.
	// TODO: check if this one works well.
	while (1) {
		lasttemp = temp;
		read_bytes(spi, &spi_cfg, 0x3C, &temp, 1);
		buffer[index++] = temp;

		if (lasttemp == 0xFF && temp == 0xD9) {
			break;
		}

		if (index >= max_size) {
			printk("Buffer overflow\n");
			return -ENOMEM;
		}
	}
}
