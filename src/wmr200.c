/*
 * wmr200.c:
 * Oregon Scientific WMR200 USB HID communication wrapper
 *
 * This software may be freely used and distributed according to the terms
 * of the GNU GPL version 2 or 3. See LICENSE for more information.
 *
 * Copyright (c) 2015 David Čepelík <cepelik@gymlit.cz>
 */

#include "common.h"
#include "log.h"
#include "wmr200.h"
#include "wmrdata.h"

#include <hidapi.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>


#define	NTH_BIT(n, val)		(((val) >> (n)) & 1)
#define	LOW(b)			((b) &  0xF)
#define	HIGH(b)			LOW((b) >> 4)

#define	VENDOR_ID		0x0FDE
#define	PRODUCT_ID		0xCA01

#define	HEARTBEAT_INTERVAL_SEC	30

#define	TENTH_OF_INCH		0.0254


enum command {
	HEARTBEAT = 0xD0,
	HISTORIC_DATA_NOTIF = 0xD1,
	HISTORIC_DATA = 0xD2,
	REQUEST_HISTORIC_DATA = 0xDA,
	LOGGER_DATA_ERASE = 0xDB,
	COMMUNICATION_STOP = 0xDF
};

enum sign {
	SIGN_POSITIVE = 0x0,
	SIGN_NEGATIVE = 0x8
};

struct wmr_handler {
	wmr_handler_t handler;
	void *arg;
	struct wmr_handler *next;
};

/*
 * flag-to-string arrays
 */

/* signal leves */
static const char *LEVEL_STRING[] = {
	"ok",
	"low"
};

/* status flags */
static const char *STATUS_STRING[] = {
	"ok",
	"failed"
};

/* forecast "icons" */
static const char *FORECAST_STRING[] = {
	"partly_cloudy-day", "rainy", "cloudy",
	"sunny", "clear", "snowy",
	"partly_cloudy-night"
};

/* wind direction */
static const char *WIND_DIR_STRING[] = {
	"N", "NNE", "NE", "ENE",
	"E", "ESE", "SE", "SSE",
	"S", "SSW", "SW", "WSW",
	"W", "WNW", "NW", "NNW"
};

/*
 * sending and receiving data
 */

static uchar read_byte(wmr200 *wmr)
{
	if (wmr->buf_avail == 0) {
		int ret = hid_read(wmr->dev, wmr->buf, WMR200_FRAME_SIZE);
		wmr->meta.num_frames++;

		if (ret != WMR200_FRAME_SIZE) {
			log_warning("Cannot read frame\n");
		}

		wmr->buf_avail = wmr->buf[0];
		wmr->buf_pos = 1;
	}

	wmr->meta.num_bytes++;
	wmr->buf_avail--;
	return (wmr->buf[wmr->buf_pos++]);
}

static void send_cmd_frame(wmr200 *wmr, uchar cmd)
{
	uchar data[WMR200_FRAME_SIZE] = { 0x01, cmd };
	int ret = hid_write(wmr->dev, data, WMR200_FRAME_SIZE);

	if (ret != WMR200_FRAME_SIZE) {
		fprintf(stderr, "hid_write: cannot send %02x command frame, "
			"return was %i", cmd, ret);

		exit(1);
	}
}

static void send_heartbeat(wmr200 *wmr)
{
	log_debug("Sending heartbeat to WMR200");
	send_cmd_frame(wmr, HEARTBEAT);
}

/*
 * data processing
 */

static time_t get_reading_time_from_packet(wmr200 *wmr)
{
	struct tm time = {
		.tm_year	= (2000 + wmr->packet[6]) - 1900,
		.tm_mon		= wmr->packet[5],
		.tm_mday	= wmr->packet[4],
		.tm_hour	= wmr->packet[3],
		.tm_min		= wmr->packet[2],
		.tm_sec		= 0,
		.tm_isdst	= -1
	};

	return (mktime(&time));
}

static void invoke_handlers(wmr200 *wmr, wmr_reading *reading)
{
	struct wmr_handler *handler = wmr->handler;
	while (handler != NULL) {
		handler->handler(reading, handler->arg);
		handler = handler->next;
	}
}

static void update_if_newer(wmr_reading *old, wmr_reading *new)
{
	if (new->time >= old->time) {
		*old = *new;
	}
}

static void process_wind_data(wmr200 *wmr, uchar *data)
{
	uint_t dir_flag = LOW(data[7]);
	float gust_speed = (256 * LOW(data[10]) + data[9]) / 10.0;
	float avg_speed	= (16 * LOW(data[11]) + HIGH(data[10])) / 10.0;
	float chill = data[12]; /* TODO verify the formula */

	wmr_reading reading = {
		.type = WMR_WIND,
		.time = get_reading_time_from_packet(wmr),
		.wind = {
			.dir = WIND_DIR_STRING[dir_flag],
			.gust_speed = gust_speed,
			.avg_speed = avg_speed,
			.chill = chill
		}
	};

	update_if_newer(&wmr->latest.wind, &reading);
	invoke_handlers(wmr, &reading);
}

static void process_rain_data(wmr200 *wmr, uchar *data)
{
	float rate = ((data[8] << 8) + data[7]) * TENTH_OF_INCH;
	float accum_hour = ((data[10] << 8) + data[9]) * TENTH_OF_INCH;
	float accum_24h	= ((data[12] << 8) + data[11]) * TENTH_OF_INCH;
	float accum_2007 = ((data[14] << 8) + data[13]) * TENTH_OF_INCH;

	wmr_reading reading = {
		.type = WMR_RAIN,
		.time = get_reading_time_from_packet(wmr),
		.rain = {
			.rate = rate,
			.accum_hour = accum_hour,
			.accum_24h = accum_24h,
			.accum_2007 = accum_2007
		}
	};

	update_if_newer(&wmr->latest.rain, &reading);
	invoke_handlers(wmr, &reading);
}

static void process_uvi_data(wmr200 *wmr, uchar *data)
{
	uint_t index = LOW(data[7]);

	wmr_reading reading = {
		.type = WMR_UVI,
		.time = get_reading_time_from_packet(wmr),
		.uvi = {
			.index = index
		}
	};

	update_if_newer(&wmr->latest.uvi, &reading);
	invoke_handlers(wmr, &reading);
}

static void process_baro_data(wmr200 *wmr, uchar *data)
{
	uint_t pressure		= 256 * LOW(data[8]) + data[7];
	uint_t alt_pressure	= 256 * LOW(data[10]) + data[9];
	uint_t forecast_flag	= HIGH(data[8]);

	wmr_reading reading = {
		.type = WMR_BARO,
		.time = get_reading_time_from_packet(wmr),
		.baro = {
			.pressure = pressure,
			.alt_pressure = alt_pressure,
			.forecast = FORECAST_STRING[forecast_flag]
		}
	};

	update_if_newer(&wmr->latest.baro, &reading);
	invoke_handlers(wmr, &reading);
}

static void process_temp_data(wmr200 *wmr, uchar *data)
{
	int sensor_id = LOW(data[7]);

	/* TODO */
	if (sensor_id > 1) {
		fprintf(stderr, "Unknown sensor, ID: %i\n", sensor_id);
		exit(1);
	}

	uint_t humidity   = data[10];
	uint_t heat_index = data[13];

	float temp = (256 * LOW(data[9]) + data[8]) / 10.0;
	if (HIGH(data[9]) == SIGN_NEGATIVE) temp = -temp;

	float dew_point = (256 * LOW(data[12]) + data[11]) / 10.0;
	if (HIGH(data[12]) == SIGN_NEGATIVE) dew_point = -dew_point;

	wmr_reading reading = {
		.type = WMR_TEMP,
		.time = get_reading_time_from_packet(wmr),
		.temp = {
			.humidity = humidity,
			.heat_index = heat_index,
			.temp = temp,
			.dew_point = dew_point,
			.sensor_id = sensor_id
		}
	};

	update_if_newer(&wmr->latest.temp[sensor_id], &reading);
	invoke_handlers(wmr, &reading);
}

static void process_status_data(wmr200 *wmr, uchar *data)
{
	uint_t wind_bat_flag = NTH_BIT(0, data[4]);
	uint_t temp_bat_flag = NTH_BIT(1, data[4]);
	uint_t rain_bat_flag = NTH_BIT(4, data[5]);
	uint_t uv_bat_flag = NTH_BIT(5, data[5]);

	uint_t wind_sensor_flag = NTH_BIT(0, data[2]);
	uint_t temp_sensor_flag = NTH_BIT(1, data[2]);
	uint_t rain_sensor_flag = NTH_BIT(4, data[3]);
	uint_t uv_sensor_flag = NTH_BIT(5, data[3]);

	uint_t rtc_signal_flag = NTH_BIT(8, data[4]);

	wmr_reading reading = {
		.type = WMR_STATUS,
		.time = get_reading_time_from_packet(wmr),
		.status = {
			.wind_bat = LEVEL_STRING[wind_bat_flag],
			.temp_bat = LEVEL_STRING[temp_bat_flag],
			.rain_bat = LEVEL_STRING[rain_bat_flag],
			.uv_bat = LEVEL_STRING[uv_bat_flag],

			.wind_sensor = STATUS_STRING[wind_sensor_flag],
			.temp_sensor = STATUS_STRING[temp_sensor_flag],
			.rain_sensor = STATUS_STRING[rain_sensor_flag],
			.uv_sensor = STATUS_STRING[uv_sensor_flag],

			.rtc_signal_level = LEVEL_STRING[rtc_signal_flag]
		}
	};

	update_if_newer(&wmr->latest.status, &reading);
	invoke_handlers(wmr, &reading);
}

static void process_historic_data(wmr200 *wmr, uchar *data)
{
	process_rain_data(wmr, data);
	process_wind_data(wmr, data + 13);
	process_uvi_data(wmr, data + 20);
	process_baro_data(wmr, data + 21);
	process_temp_data(wmr, data + 26);

	uint_t ext_sensor_count = data[32];
	if (ext_sensor_count > WMR200_MAX_TEMP_SENSORS) {
		log_warning("process_historic_data: too many external sensors, "
			"skipping extraneous sensors");
	}
	ext_sensor_count = MIN(ext_sensor_count, WMR200_MAX_TEMP_SENSORS);

	for (uint_t i = 0; i < ext_sensor_count; i++) {
		process_temp_data(wmr, data + 33 + (7 * i));
	}
}

static void emit_meta_packet(wmr200 *wmr)
{
	log_debug("Emitting system META packet 0x%02X", WMR_META);

	wmr->meta.uptime = time(NULL) - wmr->conn_since;

	wmr_reading reading = {
		.time = time(NULL),
		.type = WMR_META,
		.meta = wmr->meta,
	};

	wmr->latest.meta = reading;
	invoke_handlers(wmr, &reading);
}

/*
 * packet processing
 */

static uint_t verify_packet(wmr200 *wmr)
{
	if (wmr->packet_len <= 2) {
		return (-1);
	}

	uint_t sum = 0;
	for (uint_t i = 0; i < wmr->packet_len - 2; i++) {
		sum += wmr->packet[i];
	}

	uint_t checksum = 256 * wmr->packet[wmr->packet_len - 1]
			+ wmr->packet[wmr->packet_len - 2];

	if (sum != checksum) {
		return (-1);
	}

	/* verify packet_len */

	return (0);
}

static void dispatch_packet(wmr200 *wmr)
{
	switch (wmr->packet_type) {
	case HISTORIC_DATA:
		process_historic_data(wmr, wmr->packet);
		break;

	case WMR_WIND:
		process_wind_data(wmr, wmr->packet);
		break;

	case WMR_RAIN:
		process_rain_data(wmr, wmr->packet);
		break;

	case WMR_UVI:
		process_uvi_data(wmr, wmr->packet);
		break;

	case WMR_BARO:
		process_baro_data(wmr, wmr->packet);
		break;

	case WMR_TEMP:
		process_temp_data(wmr, wmr->packet);
		break;

	case WMR_STATUS:
		process_status_data(wmr, wmr->packet);
		break;

	default:
		log_warning("Ignoring unknown packet 0x%02X", wmr->packet_type);
	}
}

static void mainloop(wmr200 *wmr)
{
	while (1) {
		wmr->packet_type = read_byte(wmr);

act_on_packet_type:
		switch (wmr->packet_type) {
		case HISTORIC_DATA_NOTIF:
			log_info("Data logger contains some unprocessed "
				"historic records");
			log_info("Issuing REQUEST_HISTORIC_DATA command");

			send_cmd_frame(wmr, REQUEST_HISTORIC_DATA);
			continue;

		case LOGGER_DATA_ERASE:
			log_info("Data logger database purge successful");
			continue;

		case COMMUNICATION_STOP:
			/* ignore, response to prev COMMUNICATION_STOP packet */
			log_debug("Ignoring COMMUNICATION_STOP packet");
			break;
		}

		wmr->packet_len = read_byte(wmr);
		if (wmr->packet_len >= 0xD0 && wmr->packet_len <= 0xDF) {
			/* this is packet type mark, not packet length */
			wmr->packet_type = wmr->packet_len;
			goto act_on_packet_type;
		}

		wmr->packet = malloc_safe(wmr->packet_len);
		wmr->packet[0] = wmr->packet_type;
		wmr->packet[1] = wmr->packet_len;

		for (uint_t i = 2; i < wmr->packet_len; i++) {
			wmr->packet[i] = read_byte(wmr);
		}

		wmr->meta.num_packets++;

		if (verify_packet(wmr) != 0) {
			log_warning("Received incorrect packet, dropping");
			wmr->meta.num_failed++;
			continue;
		}

		log_debug("Packet 0x%02X (%u bytes)", wmr->packet_type,
			wmr->packet_len);

		wmr->meta.latest_packet = time(NULL);
		dispatch_packet(wmr);

		free(wmr->packet);
	}
}

void *mainloop_pthread(void *arg)
{
	wmr200 *wmr = (wmr200 *)arg;
	mainloop(wmr); /* TODO register any cleanup handlers here? */

	return NULL;
}

static void heartbeat_loop(wmr200 *wmr)
{
	for (;;) {
		send_heartbeat(wmr);
		emit_meta_packet(wmr);

		usleep(HEARTBEAT_INTERVAL_SEC * 1e6);
	}
}

static void *heartbeat_loop_pthread(void *arg)
{
	wmr200 *wmr = (wmr200 *)arg;
	heartbeat_loop(wmr);
	
	return NULL;
}

/*
 * public interface
 */

wmr200 *wmr_open(void)
{
	wmr200 *wmr = malloc_safe(sizeof (wmr200));

	wmr->dev = hid_open(VENDOR_ID, PRODUCT_ID, NULL);
	if (wmr->dev == NULL) {
		log_error("hid_open: cannot connect to WMR200");
		return (NULL);
	}

	wmr->packet = NULL;
	wmr->buf_avail = wmr->buf_pos = 0;
	wmr->handler = NULL;
	wmr->conn_since = time(NULL);
	memset(&wmr->latest, 0, sizeof (wmr->latest));
	memset(&wmr->meta, 0, sizeof (wmr->meta));

	/* some kind of a wake-up command */
	uchar abracadabra[8] = {
		0x20, 0x00, 0x08, 0x01,
		0x00, 0x00, 0x00, 0x00
	};

	if (hid_write(wmr->dev, abracadabra, 8) != 8) {
		log_error("hid_write: cannot initialize communication with "
			"WMR200");
		return (NULL);
	}

	return (wmr);
}

void wmr_close(wmr200 *wmr)
{
	if (wmr->dev != NULL) {
		send_cmd_frame(wmr, COMMUNICATION_STOP);
		hid_close(wmr->dev);
	}

	free(wmr);
}

void wmr_init(void)
{
	hid_init();
}

void wmr_end(void)
{
	hid_exit();
}

int wmr_start(wmr200 *wmr)
{
	if (pthread_create(&wmr->heartbeat_thread,
		NULL, heartbeat_loop_pthread, wmr) != 0) {
		log_error("Cannot start heartbeat loop thread");
		return (-1);
	}

	if (pthread_create(&wmr->mainloop_thread,
		NULL, mainloop_pthread, wmr) != 0) {
		log_error("Cannot start main communication loop thread");
		return (-1);
	}

	send_cmd_frame(wmr, LOGGER_DATA_ERASE);

	log_info("wmr_start was succesfull");
	return (0);
}

void wmr_stop(wmr200 *wmr)
{
	pthread_cancel(wmr->heartbeat_thread);
	pthread_cancel(wmr->mainloop_thread);

	pthread_join(wmr->heartbeat_thread, NULL);
	pthread_join(wmr->mainloop_thread, NULL);
}

void wmr_add_handler(wmr200 *wmr, wmr_handler_t handler, void *arg)
{
	struct wmr_handler *wh = malloc_safe(sizeof (struct wmr_handler));
	wh->handler = handler;
	wh->arg = arg;
	wh->next = wmr->handler;
	wmr->handler = wh;
}
