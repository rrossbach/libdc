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

#include <string.h> // memcpy, memcmp
#include <stdlib.h> // malloc, free
#include <assert.h> // assert

#include "mares_iconhd.h"
#include "context-private.h"
#include "device-private.h"
#include "array.h"
#include "rbstream.h"
#include "platform.h"

#define C_ARRAY_SIZE(array) (sizeof (array) / sizeof *(array))

#define ISINSTANCE(device) dc_device_isinstance((device), &mares_iconhd_device_vtable)

#define NSTEPS    1000
#define STEP(i,n) (NSTEPS * (i) / (n))

#define MATRIX    0x0F
#define SMART      0x000010
#define SMARTAPNEA 0x010010
#define ICONHD    0x14
#define ICONHDNET 0x15
#define PUCKPRO   0x18
#define NEMOWIDE2 0x19
#define GENIUS    0x1C
#define PUCK2     0x1F
#define QUADAIR   0x23
#define SMARTAIR  0x24
#define QUAD      0x29
#define HORIZON   0x2C

#define MAXRETRIES 4

#define ACK 0xAA
#define EOF 0xEA
#define XOR 0xA5

#define CMD_VERSION   0xC2
#define CMD_FLASHSIZE 0xB3
#define CMD_READ      0xE7
#define CMD_OBJ_INIT  0xBF
#define CMD_OBJ_EVEN  0xAC
#define CMD_OBJ_ODD   0xFE

#define OBJ_LOGBOOK       0x2008
#define OBJ_LOGBOOK_COUNT 0x01
#define OBJ_DIVE          0x3000
#define OBJ_DIVE_HEADER   0x02
#define OBJ_DIVE_DATA     0x03

#define AIR       0
#define GAUGE     1
#define NITROX    2
#define FREEDIVE  3

typedef struct mares_iconhd_layout_t {
	unsigned int memsize;
	unsigned int rb_profile_begin;
	unsigned int rb_profile_end;
} mares_iconhd_layout_t;

typedef struct mares_iconhd_model_t {
	unsigned char name[16 + 1];
	unsigned int id;
} mares_iconhd_model_t;

typedef struct mares_iconhd_device_t {
	dc_device_t base;
	dc_iostream_t *iostream;
	const mares_iconhd_layout_t *layout;
	unsigned char fingerprint[10];
	unsigned int fingerprint_size;
	unsigned char version[140];
	unsigned int model;
	unsigned int packetsize;
	unsigned char cache[20];
	unsigned int available;
	unsigned int offset;
	unsigned int splitcommand;
} mares_iconhd_device_t;

static dc_status_t mares_iconhd_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size);
static dc_status_t mares_iconhd_device_read (dc_device_t *abstract, unsigned int address, unsigned char data[], unsigned int size);
static dc_status_t mares_iconhd_device_dump (dc_device_t *abstract, dc_buffer_t *buffer);
static dc_status_t mares_iconhd_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);

static const dc_device_vtable_t mares_iconhd_device_vtable = {
	sizeof(mares_iconhd_device_t),
	DC_FAMILY_MARES_ICONHD,
	mares_iconhd_device_set_fingerprint, /* set_fingerprint */
	mares_iconhd_device_read, /* read */
	NULL, /* write */
	mares_iconhd_device_dump, /* dump */
	mares_iconhd_device_foreach, /* foreach */
	NULL, /* timesync */
	NULL /* close */
};

static const mares_iconhd_layout_t mares_iconhd_layout = {
	0x100000, /* memsize */
	0x00A000, /* rb_profile_begin */
	0x100000, /* rb_profile_end */
};

static const mares_iconhd_layout_t mares_iconhdnet_layout = {
	0x100000, /* memsize */
	0x00E000, /* rb_profile_begin */
	0x100000, /* rb_profile_end */
};

static const mares_iconhd_layout_t mares_genius_layout = {
	0x1000000, /* memsize */
	0x0100000, /* rb_profile_begin */
	0x1000000, /* rb_profile_end */
};

static const mares_iconhd_layout_t mares_matrix_layout = {
	0x40000, /* memsize */
	0x0A000, /* rb_profile_begin */
	0x3E000, /* rb_profile_end */
};

static const mares_iconhd_layout_t mares_nemowide2_layout = {
	0x40000, /* memsize */
	0x0A000, /* rb_profile_begin */
	0x40000, /* rb_profile_end */
};

static unsigned int
mares_iconhd_get_model (mares_iconhd_device_t *device)
{
	const mares_iconhd_model_t models[] = {
		{"Matrix",      MATRIX},
		{"Smart",       SMART},
		{"Smart Apnea", SMARTAPNEA},
		{"Icon HD",     ICONHD},
		{"Icon AIR",    ICONHDNET},
		{"Puck Pro",    PUCKPRO},
		{"Nemo Wide 2", NEMOWIDE2},
		{"Genius",      GENIUS},
		{"Puck 2",      PUCK2},
		{"Quad Air",    QUADAIR},
		{"Smart Air",   SMARTAIR},
		{"Quad",        QUAD},
		{"Horizon",     HORIZON},
	};

	// Check the product name in the version packet against the list
	// with valid names, and return the corresponding model number.
	unsigned int model = 0;
	for (unsigned int i = 0; i < C_ARRAY_SIZE(models); ++i) {
		if (memcmp (device->version + 0x46, models[i].name, sizeof (models[i].name) - 1) == 0) {
			model = models[i].id;
			break;
		}
	}

	return model;
}

static dc_status_t
mares_iconhd_read (mares_iconhd_device_t *device, unsigned char data[], size_t size)
{
	dc_status_t rc = DC_STATUS_SUCCESS;
	dc_transport_t transport = dc_iostream_get_transport(device->iostream);

	size_t nbytes = 0;
	while (nbytes < size) {
		if (transport == DC_TRANSPORT_BLE) {
			if (device->available == 0) {
				// Read a packet into the cache.
				size_t len = 0;
				rc = dc_iostream_read (device->iostream, device->cache, sizeof(device->cache), &len);
				if (rc != DC_STATUS_SUCCESS)
					return rc;

				device->available = len;
				device->offset = 0;
			}
		}

		// Set the minimum packet size.
		size_t length = (transport == DC_TRANSPORT_BLE) ? device->available : size - nbytes;

		// Limit the packet size to the total size.
		if (nbytes + length > size)
			length = size - nbytes;

		if (transport == DC_TRANSPORT_BLE) {
			// Copy the data from the cached packet.
			memcpy (data + nbytes, device->cache + device->offset, length);
			device->available -= length;
			device->offset += length;
		} else {
			// Read the packet.
			rc = dc_iostream_read (device->iostream, data + nbytes, length, &length);
			if (rc != DC_STATUS_SUCCESS)
				return rc;
		}

		nbytes += length;
	}

	return rc;
}

static dc_status_t
mares_iconhd_write (mares_iconhd_device_t *device, const unsigned char data[], size_t size)
{
	dc_status_t rc = DC_STATUS_SUCCESS;
	dc_transport_t transport = dc_iostream_get_transport(device->iostream);

	size_t nbytes = 0;
	while (nbytes < size) {
		// Set the maximum packet size.
		size_t length = (transport == DC_TRANSPORT_BLE) ? sizeof(device->cache) : size - nbytes;

		// Limit the packet size to the total size.
		if (nbytes + length > size)
			length = size - nbytes;

		// Write the packet.
		rc = dc_iostream_write (device->iostream, data + nbytes, length, &length);
		if (rc != DC_STATUS_SUCCESS)
			return rc;

		nbytes += length;
	}

	return rc;
}

static dc_status_t
mares_iconhd_packet (mares_iconhd_device_t *device,
	const unsigned char command[], unsigned int csize,
	unsigned char answer[], unsigned int asize)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;
	unsigned int split_csize;

	assert (csize >= 2);

	if (device_is_cancelled (abstract))
		return DC_STATUS_CANCELLED;

	split_csize = device->splitcommand ? 2 : csize;

	// Send the command header to the dive computer.
	status = mares_iconhd_write (device, command, split_csize);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to send the command.");
		return status;
	}

	// Receive the header byte.
	unsigned char header[1] = {0};
	status = mares_iconhd_read (device, header, sizeof (header));
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the answer.");
		return status;
	}

	// Verify the header byte.
	if (header[0] != ACK) {
		ERROR (abstract->context, "Unexpected answer byte.");
		return DC_STATUS_PROTOCOL;
	}

	// Send any remaining command payload to the dive computer.
	if (csize > split_csize) {
		status = mares_iconhd_write (device, command + split_csize, csize - split_csize);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to send the command.");
			return status;
		}
	}

	// Read the packet.
	status = mares_iconhd_read (device, answer, asize);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the answer.");
		return status;
	}

	// Receive the trailer byte.
	unsigned char trailer[1] = {0};
	status = mares_iconhd_read (device, trailer, sizeof (trailer));
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to receive the answer.");
		return status;
	}

	// Verify the trailer byte.
	if (trailer[0] != EOF) {
		ERROR (abstract->context, "Unexpected answer byte.");
		return DC_STATUS_PROTOCOL;
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
mares_iconhd_transfer (mares_iconhd_device_t *device, const unsigned char command[], unsigned int csize, unsigned char answer[], unsigned int asize)
{
	unsigned int nretries = 0;
	dc_status_t rc = DC_STATUS_SUCCESS;
	while ((rc = mares_iconhd_packet (device, command, csize, answer, asize)) != DC_STATUS_SUCCESS) {
		// Automatically discard a corrupted packet,
		// and request a new one.
		if (rc != DC_STATUS_PROTOCOL && rc != DC_STATUS_TIMEOUT)
			return rc;

		// Abort if the maximum number of retries is reached.
		if (nretries++ >= MAXRETRIES)
			return rc;

		// Discard any garbage bytes.
		dc_iostream_sleep (device->iostream, 100);
		dc_iostream_purge (device->iostream, DC_DIRECTION_INPUT);
		device->available = 0;
		device->offset = 0;
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
mares_iconhd_read_object(mares_iconhd_device_t *device, dc_event_progress_t *progress, dc_buffer_t *buffer, unsigned int index, unsigned int subindex)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	dc_device_t *abstract = (dc_device_t *) device;
	dc_transport_t transport = dc_iostream_get_transport (device->iostream);
	const unsigned int maxpacket = (transport == DC_TRANSPORT_BLE) ? 124 : 504;

	// Update and emit a progress event.
	unsigned int initial = 0;
	if (progress) {
		initial = progress->current;
		device_event_emit (abstract, DC_EVENT_PROGRESS, progress);
	}

	// Transfer the init packet.
	unsigned char rsp_init[16];
	unsigned char cmd_init[18] = {
		CMD_OBJ_INIT,
		CMD_OBJ_INIT ^ XOR,
		0x40,
		(index >> 0) & 0xFF,
		(index >> 8) & 0xFF,
		subindex & 0xFF
	};
	memset (cmd_init + 6, 0x00, sizeof(cmd_init) - 6);
	status = mares_iconhd_transfer (device, cmd_init, sizeof (cmd_init), rsp_init, sizeof (rsp_init));
	if (status != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to transfer the init packet.");
		return status;
	}

	// Verify the packet header.
	if (memcmp (cmd_init + 3, rsp_init + 1, 3) != 0) {
		ERROR (abstract->context, "Unexpected packet header.");
		return DC_STATUS_PROTOCOL;
	}

	unsigned int nbytes = 0, size = 0;
	if (rsp_init[0] == 0x41) {
		// A large (and variable size) payload is split into multiple
		// data packets. The first packet contains only the total size
		// of the payload.
		size = array_uint32_le (rsp_init + 4);
	} else if (rsp_init[0] == 0x42) {
		// A short (and fixed size) payload is embedded into the first
		// data packet.
		size = sizeof(rsp_init) - 4;

		// Append the payload to the output buffer.
		if (!dc_buffer_append (buffer, rsp_init + 4, sizeof(rsp_init) - 4)) {
			ERROR (abstract->context, "Insufficient buffer space available.");
			return DC_STATUS_NOMEMORY;
		}

		nbytes += sizeof(rsp_init) - 4;
	} else {
		ERROR (abstract->context, "Unexpected packet type (%02x).", rsp_init[0]);
		return DC_STATUS_PROTOCOL;
	}

	// Update and emit a progress event.
	if (progress) {
		progress->current = initial + STEP (nbytes, size);
		device_event_emit (abstract, DC_EVENT_PROGRESS, progress);
	}

	unsigned int npackets = 0;
	while (nbytes < size) {
		// Get the command byte.
		unsigned char toggle = npackets % 2;
		unsigned char cmd = toggle == 0 ? CMD_OBJ_EVEN : CMD_OBJ_ODD;

		// Get the packet size.
		unsigned int len = size - nbytes;
		if (len > maxpacket) {
			len = maxpacket;
		}

		// Transfer the segment packet.
		unsigned char rsp_segment[1 + 504];
		unsigned char cmd_segment[] = {cmd, cmd ^ XOR};
		status = mares_iconhd_transfer (device, cmd_segment, sizeof (cmd_segment), rsp_segment, len + 1);
		if (status != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to transfer the segment packet.");
			return status;
		}

		// Verify the packet header.
		if ((rsp_segment[0] & 0xF0) >> 4 != toggle) {
			ERROR (abstract->context, "Unexpected packet header (%02x).", rsp_segment[0]);
			return DC_STATUS_PROTOCOL;
		}

		// Append the payload to the output buffer.
		if (!dc_buffer_append (buffer, rsp_segment + 1, len)) {
			ERROR (abstract->context, "Insufficient buffer space available.");
			return DC_STATUS_NOMEMORY;
		}

		nbytes += len;
		npackets++;

		// Update and emit the progress events.
		if (progress) {
			progress->current = initial + STEP (nbytes, size);
			device_event_emit (abstract, DC_EVENT_PROGRESS, progress);
		}
	}

	return status;
}

dc_status_t
mares_iconhd_device_open (dc_device_t **out, dc_context_t *context, dc_iostream_t *iostream)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	mares_iconhd_device_t *device = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	// Allocate memory.
	device = (mares_iconhd_device_t *) dc_device_allocate (context, &mares_iconhd_device_vtable);
	if (device == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Set the default values.
	device->iostream = iostream;
	device->layout = NULL;
	memset (device->fingerprint, 0, sizeof (device->fingerprint));
	device->fingerprint_size = sizeof (device->fingerprint);
	memset (device->version, 0, sizeof (device->version));
	device->model = 0;
	device->packetsize = 0;
	memset (device->cache, 0, sizeof (device->cache));
	device->available = 0;
	device->offset = 0;

	/*
	 * At least the Mares Matrix needs the command to be split into
	 * base and argument, with a wait for the ACK byte in between.
	 *
	 * See commit 59bfb0f3189b ("Add support for the Mares Matrix")
	 * for details.
	 */
	device->splitcommand = 1;

	// Set the serial communication protocol (115200 8E1).
	status = dc_iostream_configure (device->iostream, 115200, 8, DC_PARITY_EVEN, DC_STOPBITS_ONE, DC_FLOWCONTROL_NONE);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the terminal attributes.");
		goto error_free;
	}

	// Set the timeout for receiving data (3000 ms).
	status = dc_iostream_set_timeout (device->iostream, 3000);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to set the timeout.");
		goto error_free;
	}

	// Clear the DTR line.
	status = dc_iostream_set_dtr (device->iostream, 0);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to clear the DTR line.");
		goto error_free;
	}

	// Clear the RTS line.
	status = dc_iostream_set_rts (device->iostream, 0);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (context, "Failed to clear the RTS line.");
		goto error_free;
	}

	// Make sure everything is in a sane state.
	dc_iostream_purge (device->iostream, DC_DIRECTION_ALL);

	// Send the version command.
	unsigned char command[] = {CMD_VERSION, CMD_VERSION ^ XOR};
	status = mares_iconhd_transfer (device, command, sizeof (command),
		device->version, sizeof (device->version));
	if (status != DC_STATUS_SUCCESS) {
		goto error_free;
	}

	// Autodetect the model using the version packet.
	device->model = mares_iconhd_get_model (device);

	// Read the size of the flash memory.
	unsigned int memsize = 0;
	if (device->model == QUAD) {
		unsigned char cmd_flash[] = {CMD_FLASHSIZE, CMD_FLASHSIZE ^ XOR};
		unsigned char rsp_flash[4] = {0};
		status = mares_iconhd_transfer (device, cmd_flash, sizeof (cmd_flash), rsp_flash, sizeof (rsp_flash));
		if (status != DC_STATUS_SUCCESS) {
			WARNING (context, "Failed to read the flash memory size.");
		} else {
			memsize = array_uint32_le (rsp_flash);
			DEBUG (context, "Flash memory size is %u bytes.", memsize);
		}
	}

	// Load the correct memory layout.
	switch (device->model) {
	case MATRIX:
		device->layout = &mares_matrix_layout;
		device->packetsize = 256;
		break;
	case PUCKPRO:
	case PUCK2:
	case NEMOWIDE2:
	case SMART:
	case SMARTAPNEA:
		device->layout = &mares_nemowide2_layout;
		device->packetsize = 256;
		break;
	case QUAD:
		if (memsize > 0x40000) {
			device->layout = &mares_iconhd_layout;
		} else {
			device->layout = &mares_nemowide2_layout;
		}
		device->packetsize = 256;
		break;
	case QUADAIR:
	case SMARTAIR:
		device->layout = &mares_iconhdnet_layout;
		device->packetsize = 256;
		break;
	case GENIUS:
	case HORIZON:
		device->layout = &mares_genius_layout;
		device->packetsize = 4096;
		device->fingerprint_size = 4;
		break;
	case ICONHDNET:
		device->layout = &mares_iconhdnet_layout;
		device->packetsize = 4096;
		break;
	case ICONHD:
	default:
		device->layout = &mares_iconhd_layout;
		device->packetsize = 4096;
		break;
	}

	if (dc_iostream_get_transport(device->iostream) == DC_TRANSPORT_BLE) {
		/*
		 * Don't ask for larger amounts of data with the BLE
		 * transport - it will fail.  I suspect there is a buffer
		 * overflow in the BlueLink Pro dongle when bluetooth is
		 * slower than the serial protocol that the dongle talks to
		 * the dive computer.
		 */
		if (device->packetsize > 128)
			device->packetsize = 128;
		/*
		 * With BLE, don't wait for ACK before sending the arguments
		 * to a command.
		 *
		 * There is some timing issue that makes that take too long
		 * and causes the command to be aborted.
		 */
		device->splitcommand = 0;
	}

	*out = (dc_device_t *) device;

	return DC_STATUS_SUCCESS;

error_free:
	dc_device_deallocate ((dc_device_t *) device);
	return status;
}


static dc_status_t
mares_iconhd_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size)
{
	mares_iconhd_device_t *device = (mares_iconhd_device_t *) abstract;

	if (size && size != device->fingerprint_size)
		return DC_STATUS_INVALIDARGS;

	if (size)
		memcpy (device->fingerprint, data, device->fingerprint_size);
	else
		memset (device->fingerprint, 0, device->fingerprint_size);

	return DC_STATUS_SUCCESS;
}


static dc_status_t
mares_iconhd_device_read (dc_device_t *abstract, unsigned int address, unsigned char data[], unsigned int size)
{
	dc_status_t rc = DC_STATUS_SUCCESS;
	mares_iconhd_device_t *device = (mares_iconhd_device_t *) abstract;

	unsigned int nbytes = 0;
	while (nbytes < size) {
		// Calculate the packet size.
		unsigned int len = size - nbytes;
		if (len > device->packetsize)
			len = device->packetsize;

		// Read the packet.
		unsigned char command[] = {CMD_READ, CMD_READ ^ XOR,
			(address      ) & 0xFF,
			(address >>  8) & 0xFF,
			(address >> 16) & 0xFF,
			(address >> 24) & 0xFF,
			(len      ) & 0xFF,
			(len >>  8) & 0xFF,
			(len >> 16) & 0xFF,
			(len >> 24) & 0xFF};
		rc = mares_iconhd_transfer (device, command, sizeof (command), data, len);
		if (rc != DC_STATUS_SUCCESS)
			return rc;

		nbytes += len;
		address += len;
		data += len;
	}

	return rc;
}


static dc_status_t
mares_iconhd_device_dump (dc_device_t *abstract, dc_buffer_t *buffer)
{
	mares_iconhd_device_t *device = (mares_iconhd_device_t *) abstract;

	// Allocate the required amount of memory.
	if (!dc_buffer_resize (buffer, device->layout->memsize)) {
		ERROR (abstract->context, "Insufficient buffer space available.");
		return DC_STATUS_NOMEMORY;
	}

	// Emit a vendor event.
	dc_event_vendor_t vendor;
	vendor.data = device->version;
	vendor.size = sizeof (device->version);
	device_event_emit (abstract, DC_EVENT_VENDOR, &vendor);

	return device_dump_read (abstract, dc_buffer_get_data (buffer),
		dc_buffer_get_size (buffer), device->packetsize);
}

static dc_status_t
mares_iconhd_device_foreach_raw (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	dc_status_t rc = DC_STATUS_SUCCESS;
	mares_iconhd_device_t *device = (mares_iconhd_device_t *) abstract;

	const mares_iconhd_layout_t *layout = device->layout;

	// Enable progress notifications.
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	progress.maximum = layout->rb_profile_end - layout->rb_profile_begin;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Get the model code.
	unsigned int model = device->model;

	// Get the corresponding dive header size.
	unsigned int header = 0x5C;
	if (model == ICONHDNET)
		header = 0x80;
	else if (model == QUADAIR)
		header = 0x84;
	else if (model == SMART || model == SMARTAIR)
		header = 4; // Type and number of samples only!
	else if (model == SMARTAPNEA)
		header = 6; // Type and number of samples only!

	// Get the end of the profile ring buffer.
	unsigned int eop = 0;
	const unsigned int config[] = {0x2001, 0x3001};
	for (unsigned int i = 0; i < sizeof (config) / sizeof (*config); ++i) {
		// Read the pointer.
		unsigned char pointer[4] = {0};
		rc = mares_iconhd_device_read (abstract, config[i], pointer, sizeof (pointer));
		if (rc != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to read the memory.");
			return rc;
		}

		// Update and emit a progress event.
		progress.maximum += sizeof (pointer);
		progress.current += sizeof (pointer);
		device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

		eop = array_uint32_le (pointer);
		if (eop != 0xFFFFFFFF)
			break;
	}
	if (eop < layout->rb_profile_begin || eop >= layout->rb_profile_end) {
		if (eop == 0xFFFFFFFF)
			return DC_STATUS_SUCCESS; // No dives available.
		ERROR (abstract->context, "Ringbuffer pointer out of range (0x%08x).", eop);
		return DC_STATUS_DATAFORMAT;
	}

	// Create the ringbuffer stream.
	dc_rbstream_t *rbstream = NULL;
	rc = dc_rbstream_new (&rbstream, abstract, 1, device->packetsize, layout->rb_profile_begin, layout->rb_profile_end, eop);
	if (rc != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to create the ringbuffer stream.");
		return rc;
	}

	// Allocate memory for the dives.
	unsigned char *buffer = (unsigned char *) malloc (layout->rb_profile_end - layout->rb_profile_begin);
	if (buffer == NULL) {
		ERROR (abstract->context, "Failed to allocate memory.");
		dc_rbstream_free (rbstream);
		return DC_STATUS_NOMEMORY;
	}

	unsigned int offset = layout->rb_profile_end - layout->rb_profile_begin;
	while (offset >= header + 4) {
		// Read the first part of the dive header.
		rc = dc_rbstream_read (rbstream, &progress, buffer + offset - header, header);
		if (rc != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to read the dive.");
			dc_rbstream_free (rbstream);
			free (buffer);
			return rc;
		}

		// Get the number of samples in the profile data.
		unsigned int type = 0, nsamples = 0;
		if (model == SMART || model == SMARTAPNEA || model == SMARTAIR) {
			type     = array_uint16_le (buffer + offset - header + 2);
			nsamples = array_uint16_le (buffer + offset - header + 0);
		} else {
			type     = array_uint16_le (buffer + offset - header + 0);
			nsamples = array_uint16_le (buffer + offset - header + 2);
		}
		if (nsamples == 0xFFFF || type == 0xFFFF)
			break;

		// Get the dive mode.
		unsigned int mode = type & 0x03;

		// Get the header/sample size and fingerprint offset.
		unsigned int headersize = 0x5C;
		unsigned int samplesize = 8;
		unsigned int fingerprint = 6;
		if (model == ICONHDNET) {
			headersize = 0x80;
			samplesize = 12;
		} else if (model == QUADAIR) {
			headersize = 0x84;
			samplesize = 12;
		} else if (model == SMART) {
			if (mode == FREEDIVE) {
				headersize = 0x2E;
				samplesize = 6;
				fingerprint = 0x20;
			} else {
				headersize = 0x5C;
				samplesize = 8;
				fingerprint = 2;
			}
		} else if (model == SMARTAPNEA) {
			headersize = 0x50;
			samplesize = 14;
			fingerprint = 0x40;
		} else if (model == SMARTAIR) {
			if (mode == FREEDIVE) {
				headersize = 0x30;
				samplesize = 6;
				fingerprint = 0x22;
			} else {
				headersize = 0x84;
				samplesize = 12;
				fingerprint = 2;
			}
		}
		if (offset < headersize)
			break;

		// Read the second part of the dive header.
		rc = dc_rbstream_read (rbstream, &progress, buffer + offset - headersize, headersize - header);
		if (rc != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to read the dive.");
			dc_rbstream_free (rbstream);
			free (buffer);
			return rc;
		}

		// Calculate the total number of bytes for this dive.
		// If the buffer does not contain that much bytes, we reached the
		// end of the ringbuffer. The current dive is incomplete (partially
		// overwritten with newer data), and processing should stop.
		unsigned int nbytes = 4 + headersize + nsamples * samplesize;
		if (model == ICONHDNET || model == QUADAIR || (model == SMARTAIR && mode != FREEDIVE)) {
			nbytes += (nsamples / 4) * 8;
		} else if (model == SMARTAPNEA) {
			unsigned int settings = array_uint16_le (buffer + offset - headersize + 0x1C);
			unsigned int divetime = array_uint32_le (buffer + offset - headersize + 0x24);
			unsigned int samplerate = 1 << ((settings >> 9) & 0x03);

			nbytes += divetime * samplerate * 2;
		}
		if (offset < nbytes)
			break;

		// Read the remainder of the dive.
		rc = dc_rbstream_read (rbstream, &progress, buffer + offset - nbytes, nbytes - headersize);
		if (rc != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to read the dive.");
			dc_rbstream_free (rbstream);
			free (buffer);
			return rc;
		}

		// Move to the start of the dive.
		offset -= nbytes;

		// Verify that the length that is stored in the profile data
		// equals the calculated length. If both values are different,
		// we assume we reached the last dive.
		unsigned int length = array_uint32_le (buffer + offset);
		if (length != nbytes)
			break;

		unsigned char *fp = buffer + offset + length - headersize + fingerprint;
		if (memcmp (fp, device->fingerprint, sizeof (device->fingerprint)) == 0) {
			break;
		}

		if (callback && !callback (buffer + offset, length, fp, sizeof (device->fingerprint), userdata)) {
			break;
		}
	}

	dc_rbstream_free (rbstream);
	free (buffer);

	return rc;
}

static dc_status_t
mares_iconhd_device_foreach_object (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	dc_status_t rc = DC_STATUS_SUCCESS;
	mares_iconhd_device_t *device = (mares_iconhd_device_t *) abstract;

	// Enable progress notifications.
	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Allocate memory for the dives.
	dc_buffer_t *buffer = dc_buffer_new (4096);
	if (buffer == NULL) {
		ERROR (abstract->context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Read the number of dives.
	rc = mares_iconhd_read_object (device, NULL, buffer, OBJ_LOGBOOK, OBJ_LOGBOOK_COUNT);
	if (rc != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to read the number of dives.");
		dc_buffer_free (buffer);
		return rc;
	}

	if (dc_buffer_get_size (buffer) < 2) {
		ERROR (abstract->context, "Unexpected number of bytes received (" DC_PRINTF_SIZE ").",
			dc_buffer_get_size (buffer));
		dc_buffer_free (buffer);
		return DC_STATUS_PROTOCOL;
	}

	// Get the number of dives.
	unsigned int ndives = array_uint16_le (dc_buffer_get_data(buffer));

	// Update and emit a progress event.
	progress.current = 1 * NSTEPS;
	progress.maximum = (1 + ndives * 2) * NSTEPS;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	// Download the dives.
	for (unsigned int i = 0; i < ndives; ++i) {
		// Erase the buffer.
		dc_buffer_clear (buffer);

		// Read the dive header.
		rc = mares_iconhd_read_object (device, &progress, buffer, OBJ_DIVE + i, OBJ_DIVE_HEADER);
		if (rc != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to read the dive header.");
			break;
		}

		// Check the fingerprint data.
		if (memcmp (dc_buffer_get_data (buffer) + 0x08, device->fingerprint, device->fingerprint_size) == 0) {
			INFO (abstract->context, "Stopping due to detecting a matching fingerprint");
			break;
		}

		// Read the dive data.
		rc = mares_iconhd_read_object (device, &progress, buffer, OBJ_DIVE + i, OBJ_DIVE_DATA);
		if (rc != DC_STATUS_SUCCESS) {
			ERROR (abstract->context, "Failed to read the dive data.");
			break;
		}

		const unsigned char *data = dc_buffer_get_data (buffer);
		if (callback && !callback (data, dc_buffer_get_size (buffer), data + 0x08, device->fingerprint_size, userdata)) {
			break;
		}
	}

	dc_buffer_free(buffer);

	return rc;
}

static dc_status_t
mares_iconhd_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	dc_status_t rc = DC_STATUS_SUCCESS;
	mares_iconhd_device_t *device = (mares_iconhd_device_t *) abstract;

	// Emit a vendor event.
	dc_event_vendor_t vendor;
	vendor.data = device->version;
	vendor.size = sizeof (device->version);
	device_event_emit (abstract, DC_EVENT_VENDOR, &vendor);

	// Read the serial number.
	unsigned char serial[4] = {0};
	rc = mares_iconhd_device_read (abstract, 0x0C, serial, sizeof (serial));
	if (rc != DC_STATUS_SUCCESS) {
		ERROR (abstract->context, "Failed to read the memory.");
		return rc;
	}

	// Emit a device info event.
	dc_event_devinfo_t devinfo;
	devinfo.model = device->model;
	devinfo.firmware = 0;
	devinfo.serial = array_uint32_le (serial);
	device_event_emit (abstract, DC_EVENT_DEVINFO, &devinfo);

	if (device->model == GENIUS || device->model == HORIZON) {
		return mares_iconhd_device_foreach_object (abstract, callback, userdata);
	} else {
		return mares_iconhd_device_foreach_raw (abstract, callback, userdata);
	}
}
