/*
 * libdivecomputer
 *
 * Copyright (C) 2010 Jef Driesen
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301 USA
 */

#include <stdlib.h>
#include <stdio.h>	/* for snprintf */
#include <string.h>	/* for strdup */

#include <libdivecomputer/units.h>

#include "mares_iconhd.h"
#include "context-private.h"
#include "parser-private.h"
#include "array.h"
#include "checksum.h"

#define ISINSTANCE(parser) dc_parser_isinstance((parser), &mares_iconhd_parser_vtable)

#define OBJVERSION(major,minor) ( \
		(((major) & 0xFF) << 8) | \
		((minor) & 0xFF))

#define UNSUPPORTED 0xFFFFFFFF

#define SMART      0x000010
#define SMARTAPNEA 0x010010
#define ICONHD    0x14
#define ICONHDNET 0x15
#define GENIUS    0x1C
#define QUADAIR   0x23
#define SMARTAIR  0x24
#define HORIZON   0x2C

#define NGASMIXES_ICONHD 3
#define NGASMIXES_GENIUS 5
#define NGASMIXES        NGASMIXES_GENIUS

#define NTANKS_ICONHD NGASMIXES_ICONHD
#define NTANKS_GENIUS NGASMIXES_GENIUS
#define NTANKS        NGASMIXES

#define ICONHD_AIR       0
#define ICONHD_GAUGE     1
#define ICONHD_NITROX    2
#define ICONHD_FREEDIVE  3

#define GENIUS_AIR           0
#define GENIUS_NITROX_SINGLE 1
#define GENIUS_NITROX_MULTI  2
#define GENIUS_TRIMIX        3
#define GENIUS_GAUGE         4
#define GENIUS_FREEDIVE      5
#define GENIUS_SCR           6
#define GENIUS_OC            7

// Record types and sizes
#define DSTR_TYPE 0x44535452 // Dive start record
#define DSTR_SIZE 58
#define TISS_TYPE 0x54495353 // Tissue record
#define TISS_SIZE 138
#define DPRS_TYPE 0x44505253 // Sample record
#define DPRS_SIZE 34
#define SDPT_TYPE 0x53445054 // SCR sample record
#define SDPT_SIZE 78
#define AIRS_TYPE 0x41495253 // Air integration record
#define AIRS_SIZE 16
#define DEND_TYPE 0x44454E44 // Dive end record
#define DEND_SIZE 162

#define GASMIX_OFF   0
#define GASMIX_READY 1
#define GASMIX_INUSE 2
#define GASMIX_IGNRD 3

#define WATER_FRESH   0
#define WATER_SALT    1
#define WATER_EN13319 2

#define ALARM_NONE                 0
#define ALARM_SLOW_DOWN            1
#define ALARM_FAST_ASCENT          2
#define ALARM_UNCONTROLLED_ASCENT  3
#define ALARM_MOD_REACHED          4
#define ALARM_CNS_DANGER           5
#define ALARM_CNS_EXTREME          6
#define ALARM_MISSED_DECO          7
#define ALARM_DIVE_VIOLATION_DECO  8
#define ALARM_LOW_BATTERY          9
#define ALARM_VERY_LOW_BATTERY     10
#define ALARM_PROBE_LOW_BATTERY    11
#define ALARM_LOW_TANK_PRESSURE    12
#define ALARM_TANK_RESERVE_REACHED 13
#define ALARM_TANK_LOST_LINK       14
#define ALARM_MAX_DIVE_DEPTH       15
#define ALARM_RUN_AWAY_DECO        16
#define ALARM_TANK_HALF_REACHED    17
#define ALARM_NODECO_2MIN          18
#define ALARM_NODECO_DECO          19
#define ALARM_MULTIGAS_ATANKISLOW  20
#define ALARM_DIVETIME_HALFTIME    21
#define ALARM_DIVETIME_FULLTIME    22
#define ALARM_GAS_SWITCHPOINT      23
#define ALARM_GAS_IGNORED          24
#define ALARM_GAS_CHANGED          25
#define ALARM_GAS_NOTCHANGED       26
#define ALARM_GAS_ADDED            27

typedef struct mares_iconhd_parser_t mares_iconhd_parser_t;

typedef struct mares_iconhd_layout_t {
	unsigned int settings;
	unsigned int datetime;
	unsigned int divetime;
	unsigned int maxdepth;
	unsigned int atmospheric;
	unsigned int atmospheric_divisor;
	unsigned int temperature_min;
	unsigned int temperature_max;
	unsigned int gasmixes;
	unsigned int tanks;
} mares_iconhd_layout_t;

typedef struct mares_iconhd_gasmix_t {
	unsigned int oxygen;
	unsigned int helium;
} mares_iconhd_gasmix_t;

typedef struct mares_iconhd_tank_t {
	unsigned int volume;
	unsigned int workpressure;
	unsigned int beginpressure;
	unsigned int endpressure;
} mares_iconhd_tank_t;

struct mares_iconhd_parser_t {
	dc_parser_t base;
	unsigned int model;
	// Cached fields.
	unsigned int cached;
	unsigned int logformat;
	unsigned int mode;
	unsigned int nsamples;
	unsigned int samplesize;
	unsigned int headersize;
	unsigned int settings;
	unsigned int surftime;
	unsigned int interval;
	unsigned int samplerate;
	unsigned int ntanks;
	unsigned int ngasmixes;
	unsigned int serial;
	mares_iconhd_gasmix_t gasmix[NGASMIXES];
	mares_iconhd_tank_t tank[NTANKS];
	const mares_iconhd_layout_t *layout;
};

static const mares_iconhd_layout_t iconhd = {
	0x0C, /* settings */
	0x02, /* datetime */
	UNSUPPORTED, /* divetime */
	0x00, /* maxdepth */
	0x22, 8, /* atmospheric */
	0x42, /* temperature_min */
	0x44, /* temperature_max */
	0x10, /* gasmixes */
	UNSUPPORTED, /* tanks */
};

static const mares_iconhd_layout_t iconhdnet = {
	0x0C, /* settings */
	0x02, /* datetime */
	UNSUPPORTED, /* divetime */
	0x00, /* maxdepth */
	0x22, 8, /* atmospheric */
	0x42, /* temperature_min */
	0x44, /* temperature_max */
	0x10, /* gasmixes */
	0x58, /* tanks */
};

static const mares_iconhd_layout_t smartair = {
	0x0C, /* settings */
	0x02, /* datetime */
	UNSUPPORTED, /* divetime */
	0x00, /* maxdepth */
	0x22, 8, /* atmospheric */
	0x42, /* temperature_min */
	0x44, /* temperature_max */
	0x10, /* gasmixes */
	0x5C, /* tanks */
};

static const mares_iconhd_layout_t smartapnea = {
	0x1C, /* settings */
	0x40, /* datetime */
	0x24, /* divetime */
	0x3A, /* maxdepth */
	0x38, 1, /* atmospheric */
	0x3E, /* temperature_min */
	0x3C, /* temperature_max */
	UNSUPPORTED, /* gasmixes */
	UNSUPPORTED, /* tanks */
};

static const mares_iconhd_layout_t smart_freedive = {
	0x08, /* settings */
	0x20, /* datetime */
	0x0C, /* divetime */
	0x1A, /* maxdepth */
	0x18, 1, /* atmospheric */
	0x1C, /* temperature_min */
	0x1E, /* temperature_max */
	UNSUPPORTED, /* gasmixes */
	UNSUPPORTED, /* tanks */
};

static const mares_iconhd_layout_t smartair_freedive = {
	0x08, /* settings */
	0x22, /* datetime */
	0x0E, /* divetime */
	0x1C, /* maxdepth */
	0x1A, 1, /* atmospheric */
	0x20, /* temperature_min */
	0x1E, /* temperature_max */
	UNSUPPORTED, /* gasmixes */
	UNSUPPORTED, /* tanks */
};

static const mares_iconhd_layout_t genius = {
	0x0C, /* settings */
	0x08, /* datetime */
	UNSUPPORTED, /* divetime */
	0x22, /* maxdepth */
	0x3E, 1, /* atmospheric */
	0x28, /* temperature_min */
	0x26, /* temperature_max */
	0x54, /* gasmixes */
	0x54, /* tanks */
};

static const mares_iconhd_layout_t horizon = {
	0x0C, /* settings */
	0x08, /* datetime */
	UNSUPPORTED, /* divetime */
	0x22 + 8, /* maxdepth */
	0x3E + 8, 1, /* atmospheric */
	0x28 + 8, /* temperature_min */
	0x26 + 8, /* temperature_max */
	0x54 + 8, /* gasmixes */
	0x54 + 8, /* tanks */
};

static dc_status_t mares_iconhd_parser_set_data (dc_parser_t *abstract, const unsigned char *data, unsigned int size);
static dc_status_t mares_iconhd_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime);
static dc_status_t mares_iconhd_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value);
static dc_status_t mares_iconhd_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata);

static const dc_parser_vtable_t mares_iconhd_parser_vtable = {
	sizeof(mares_iconhd_parser_t),
	DC_FAMILY_MARES_ICONHD,
	mares_iconhd_parser_set_data, /* set_data */
	mares_iconhd_parser_get_datetime, /* datetime */
	mares_iconhd_parser_get_field, /* fields */
	mares_iconhd_parser_samples_foreach, /* samples_foreach */
	NULL /* destroy */
};

static unsigned int
mares_genius_isvalid (const unsigned char data[], size_t size, unsigned int type)
{
	if (size < 10) {
		return 0;
	}

	unsigned int head = array_uint32_be(data);
	unsigned int tail = array_uint32_be(data + size - 4);
	if (head != type || tail != type) {
		return 0;
	}

	unsigned short crc = array_uint16_le(data + size - 6);
	unsigned short ccrc = checksum_crc16_ccitt(data + 4, size - 10, 0x0000);
	if (crc != ccrc) {
		return 0;
	}

	return 1;
}

static dc_status_t
mares_iconhd_cache (mares_iconhd_parser_t *parser)
{
	dc_parser_t *abstract = (dc_parser_t *) parser;
	const unsigned char *data = parser->base.data;
	unsigned int size = parser->base.size;

	unsigned int header = 0x5C;
	if (parser->model == ICONHDNET)
		header = 0x80;
	else if (parser->model == QUADAIR)
		header = 0x84;
	else if (parser->model == SMART || parser->model == SMARTAIR)
		header = 4; // Type and number of samples only!
	else if (parser->model == SMARTAPNEA)
		header = 6; // Type and number of samples only!

	if (size < 4) {
		ERROR (abstract->context, "Buffer overflow detected!");
		return DC_STATUS_DATAFORMAT;
	}

	unsigned int length = array_uint32_le (data);
	if (length < 4 + header || length > size) {
		ERROR (abstract->context, "Buffer overflow detected!");
		return DC_STATUS_DATAFORMAT;
	}

	// Get the number of samples in the profile data.
	unsigned int type = 0, nsamples = 0;
	if (parser->model == SMART || parser->model == SMARTAPNEA || parser->model == SMARTAIR) {
		type     = array_uint16_le (data + length - header + 2);
		nsamples = array_uint16_le (data + length - header + 0);
	} else {
		type     = array_uint16_le (data + length - header + 0);
		nsamples = array_uint16_le (data + length - header + 2);
	}

	// Get the dive mode.
	unsigned int mode = type & 0x03;

	// Get the header and sample size.
	unsigned int headersize = 0x5C;
	unsigned int samplesize = 8;
	const mares_iconhd_layout_t *layout = &iconhd;
	if (parser->model == ICONHDNET) {
		headersize = 0x80;
		samplesize = 12;
		layout = &iconhdnet;
	} else if (parser->model == QUADAIR) {
		headersize = 0x84;
		samplesize = 12;
		layout = &smartair;
	} else if (parser->model == SMART) {
		if (mode == ICONHD_FREEDIVE) {
			headersize = 0x2E;
			samplesize = 6;
			layout = &smart_freedive;
		} else {
			headersize = 0x5C;
			samplesize = 8;
			layout = &iconhd;
		}
	} else if (parser->model == SMARTAPNEA) {
		headersize = 0x50;
		samplesize = 14;
		layout = &smartapnea;
	} else if (parser->model == SMARTAIR) {
		if (mode == ICONHD_FREEDIVE) {
			headersize = 0x30;
			samplesize = 6;
			layout = &smartair_freedive;
		} else {
			headersize = 0x84;
			samplesize = 12;
			layout = &smartair;
		}
	}

	if (length < 4 + headersize) {
		ERROR (abstract->context, "Buffer overflow detected!");
		return DC_STATUS_DATAFORMAT;
	}

	const unsigned char *p = data + length - headersize;
	if (parser->model != SMART && parser->model != SMARTAPNEA && parser->model != SMARTAIR) {
		p += 4;
	}

	// Get the dive settings.
	unsigned int settings = array_uint16_le (p + layout->settings);

	// Get the sample interval.
	unsigned int interval = 0;
	unsigned int samplerate = 0;
	if (parser->model == SMARTAPNEA) {
		unsigned int idx = (settings & 0x0600) >> 9;
		interval = 1;
		samplerate = 1 << idx;
	} else {
		const unsigned int intervals[] = {1, 5, 10, 20};
		unsigned int idx = (settings & 0x0C00) >> 10;
		interval = intervals[idx];
		samplerate = 1;
	}

	// Calculate the total number of bytes for this dive.
	unsigned int nbytes = 4 + headersize + nsamples * samplesize;
	if (layout->tanks != UNSUPPORTED) {
		nbytes += (nsamples / 4) * 8;
	} else if (parser->model == SMARTAPNEA) {
		unsigned int divetime = array_uint32_le (p + 0x24);
		nbytes += divetime * samplerate * 2;
	}
	if (length != nbytes) {
		ERROR (abstract->context, "Calculated and stored size are not equal.");
		return DC_STATUS_DATAFORMAT;
	}

	// Gas mixes
	unsigned int ngasmixes = 0;
	mares_iconhd_gasmix_t gasmix[NGASMIXES_ICONHD] = {0};
	if (layout->gasmixes != UNSUPPORTED) {
		if (mode == ICONHD_GAUGE || mode == ICONHD_FREEDIVE) {
			ngasmixes = 0;
		} else if (mode == ICONHD_AIR) {
			gasmix[0].oxygen = 21;
			gasmix[0].helium = 0;
			ngasmixes = 1;
		} else {
			// Count the number of active gas mixes. The active gas
			// mixes are always first, so we stop counting as soon
			// as the first gas marked as disabled is found.
			ngasmixes = 0;
			while (ngasmixes < NGASMIXES_ICONHD) {
				unsigned int offset = layout->gasmixes + ngasmixes * 4;
				if (p[offset + 1] & 0x80)
					break;
				gasmix[ngasmixes].oxygen = p[offset];
				gasmix[ngasmixes].helium = 0;
				ngasmixes++;
			}
		}
	}

	// Tanks
	unsigned int ntanks = 0;
	mares_iconhd_tank_t tank[NTANKS_ICONHD] = {0};
	if (layout->tanks != UNSUPPORTED) {
		unsigned int tankoffset = layout->tanks;
		while (ntanks < NTANKS_ICONHD) {
			tank[ntanks].volume        = array_uint16_le (p + tankoffset + 0x0C + ntanks * 8 + 0);
			tank[ntanks].workpressure  = array_uint16_le (p + tankoffset + 0x0C + ntanks * 8 + 2);
			tank[ntanks].beginpressure = array_uint16_le (p + tankoffset + ntanks * 4 + 0);
			tank[ntanks].endpressure   = array_uint16_le (p + tankoffset + ntanks * 4 + 2);
			if (tank[ntanks].beginpressure == 0 && (tank[ntanks].endpressure == 0 || tank[ntanks].endpressure == 36000))
				break;
			ntanks++;
		}
	}

	// Limit the size to the actual length.
	parser->base.size = length;

	// Cache the data for later use.
	parser->logformat = 0;
	parser->mode = mode;
	parser->nsamples = nsamples;
	parser->samplesize = samplesize;
	parser->headersize = headersize;
	parser->settings = settings;
	parser->surftime = 3 * 60;
	parser->interval = interval;
	parser->samplerate = samplerate;
	parser->ntanks = ntanks;
	parser->ngasmixes = ngasmixes;
	for (unsigned int i = 0; i < ngasmixes; ++i) {
		parser->gasmix[i] = gasmix[i];
	}
	for (unsigned int i = 0; i < ntanks; ++i) {
		parser->tank[i] = tank[i];
	}
	parser->layout = layout;
	parser->cached = 1;

	return DC_STATUS_SUCCESS;
}

static dc_status_t
mares_genius_cache (mares_iconhd_parser_t *parser)
{
	dc_parser_t *abstract = (dc_parser_t *) parser;
	const unsigned char *data = parser->base.data;
	unsigned int size = parser->base.size;

	if (size < 20) {
		ERROR (abstract->context, "Buffer overflow detected!");
		return DC_STATUS_DATAFORMAT;
	}

	// Check the header type and version.
	unsigned int type = array_uint16_le (data);
	unsigned int minor = data[2];
	unsigned int major = data[3];
	if (type != 1 || OBJVERSION(major,minor) > OBJVERSION(1,1)) {
		ERROR (abstract->context, "Unsupported object type (%u) or version (%u.%u).",
			type, major, minor);
		return DC_STATUS_DATAFORMAT;
	}

	// Get the data format.
	unsigned int logformat = data[0x10];

	// The Horizon header has 8 bytes extra at offset 0x18.
	unsigned int extra = 0;
	const mares_iconhd_layout_t * layout = &genius;
	if (logformat == 1) {
		extra = 8;
		layout = &horizon;
	}

	// The Genius header (v1.x) has 10 bytes more at the end.
	unsigned int more = 0;
	if (major == 1) {
		more = 16;
	}

	// Get the header size.
	unsigned int headersize = 0xB8 + extra + more;
	if (headersize > size) {
		ERROR (abstract->context, "Buffer overflow detected!");
		return DC_STATUS_DATAFORMAT;
	}

	// Get the number of samples in the profile data.
	unsigned int nsamples = array_uint16_le (data + 0x20 + extra);

	// Get the dive settings.
	unsigned int settings = array_uint32_le (data + layout->settings);

	// Get the dive mode.
	unsigned int mode = settings & 0xF;

	// Get the sample size.
	unsigned int samplesize = logformat == 1 ? SDPT_SIZE: DPRS_SIZE;

	// Calculate the total number of bytes for this dive.
	unsigned int nbytes = headersize + 4 + DSTR_SIZE + TISS_SIZE + nsamples * samplesize + (nsamples / 4) * AIRS_SIZE + DEND_SIZE;
	if (nbytes > size) {
		ERROR (abstract->context, "Buffer overflow detected!");
		return DC_STATUS_DATAFORMAT;
	}

	// Get the profile type and version.
	unsigned int profile_type = array_uint16_le (data + headersize);
	unsigned int profile_minor = data[headersize + 2];
	unsigned int profile_major = data[headersize + 3];

	// Get the surface timeout setting (in minutes).
	// For older firmware versions the value is hardcoded to 3 minutes, but
	// starting with the newer v01.02.00 firmware the value is configurable and
	// stored in the settings. To detect whether the setting is available, we
	// need to check the profile version instead of the header version.
	unsigned int surftime = 3;
	if (profile_type == 0 &&
		OBJVERSION(profile_major,profile_minor) >= OBJVERSION(1,0)) {
		surftime = (settings >> 13) & 0x3F;
	}

	// Gas mixes and tanks.
	unsigned int ntanks = 0;
	unsigned int ngasmixes = 0;
	mares_iconhd_gasmix_t gasmix[NGASMIXES_GENIUS] = {0};
	mares_iconhd_tank_t tank[NTANKS_GENIUS] = {0};
	for (unsigned int i = 0; i < NGASMIXES_GENIUS; i++) {
		unsigned int offset = layout->tanks + i * 20;
		unsigned int gasmixparams  = array_uint32_le(data + offset + 0);
		unsigned int beginpressure = array_uint16_le(data + offset + 4);
		unsigned int endpressure   = array_uint16_le(data + offset + 6);
		unsigned int volume        = array_uint16_le(data + offset + 8);
		unsigned int workpressure  = array_uint16_le(data + offset + 10);

		unsigned int o2      = (gasmixparams      ) & 0x7F;
		unsigned int n2      = (gasmixparams >>  7) & 0x7F;
		unsigned int he      = (gasmixparams >> 14) & 0x7F;
		unsigned int state   = (gasmixparams >> 21) & 0x03;
		unsigned int changed = (gasmixparams >> 23) & 0x01;

		if (o2 + n2 + he != 100) {
			WARNING (abstract->context, "Invalid gas mix (%u%% He, %u%% O2, %u%% N2).", he, o2, n2);
		}

		// The active gas mixes are always first, so we stop processing
		// as soon as the first gas mix marked as disabled is found.
		if (state != GASMIX_OFF && ngasmixes == i) {
			gasmix[i].oxygen = o2;
			gasmix[i].helium = he;
			ngasmixes++;
		}

		// Assume the active transmitters are always first, so we can
		// stop processing as soon as the first inactive transmitter is
		// found.
		if ((beginpressure != 0 || (endpressure != 0 && endpressure != 36000)) &&
			(ntanks == i)) {
			tank[i].volume = volume;
			tank[i].workpressure = workpressure;
			tank[i].beginpressure = beginpressure;
			tank[i].endpressure = endpressure;
			ntanks++;
		}
	}

	// Cache the data for later use.
	parser->logformat = logformat;
	parser->mode = mode;
	parser->nsamples = nsamples;
	parser->samplesize = samplesize;
	parser->headersize = headersize;
	parser->settings = settings;
	parser->surftime = surftime * 60;
	parser->interval = 5;
	parser->samplerate = 1;
	parser->ntanks = ntanks;
	parser->ngasmixes = ngasmixes;
	for (unsigned int i = 0; i < ngasmixes; ++i) {
		parser->gasmix[i] = gasmix[i];
	}
	for (unsigned int i = 0; i < ntanks; ++i) {
		parser->tank[i] = tank[i];
	}
	parser->layout = layout;
	parser->cached = 1;

	return DC_STATUS_SUCCESS;
}

static dc_status_t
mares_iconhd_parser_cache (mares_iconhd_parser_t *parser)
{
	if (parser->cached) {
		return DC_STATUS_SUCCESS;
	}

	if (parser->model == GENIUS || parser->model == HORIZON) {
		return mares_genius_cache (parser);
	} else {
		return mares_iconhd_cache (parser);
	}
}

dc_status_t
mares_iconhd_parser_create (dc_parser_t **out, dc_context_t *context, unsigned int model, unsigned int serial)
{
	mares_iconhd_parser_t *parser = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	parser = (mares_iconhd_parser_t *) dc_parser_allocate (context, &mares_iconhd_parser_vtable);
	if (parser == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Set the default values.
	parser->model = model;
	parser->cached = 0;
	parser->logformat = 0;
	parser->mode = (model == GENIUS || model == HORIZON) ? GENIUS_AIR : ICONHD_AIR;
	parser->nsamples = 0;
	parser->samplesize = 0;
	parser->headersize = 0;
	parser->settings = 0;
	parser->surftime = 0;
	parser->interval = 0;
	parser->samplerate = 0;
	parser->ntanks = 0;
	parser->ngasmixes = 0;
	parser->serial = serial;
	for (unsigned int i = 0; i < NGASMIXES; ++i) {
		parser->gasmix[i].oxygen = 0;
		parser->gasmix[i].helium = 0;
	}
	for (unsigned int i = 0; i < NTANKS; ++i) {
		parser->tank[i].volume = 0;
		parser->tank[i].workpressure = 0;
		parser->tank[i].beginpressure = 0;
		parser->tank[i].endpressure = 0;
	}
	parser->layout = NULL;

	*out = (dc_parser_t*) parser;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
mares_iconhd_parser_set_data (dc_parser_t *abstract, const unsigned char *data, unsigned int size)
{
	mares_iconhd_parser_t *parser = (mares_iconhd_parser_t *) abstract;

	// Reset the cache.
	parser->cached = 0;
	parser->logformat = 0;
	parser->mode = (parser->model == GENIUS || parser->model == HORIZON) ? GENIUS_AIR : ICONHD_AIR;
	parser->nsamples = 0;
	parser->samplesize = 0;
	parser->headersize = 0;
	parser->settings = 0;
	parser->surftime = 0;
	parser->interval = 0;
	parser->samplerate = 0;
	parser->ntanks = 0;
	parser->ngasmixes = 0;
	for (unsigned int i = 0; i < NGASMIXES; ++i) {
		parser->gasmix[i].oxygen = 0;
		parser->gasmix[i].helium = 0;
	}
	for (unsigned int i = 0; i < NTANKS; ++i) {
		parser->tank[i].volume = 0;
		parser->tank[i].workpressure = 0;
		parser->tank[i].beginpressure = 0;
		parser->tank[i].endpressure = 0;
	}
	parser->layout = NULL;

	return DC_STATUS_SUCCESS;
}


static dc_status_t
mares_iconhd_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime)
{
	mares_iconhd_parser_t *parser = (mares_iconhd_parser_t *) abstract;

	// Cache the parser data.
	dc_status_t rc = mares_iconhd_parser_cache (parser);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Pointer to the header data.
	const unsigned char *p = abstract->data;
	if (parser->model != GENIUS && parser->model != HORIZON) {
		p += abstract->size - parser->headersize;
		if (parser->model != SMART && parser->model != SMARTAPNEA && parser->model != SMARTAIR) {
			p += 4;
		}
	}

	// Offset to the date/time field.
	p += parser->layout->datetime;

	if (datetime) {
		if (parser->model == GENIUS || parser->model == HORIZON) {
			unsigned int timestamp = array_uint32_le (p);
			datetime->hour   = (timestamp     ) & 0x1F;
			datetime->minute = (timestamp >> 5) & 0x3F;
			datetime->second = 0;
			datetime->day    = (timestamp >> 11) & 0x1F;
			datetime->month  = (timestamp >> 16) & 0x0F;
			datetime->year   = (timestamp >> 20) & 0x0FFF;
		} else {
			datetime->hour   = array_uint16_le (p + 0);
			datetime->minute = array_uint16_le (p + 2);
			datetime->second = 0;
			datetime->day    = array_uint16_le (p + 4);
			datetime->month  = array_uint16_le (p + 6) + 1;
			datetime->year   = array_uint16_le (p + 8) + 1900;
		}
		datetime->timezone = DC_TIMEZONE_NONE;
	}

	return DC_STATUS_SUCCESS;
}

#define BUFLEN 16

static dc_status_t
mares_iconhd_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value)
{
	mares_iconhd_parser_t *parser = (mares_iconhd_parser_t *) abstract;

	// Cache the parser data.
	dc_status_t rc = mares_iconhd_parser_cache (parser);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Pointer to the header data.
	const unsigned char *p = abstract->data;
	if (parser->model != GENIUS && parser->model != HORIZON) {
		p += abstract->size - parser->headersize;
		if (parser->model != SMART && parser->model != SMARTAPNEA && parser->model != SMARTAIR) {
			p += 4;
		}
	}

	// The Horizon header has 8 bytes extra at offset 0x18.
	unsigned int extra = 0;
	if (parser->logformat == 1) {
		extra = 8;
	}

	unsigned int metric = (parser->model == GENIUS || parser->model == HORIZON) ?
		p[0x34 + extra] : parser->settings & 0x0100;

	dc_gasmix_t *gasmix = (dc_gasmix_t *) value;
	dc_tank_t *tank = (dc_tank_t *) value;
	dc_salinity_t *water = (dc_salinity_t *) value;
	dc_field_string_t *string = (dc_field_string_t *) value;
	char buf[BUFLEN];

	if (value) {
		switch (type) {
		case DC_FIELD_DIVETIME:
			if (parser->layout->divetime != UNSUPPORTED) {
				*((unsigned int *) value) = array_uint16_le (p + parser->layout->divetime);
			} else {
				*((unsigned int *) value) = parser->nsamples * parser->interval - parser->surftime;
			}
			break;
		case DC_FIELD_MAXDEPTH:
			*((double *) value) = array_uint16_le (p + parser->layout->maxdepth) / 10.0;
			break;
		case DC_FIELD_GASMIX_COUNT:
			*((unsigned int *) value) = parser->ngasmixes;
			break;
		case DC_FIELD_GASMIX:
			gasmix->oxygen = parser->gasmix[flags].oxygen / 100.0;
			gasmix->helium = parser->gasmix[flags].helium / 100.0;
			gasmix->nitrogen = 1.0 - gasmix->oxygen - gasmix->helium;
			break;
		case DC_FIELD_TANK_COUNT:
			*((unsigned int *) value) = parser->ntanks;
			break;
		case DC_FIELD_TANK:
			if (metric) {
				tank->type = DC_TANKVOLUME_METRIC;
				tank->volume = parser->tank[flags].volume;
				tank->workpressure = parser->tank[flags].workpressure;
			} else {
				if (parser->tank[flags].workpressure == 0)
					return DC_STATUS_DATAFORMAT;
				tank->type = DC_TANKVOLUME_IMPERIAL;
				tank->volume = parser->tank[flags].volume * CUFT * 1000.0;
				tank->volume /= parser->tank[flags].workpressure * PSI / ATM;
				tank->workpressure = parser->tank[flags].workpressure * PSI / BAR;
			}
			tank->beginpressure = parser->tank[flags].beginpressure / 100.0;
			tank->endpressure   = parser->tank[flags].endpressure / 100.0;
			if (flags < parser->ngasmixes) {
				tank->gasmix = flags;
			} else {
				tank->gasmix = DC_GASMIX_UNKNOWN;
			}
			break;
		case DC_FIELD_ATMOSPHERIC:
			*((double *) value) = array_uint16_le (p + parser->layout->atmospheric) / (1000.0 * parser->layout->atmospheric_divisor);
			break;
		case DC_FIELD_SALINITY:
			if (parser->model == GENIUS || parser->model == HORIZON) {
				unsigned int salinity = (parser->settings >> 5) & 0x03;
				switch (salinity) {
				case WATER_FRESH:
					water->type = DC_WATER_FRESH;
					water->density = 0.0;
					break;
				case WATER_SALT:
					water->type = DC_WATER_SALT;
					water->density = 0.0;
					break;
				case WATER_EN13319:
					water->type = DC_WATER_SALT;
					water->density = MSW / GRAVITY;
					break;
				default:
					return DC_STATUS_DATAFORMAT;
				}
			} else if (parser->model == SMARTAPNEA) {
				unsigned int salinity = parser->settings & 0x003F;
				if (salinity == 0) {
					water->type = DC_WATER_FRESH;
				} else {
					water->type = DC_WATER_SALT;
				}
				water->density = 1000.0 + salinity;
			} else {
				if (parser->settings & 0x0010) {
					water->type = DC_WATER_FRESH;
				} else {
					water->type = DC_WATER_SALT;
				}
				water->density = 0.0;
			}
			break;
		case DC_FIELD_TEMPERATURE_MINIMUM:
			*((double *) value) = (signed short) array_uint16_le (p + parser->layout->temperature_min) / 10.0;
			break;
		case DC_FIELD_TEMPERATURE_MAXIMUM:
			*((double *) value) = (signed short) array_uint16_le (p + parser->layout->temperature_max) / 10.0;
			break;
		case DC_FIELD_DIVEMODE:
			if (parser->model == GENIUS || parser->model == HORIZON) {
				switch (parser->mode) {
				case GENIUS_AIR:
				case GENIUS_NITROX_SINGLE:
				case GENIUS_NITROX_MULTI:
				case GENIUS_TRIMIX:
				case GENIUS_OC:
					*((dc_divemode_t *) value) = DC_DIVEMODE_OC;
					break;
				case GENIUS_GAUGE:
					*((dc_divemode_t *) value) = DC_DIVEMODE_GAUGE;
					break;
				case GENIUS_FREEDIVE:
					*((dc_divemode_t *) value) = DC_DIVEMODE_FREEDIVE;
					break;
				case GENIUS_SCR:
					*((dc_divemode_t *) value) = DC_DIVEMODE_SCR;
					break;
				default:
					return DC_STATUS_DATAFORMAT;
				}
			} else {
				switch (parser->mode) {
				case ICONHD_AIR:
				case ICONHD_NITROX:
					*((dc_divemode_t *) value) = DC_DIVEMODE_OC;
					break;
				case ICONHD_GAUGE:
					*((dc_divemode_t *) value) = DC_DIVEMODE_GAUGE;
					break;
				case ICONHD_FREEDIVE:
					*((dc_divemode_t *) value) = DC_DIVEMODE_FREEDIVE;
					break;
				default:
					return DC_STATUS_DATAFORMAT;
				}
			}
			break;
		case DC_FIELD_STRING:
			switch(flags) {
			case 0: /* serial */
				string->desc = "Serial";
				snprintf(buf, BUFLEN, "%u", parser->serial);
				break;
			default:
				return DC_STATUS_UNSUPPORTED;
			}
			string->value = strdup(buf);
			break;
		default:
			return DC_STATUS_UNSUPPORTED;
		}
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
mares_iconhd_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
	mares_iconhd_parser_t *parser = (mares_iconhd_parser_t *) abstract;

	// Cache the parser data.
	dc_status_t rc = mares_iconhd_parser_cache (parser);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	const unsigned char *data = abstract->data;

	if (parser->samplerate > 1) {
		// The Smart Apnea supports multiple samples per second
		// (e.g. 2, 4 or 8). Since our smallest unit of time is one
		// second, we can't represent this, and the extra samples
		// will get dropped.
		WARNING(abstract->context, "Multiple samples per second are not supported!");
	}

	// Previous gas mix - initialize with impossible value
	unsigned int gasmix_previous = 0xFFFFFFFF;

	unsigned int offset = 4;
	unsigned int marker = 0;
	if (parser->model == GENIUS || parser->model == HORIZON) {
		// Skip the dive header.
		data += parser->headersize;

		// Check the profile type and version.
		unsigned int type = array_uint16_le (data);
		unsigned int minor = data[2];
		unsigned int major = data[3];
		if (type > 1 ||
			(type == 0 && OBJVERSION(major,minor) > OBJVERSION(1,0)) ||
			(type == 1 && OBJVERSION(major,minor) > OBJVERSION(0,2))) {
			ERROR (abstract->context, "Unsupported object type (%u) or version (%u.%u).",
				type, major, minor);
			return DC_STATUS_DATAFORMAT;
		}

		// Skip the DSTR record.
		if (!mares_genius_isvalid (data + offset, DSTR_SIZE, DSTR_TYPE)) {
			ERROR (abstract->context, "Invalid DSTR record.");
			return DC_STATUS_DATAFORMAT;
		}
		offset += DSTR_SIZE;

		// Skip the TISS record.
		if (!mares_genius_isvalid (data + offset, TISS_SIZE, TISS_TYPE)) {
			ERROR (abstract->context, "Invalid TISS record.");
			return DC_STATUS_DATAFORMAT;
		}
		offset += TISS_SIZE;

		// Size of the record type marker.
		marker = 4;
	}

	unsigned int time = 0;
	unsigned int nsamples = 0;
	while (nsamples < parser->nsamples) {
		dc_sample_value_t sample = {0};

		if (parser->model == SMARTAPNEA) {
			unsigned int maxdepth = array_uint16_le (data + offset + 0);
			unsigned int divetime = array_uint16_le (data + offset + 2);
			unsigned int surftime = array_uint16_le (data + offset + 4);

			// Surface Time (seconds).
			time += surftime;
			sample.time = time;
			if (callback) callback (DC_SAMPLE_TIME, sample, userdata);

			// Surface Depth (0 m).
			sample.depth = 0.0;
			if (callback) callback (DC_SAMPLE_DEPTH, sample, userdata);

			offset += parser->samplesize;
			nsamples++;

			for (unsigned int i = 0; i < divetime; ++i) {
				// Time (seconds).
				time += parser->interval;
				sample.time = time;
				if (callback) callback (DC_SAMPLE_TIME, sample, userdata);

				// Depth (1/10 m).
				unsigned int depth = array_uint16_le (data + offset);
				sample.depth = depth / 10.0;
				if (callback) callback (DC_SAMPLE_DEPTH, sample, userdata);

				offset += 2 * parser->samplerate;
			}
		} else if (parser->model != GENIUS && parser->model != HORIZON && parser->mode == ICONHD_FREEDIVE) {
			unsigned int maxdepth = array_uint16_le (data + offset + 0);
			unsigned int divetime = array_uint16_le (data + offset + 2);
			unsigned int surftime = array_uint16_le (data + offset + 4);

			// Surface Time (seconds).
			time += surftime;
			sample.time = time;
			if (callback) callback (DC_SAMPLE_TIME, sample, userdata);

			// Surface Depth (0 m).
			sample.depth = 0.0;
			if (callback) callback (DC_SAMPLE_DEPTH, sample, userdata);

			// Dive Time (seconds).
			time += divetime;
			sample.time = time;
			if (callback) callback (DC_SAMPLE_TIME, sample, userdata);

			// Maximum Depth (1/10 m).
			sample.depth = maxdepth / 10.0;
			if (callback) callback (DC_SAMPLE_DEPTH, sample, userdata);

			offset += parser->samplesize;
			nsamples++;
		} else {
			unsigned int depth = 0, temperature = 0;
			unsigned int gasmix = 0, alarms = 0;
			unsigned int decostop = 0, decodepth = 0, decotime = 0, tts = 0;
			if (parser->model == GENIUS || parser->model == HORIZON) {
				if (parser->logformat == 1) {
					if (!mares_genius_isvalid (data + offset, SDPT_SIZE, SDPT_TYPE)) {
						ERROR (abstract->context, "Invalid SDPT record.");
						return DC_STATUS_DATAFORMAT;
					}

					unsigned int misc = 0, deco = 0;
					depth       = array_uint16_le (data + offset + marker + 2);
					temperature = array_uint16_le (data + offset + marker + 6);
					alarms      = array_uint32_le (data + offset + marker + 0x14);
					misc        = array_uint32_le (data + offset + marker + 0x18);
					deco        = array_uint32_le (data + offset + marker + 0x1C);
					gasmix      = (misc >>  6) & 0x0F;
					decostop    = (misc >> 10) & 0x01;
					if (decostop) {
						decodepth = (deco >>  3) & 0x7F;
						decotime  = (deco >> 10) & 0xFF;
						tts       = (deco >> 18) & 0x3FFF;
					} else {
						decotime  = deco & 0xFF;
					}
				} else {
					if (!mares_genius_isvalid (data + offset, DPRS_SIZE, DPRS_TYPE)) {
						ERROR (abstract->context, "Invalid DPRS record.");
						return DC_STATUS_DATAFORMAT;
					}

					unsigned int misc = 0;
					depth       = array_uint16_le (data + offset + marker + 0);
					temperature = array_uint16_le (data + offset + marker + 4);
					decotime    = array_uint16_le (data + offset + marker + 0x0A);
					alarms      = array_uint32_le (data + offset + marker + 0x0C);
					misc        = array_uint32_le (data + offset + marker + 0x14);
					gasmix      = (misc >>  6) & 0x0F;
					decostop    = (misc >> 18) & 0x01;
					decodepth   = (misc >> 19) & 0x7F;
				}
			} else {
				depth = array_uint16_le (data + offset + 0);
				temperature = array_uint16_le (data + offset + 2) & 0x0FFF;
				gasmix = (data[offset + 3] & 0xF0) >> 4;
			}

			// Time (seconds).
			time += parser->interval;
			sample.time = time;
			if (callback) callback (DC_SAMPLE_TIME, sample, userdata);

			// Depth (1/10 m).
			sample.depth = depth / 10.0;
			if (callback) callback (DC_SAMPLE_DEPTH, sample, userdata);

			// Temperature (1/10 °C).
			sample.temperature = temperature / 10.0;
			if (callback) callback (DC_SAMPLE_TEMPERATURE, sample, userdata);

			// Current gas mix
			if (parser->ngasmixes > 0) {
				if (gasmix >= parser->ngasmixes) {
					ERROR (abstract->context, "Invalid gas mix index.");
					return DC_STATUS_DATAFORMAT;
				}
				if (gasmix != gasmix_previous) {
					sample.gasmix = gasmix;
					if (callback) callback (DC_SAMPLE_GASMIX, sample, userdata);
					gasmix_previous = gasmix;
				}
			}

			if (parser->model == GENIUS || parser->model == HORIZON) {
				// Deco stop / NDL.
				if (decostop) {
					sample.deco.type = DC_DECO_DECOSTOP;
					sample.deco.depth = decodepth;
				} else {
					sample.deco.type = DC_DECO_NDL;
					sample.deco.depth = 0.0;
				}
				sample.deco.time = decotime * 60;
				if (callback) callback (DC_SAMPLE_DECO, sample, userdata);

				// Alarms
				for (unsigned int v = alarms, i = 0; v; v >>= 1, ++i) {
					if ((v & 1) == 0) {
						continue;
					}

					switch (i) {
					case ALARM_FAST_ASCENT:
					case ALARM_UNCONTROLLED_ASCENT:
						sample.event.type = SAMPLE_EVENT_ASCENT;
						break;
					case ALARM_MISSED_DECO:
					case ALARM_DIVE_VIOLATION_DECO:
						sample.event.type = SAMPLE_EVENT_CEILING;
						break;
					default:
						sample.event.type = SAMPLE_EVENT_NONE;
						break;
					}

					if (sample.event.type != SAMPLE_EVENT_NONE) {
						sample.event.time = 0;
						sample.event.flags = 0;
						sample.event.value = 0;
						if (callback) callback (DC_SAMPLE_EVENT, sample, userdata);
					}
				}
			}

			offset += parser->samplesize;
			nsamples++;

			// Some extra data.
			if (parser->layout->tanks != UNSUPPORTED && (nsamples % 4) == 0) {
				if ((parser->model == GENIUS || parser->model == HORIZON) &&
					!mares_genius_isvalid (data + offset, AIRS_SIZE, AIRS_TYPE)) {
					ERROR (abstract->context, "Invalid AIRS record.");
					return DC_STATUS_DATAFORMAT;
				}

				// Pressure (1/100 bar).
				unsigned int pressure = array_uint16_le(data + offset + marker + 0);
				if (gasmix < parser->ntanks) {
					sample.pressure.tank = gasmix;
					sample.pressure.value = pressure / 100.0;
					if (callback) callback (DC_SAMPLE_PRESSURE, sample, userdata);
				} else if (pressure != 0) {
					WARNING (abstract->context, "Invalid tank with non-zero pressure.");
				}

				offset += (parser->model == GENIUS || parser->model == HORIZON) ? AIRS_SIZE : 8;
			}
		}
	}

	if (parser->model == GENIUS || parser->model == HORIZON) {
		// Skip the DEND record.
		if (!mares_genius_isvalid (data + offset, DEND_SIZE, DEND_TYPE)) {
			ERROR (abstract->context, "Invalid DEND record.");
			return DC_STATUS_DATAFORMAT;
		}
		offset += DEND_SIZE;
	}

	return DC_STATUS_SUCCESS;
}
