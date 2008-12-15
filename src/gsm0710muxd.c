/*
 * GSM 07.10 Implementation with User Space Serial Ports
 *
 * Code heavily based on gsmMuxd written by
 * Copyright (C) 2003 Tuukka Karvonen <tkarvone@iki.fi>
 * Modified November 2004 by David Jander <david@protonic.nl>
 * Modified January 2006 by Tuukka Karvonen <tkarvone@iki.fi>
 * Modified January 2006 by Antti Haapakoski <antti.haapakoski@iki.fi>
 * Modified March 2006 by Tuukka Karvonen <tkarvone@iki.fi>
 * Modified October 2006 by Vasiliy Novikov <vn@hotbox.ru>
 *
 * Copyright (C) 2008 M. Dietrich <mdt@pyneo.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <features.h>
#include <paths.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <syslog.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <glib.h> // http://library.gnome.org/devel/glib/unstable/glib-core.html
#include "muxercontrol.h"

// http://maemo.org/api_refs/4.0/dbus-glib/group__DBusGLibInternals.html#gfac56b6025a90951510d33423ff04120
// http://wiki.bluez.org/wiki/HOWTO/DiscoveringDevices
// http://dbus.freedesktop.org/doc/api/html/example-service_8c-source.html
// ~/Source/openmoko/build/tmp/work/i686-linux/glib-2.0-native-2.12.4-r1/glib-2.12.4/tests/mainloop-test.c
// http://www.linuxquestions.org/questions/linux-software-2/dbus-problem-505442/

///////////////////////////////////////////////////////////////// defines
#define LOG(lvl, f, ...) do{\
	if(lvl<=syslog_level){\
		if (mylogfile){\
			fprintf(mylogfile,"%s:%d: [%d] %s(): " f "\n", __FILE__, __LINE__, lvl, __FUNCTION__, ##__VA_ARGS__);\
			fflush(mylogfile);\
		}else{\
			syslog(lvl,"%s:%d:%s(): " f "\n", __FILE__, __LINE__, __FUNCTION__, ##__VA_ARGS__);\
	}}}while(0)

#define SYSCHECK(c) do{\
		if((c)<0){\
			LOG(LOG_ERR, "system-error: '%s' (code: %d)", strerror(errno), errno);\
			return -1;\
		}\
	}while(0)

#define GSM0710_FRAME_FLAG 0xF9// basic mode flag for frame start and end
#define GSM0710_FRAME_ADV_FLAG 0x7E// advanced mode flag for frame start and end
#define GSM0710_FRAME_ADV_ESC 0x7D// advanced mode escape symbol
#define GSM0710_FRAME_ADV_ESC_COPML 0x20// advanced mode escape complement mask
#define GSM0710_FRAME_ADV_ESCAPED_SYMS { GSM0710_FRAME_ADV_FLAG, GSM0710_FRAME_ADV_ESC, 0x11, 0x91, 0x13, 0x93 }// advanced mode escaped symbols: Flag, Escape, XON and XOFF
// bits: Poll/final, Command/Response, Extension
#define GSM0710_PF 0x10//16
#define GSM0710_CR 0x02//2
#define GSM0710_EA 0x01//1
// type of frames
#define GSM0710_TYPE_SABM 0x2F//47 Set Asynchronous Balanced Mode
#define GSM0710_TYPE_UA 0x63//99 Unnumbered Acknowledgement
#define GSM0710_TYPE_DM 0x0F//15 Disconnected Mode
#define GSM0710_TYPE_DISC 0x43//67 Disconnect
#define GSM0710_TYPE_UIH 0xEF//239 Unnumbered information with header check
#define GSM0710_TYPE_UI 0x03//3 Unnumbered Acknowledgement
// control channel commands
#define GSM0710_CONTROL_PN (0x80|GSM0710_EA)//?? DLC parameter negotiation
#define GSM0710_CONTROL_CLD (0xC0|GSM0710_EA)//193 Multiplexer close down
#define GSM0710_CONTROL_PSC (0x40|GSM0710_EA)//??? Power Saving Control
#define GSM0710_CONTROL_TEST (0x20|GSM0710_EA)//33 Test Command
#define GSM0710_CONTROL_MSC (0xE0|GSM0710_EA)//225 Modem Status Command
#define GSM0710_CONTROL_NSC (0x10|GSM0710_EA)//17 Non Supported Command Response
#define GSM0710_CONTROL_RPN (0x90|GSM0710_EA)//?? Remote Port Negotiation Command
#define GSM0710_CONTROL_RLS (0x50|GSM0710_EA)//?? Remote Line Status Command
#define GSM0710_CONTROL_SNC (0xD0|GSM0710_EA)//?? Service Negotiation Command 
// V.24 signals: flow control, ready to communicate, ring indicator,
// data valid three last ones are not supported by Siemens TC_3x
#define GSM0710_SIGNAL_FC 0x02
#define GSM0710_SIGNAL_RTC 0x04
#define GSM0710_SIGNAL_RTR 0x08
#define GSM0710_SIGNAL_IC 0x40//64
#define GSM0710_SIGNAL_DV 0x80//128
#define GSM0710_SIGNAL_DTR 0x04
#define GSM0710_SIGNAL_DSR 0x04
#define GSM0710_SIGNAL_RTS 0x08
#define GSM0710_SIGNAL_CTS 0x08
#define GSM0710_SIGNAL_DCD 0x80//128
//
#define GSM0710_COMMAND_IS(type, command) ((type & ~GSM0710_CR) == command)
#define GSM0710_FRAME_IS(type, frame) ((frame->control & ~GSM0710_PF) == type)
#ifndef min
#define min(a,b) ((a < b) ? a :b)
#endif
#define GSM0710_WRITE_RETRIES 5
#define GSM0710_MAX_CHANNELS 32
// Defines how often the modem is polled when automatic restarting is
// enabled The value is in seconds
#define GSM0710_POLLING_INTERVAL 5
#define GSM0710_BUFFER_SIZE 2048
#define WAKEUP "\x1a"

////////////////////////////////////////////////////// types
//
typedef struct GSM0710_Frame
{
	unsigned char channel;
	unsigned char control;
	int length;
	unsigned char *data;
} GSM0710_Frame;

//
typedef struct GSM0710_Buffer
{
	unsigned char data[GSM0710_BUFFER_SIZE];
	unsigned char *readp;
	unsigned char *writep;
	unsigned char *endp;
	int flag_found;// set if last character read was flag
	unsigned long received_count;
	unsigned long dropped_count;
	unsigned char adv_data[GSM0710_BUFFER_SIZE];
	int adv_length;
	int adv_found_esc;
} GSM0710_Buffer;

// Channel data 
typedef struct Channel
{
	int id; // gsm 07 10 channel id
	char* devicename;
	int fd;
	int opened;
	unsigned char v24_signals;
	char* ptsname;
	char* purpose;
	int remaining;
	unsigned char *tmp;
	guint g_source;
} Channel;

typedef enum MuxerStates 
{
	MUX_STATE_OPENING,
	MUX_STATE_INITILIZING,
	MUX_STATE_MUXING,
	MUX_STATE_CLOSING,
	MUX_STATE_OFF,
	MUX_STATES_COUNT // keep this the last
} MuxerStates;

typedef struct Serial
{
	char *devicename;
	char* pm_base_dir;
	int fd;
	MuxerStates state;
	GSM0710_Buffer *in_buf;// input buffer
	unsigned char *adv_frame_buf;
	time_t frame_receive_time;
	int ping_number;
	guint g_source;
	guint g_source_watchdog;
} Serial;

typedef enum AdditionFunctionality {
	AF_NONE = 0,
	AF_WAKEUP_WITH_SYSFS = 1,
	AF_WAKEUP_WITH_SEQUENCE = 2,
	AF_SIEMENS_C35 = 4,
	AF_ENFORA = 8,
	AF_SHORT_INIT = 16,
} AdditionFunctionality;

/////////////////////////////////////////// function prototypes
/**
 * increases buffer pointer by one and wraps around if necessary
 */
//void gsm0710_buffer_inc(GSM0710_Buffer *buf, void&* p);
#define gsm0710_buffer_inc(buf,p) do { p++; if (p == buf->endp) p = buf->data; } while (0)
/**
 * Tells how many chars are saved into the buffer.
 */
//int gsm0710_buffer_length(GSM0710_Buffer *buf);
#define gsm0710_buffer_length(buf) ((buf->readp > buf->writep) ? \
	(GSM0710_BUFFER_SIZE - (buf->readp - buf->writep)) : \
	(buf->writep-buf->readp))
/**
 * tells how much free space there is in the buffer
 */
//int gsm0710_buffer_free(GSM0710_Buffer *buf);
#define gsm0710_buffer_free(buf) ((buf->readp > buf->writep) ? \
	(buf->readp - buf->writep) : \
	(GSM0710_BUFFER_SIZE - (buf->writep-buf->readp)) - 1)

////////////////////////////////// constants & globals
static unsigned char close_channel_cmd[] = { GSM0710_CONTROL_CLD | GSM0710_CR, GSM0710_EA | (0 << 1) };
static unsigned char test_channel_cmd[] = { GSM0710_CONTROL_TEST | GSM0710_CR, GSM0710_EA | (6 << 1), 'P', 'I', 'N', 'G', '\r', '\n', };
//static unsigned char psc_channel_cmd[] = { GSM0710_CONTROL_PSC | GSM0710_CR, GSM0710_EA | (0 << 1), };
static unsigned char wakeup_sequence[] = { GSM0710_FRAME_FLAG, };
// crc table from gsm0710 spec
static const unsigned char r_crctable[] = {//reversed, 8-bit, poly=0x07
	0x00, 0x91, 0xE3, 0x72, 0x07, 0x96, 0xE4, 0x75, 0x0E, 0x9F, 0xED,
	0x7C, 0x09, 0x98, 0xEA, 0x7B, 0x1C, 0x8D, 0xFF, 0x6E, 0x1B, 0x8A,
	0xF8, 0x69, 0x12, 0x83, 0xF1, 0x60, 0x15, 0x84, 0xF6, 0x67, 0x38,
	0xA9, 0xDB, 0x4A, 0x3F, 0xAE, 0xDC, 0x4D, 0x36, 0xA7, 0xD5, 0x44,
	0x31, 0xA0, 0xD2, 0x43, 0x24, 0xB5, 0xC7, 0x56, 0x23, 0xB2, 0xC0,
	0x51, 0x2A, 0xBB, 0xC9, 0x58, 0x2D, 0xBC, 0xCE, 0x5F, 0x70, 0xE1,
	0x93, 0x02, 0x77, 0xE6, 0x94, 0x05, 0x7E, 0xEF, 0x9D, 0x0C, 0x79,
	0xE8, 0x9A, 0x0B, 0x6C, 0xFD, 0x8F, 0x1E, 0x6B, 0xFA, 0x88, 0x19,
	0x62, 0xF3, 0x81, 0x10, 0x65, 0xF4, 0x86, 0x17, 0x48, 0xD9, 0xAB,
	0x3A, 0x4F, 0xDE, 0xAC, 0x3D, 0x46, 0xD7, 0xA5, 0x34, 0x41, 0xD0,
	0xA2, 0x33, 0x54, 0xC5, 0xB7, 0x26, 0x53, 0xC2, 0xB0, 0x21, 0x5A,
	0xCB, 0xB9, 0x28, 0x5D, 0xCC, 0xBE, 0x2F, 0xE0, 0x71, 0x03, 0x92,
	0xE7, 0x76, 0x04, 0x95, 0xEE, 0x7F, 0x0D, 0x9C, 0xE9, 0x78, 0x0A,
	0x9B, 0xFC, 0x6D, 0x1F, 0x8E, 0xFB, 0x6A, 0x18, 0x89, 0xF2, 0x63,
	0x11, 0x80, 0xF5, 0x64, 0x16, 0x87, 0xD8, 0x49, 0x3B, 0xAA, 0xDF,
	0x4E, 0x3C, 0xAD, 0xD6, 0x47, 0x35, 0xA4, 0xD1, 0x40, 0x32, 0xA3,
	0xC4, 0x55, 0x27, 0xB6, 0xC3, 0x52, 0x20, 0xB1, 0xCA, 0x5B, 0x29,
	0xB8, 0xCD, 0x5C, 0x2E, 0xBF, 0x90, 0x01, 0x73, 0xE2, 0x97, 0x06,
	0x74, 0xE5, 0x9E, 0x0F, 0x7D, 0xEC, 0x99, 0x08, 0x7A, 0xEB, 0x8C,
	0x1D, 0x6F, 0xFE, 0x8B, 0x1A, 0x68, 0xF9, 0x82, 0x13, 0x61, 0xF0,
	0x85, 0x14, 0x66, 0xF7, 0xA8, 0x39, 0x4B, 0xDA, 0xAF, 0x3E, 0x4C,
	0xDD, 0xA6, 0x37, 0x45, 0xD4, 0xA1, 0x30, 0x42, 0xD3, 0xB4, 0x25,
	0x57, 0xC6, 0xB3, 0x22, 0x50, 0xC1, 0xBA, 0x2B, 0x59, 0xC8, 0xBD,
	0x2C, 0x5E, 0xCF, };
// config stuff
static int no_daemon = 1;
static int pin_code = -1;
static AdditionFunctionality additional_functionality = 0;
static int use_ping = 0;
static int use_timeout = 0;
static FILE * mylogfile;
static int syslog_level = LOG_INFO;
// serial io
static Serial serial;
// muxed io channels
static Channel channellist[GSM0710_MAX_CHANNELS]; // remember: [0] is not used acticly because it's the control channel
// +CMUX=<mode>[,<subset>[,<port_speed>[,<N1>[,<T1>[,<N2>[,<T2>[,<T3>[,<k>]]]]]]]]
static int cmux_mode = 1;
static int cmux_subset = 0;
static int cmux_port_speed = 5;
static int at_port_speed = 5;
// Maximum Frame Size (N1): 64/31
static int cmux_N1 = 64;
#if 0
// Acknowledgement Timer (T1) sec/100: 10 
static int cmux_T1 = 10;
// Maximum number of retransmissions (N2): 3
static int cmux_N2 = 3;
// Response Timer for multiplexer control channel (T2) sec/100: 30
static int cmux_T2 = 30;
// Response Timer for wake-up procedure(T3) sec: 10
static int cmux_T3 = 10;
// Window Size (k): 2
static int cmux_k = 2;
#endif
// TODO: set automatically from at+cmux=?
// neo: +CMUX: (1),(0),(1-5),(10-100),(1-255),(0-100),(2-255),(1-255),(1-7)
/*
 * The following arrays must have equal length and the values must
 * correspond. also it has to correspond to the gsm0710 spec regarding
 * baud id of CMUX the command.
 */
static int baud_rates[] = {
	0, 9600, 19200, 38400, 57600, 115200, 230400, 460800
};
static speed_t baud_bits[] = {
	0, B9600, B19200, B38400, B57600, B115200, B230400, B460800
};
int uih_pf_bit_received = 0;

/**
 * Determine baud-rate index for CMUX command
 */
static int baud_rate_index(
	int baud_rate)
{
	int i;
	for (i = 0; i < sizeof(baud_rates) / sizeof(*baud_rates); ++i)
		if (baud_rates[i] == baud_rate)
			return i;
	return -1;
}

/**
 * Calculates frame check sequence from given characters.
 *
 * PARAMS:
 * input - character array
 * length - number of characters in array (that are included)
 * RETURNS:
 * frame check sequence
 */
static unsigned char frame_calc_crc(
	const unsigned char *input,
	int length)
{
	unsigned char fcs = 0xFF;
	int i;
	for (i = 0; i < length; i++)
		fcs = r_crctable[fcs ^ input[i]];
	return 0xFF - fcs;
}

/**
 * Escapes GSM0710_FRAME_ADV_ESCAPED_SYMS characters.
 * returns escaped buffer length.
 */
static int fill_adv_frame_buf(
	unsigned char *adv_buf,
	const unsigned char *data,
	int length)
{
	static const unsigned char esc[] = GSM0710_FRAME_ADV_ESCAPED_SYMS;
	int i, esc_i, adv_i = 0;
	for (i = 0; i < length; ++i, ++adv_i)
	{
		adv_buf[adv_i] = data[i];
		for (esc_i = 0; esc_i < sizeof(esc) / sizeof(esc[0]); ++esc_i)
			if (data[i] == esc[esc_i])
			{
				adv_buf[adv_i] = GSM0710_FRAME_ADV_ESC;
				adv_i++;
				adv_buf[adv_i] = data[i] ^ GSM0710_FRAME_ADV_ESC_COPML;
				break;
			}
	}
	return adv_i;
}

/**
 * ascii/hexdump a byte buffer
 */
static int syslogdump(
	const char *prefix,
	const unsigned char *ptr,
	unsigned int length)
{
	if (LOG_DEBUG>syslog_level)
		return 0;
	char buffer[100];
	unsigned int offset = 0l;
	int i;
	while (offset < length)
	{
		int off;
		strcpy(buffer, prefix);
		off = strlen(buffer);
		SYSCHECK(snprintf(buffer + off, sizeof(buffer) - off, "%08x: ", offset));
		off = strlen(buffer);
		for (i = 0; i < 16; i++)
		{
			if (offset + i < length)
				SYSCHECK(snprintf(buffer + off, sizeof(buffer) - off, "%02x%c", ptr[offset + i], i == 7 ? '-' : ' '));
			else
				SYSCHECK(snprintf(buffer + off, sizeof(buffer) - off, " .%c", i == 7 ? '-' : ' '));
			off = strlen(buffer);
		}
		SYSCHECK(snprintf(buffer + off, sizeof(buffer) - off, " "));
		off = strlen(buffer);
		for (i = 0; i < 16; i++)
			if (offset + i < length)
			{
				SYSCHECK(snprintf(buffer + off, sizeof(buffer) - off, "%c", (ptr[offset + i] < ' ' || ptr[offset + i] > 127) ? '.' : ptr[offset + i]));
				off = strlen(buffer);
			}
		offset += 16;
		LOG(LOG_DEBUG, "%s", buffer);
	}
	return 0;
}

static int modem_hw_(const char* pm_base_dir, const char* entry, int on)
{
	LOG(LOG_DEBUG, "Enter");
	if (pm_base_dir != NULL)
	{
		char fn[256];
		SYSCHECK(snprintf(fn, sizeof(fn), "%s/%s", pm_base_dir, entry));
		LOG(LOG_DEBUG, "echo %c > %s", on?'1':'0', fn);
		int fd;
		SYSCHECK(fd = open(fn, O_RDWR | O_NONBLOCK));
		SYSCHECK(write(fd, on?"1\n":"0\n", 2));
		SYSCHECK(close(fd));
	}
	else
		LOG(LOG_DEBUG, "no pm_base_dir");
	LOG(LOG_DEBUG, "Leave");
	return 0;
}

/**
 * Writes a frame to a logical channel. C/R bit is set to 1.
 * Doesn't support FCS counting for GSM0710_TYPE_UI frames.
 *
 * PARAMS:
 * channel - channel number (0 = control)
 * input - the data to be written
 * length - the length of the data
 * type - the type of the frame (with possible P/F-bit)
 *
 * RETURNS:
 * number of characters written
 */
static int write_frame(
	int channel,
	const unsigned char *input,
	int length,
	unsigned char type)
{
	LOG(LOG_DEBUG, "Enter");
//flag, GSM0710_EA=1 C channel, frame type, length 1-2
	unsigned char prefix[5] = { GSM0710_FRAME_FLAG, GSM0710_EA | GSM0710_CR, 0, 0, 0 };
	unsigned char postfix[2] = { 0xFF, GSM0710_FRAME_FLAG };
	int prefix_length = 4;
	int c;
	if (additional_functionality & AF_WAKEUP_WITH_SYSFS)
		modem_hw_(serial.pm_base_dir, "wake", 1);
	else if (additional_functionality & AF_WAKEUP_WITH_SEQUENCE)
	{
//	char w = 0;
//	int count = 0;
//	do
//	{
		syslogdump(">s ", (unsigned char *)wakeup_sequence, sizeof(wakeup_sequence));
		c = write(serial.fd, wakeup_sequence, sizeof(wakeup_sequence));
//		SYSCHECK(tcdrain(serial.fd));
//		fd_set rfds;
//		FD_ZERO(&rfds);
//		FD_SET(serial.fd, &rfds);
//		struct timeval timeout;
//		timeout.tv_sec = 0;
//		timeout.tv_usec = 1000 / 100 * cmux_T2;
//		int sel = select(serial.fd + 1, &rfds, NULL, NULL, &timeout);
//		if (sel > 0 && FD_ISSET(serial.fd, &rfds))
//			read(serial.fd, &w, 1);
//		else
//			count++;
//	} while (w != wakeup_sequence[0] && count < cmux_N2);
//	if (w != wakeup_sequence[0])
//		LOG(LOG_WARNING, "Didn't get frame-flag after wakeup");
	}
	LOG(LOG_DEBUG, "Sending frame to channel %d", channel);
//GSM0710_EA=1, Command, let's add address
	prefix[1] = prefix[1] | ((63 & (unsigned char) channel) << 2);
//let's set control field
	prefix[2] = type;
	if (additional_functionality & AF_ENFORA)
	{
		if ((type ==  GSM0710_TYPE_UIH || type ==  GSM0710_TYPE_UI) && 
			uih_pf_bit_received == 1 && 
			GSM0710_COMMAND_IS(input[0], GSM0710_CONTROL_MSC))
		{
			prefix[2] |= GSM0710_PF; //Set the P/F bit in Response if Command from modem had it set
			uih_pf_bit_received = 0; //Reset the variable, so it is ready for next command
			LOG(LOG_DEBUG, "uih_pf_bit_received is %d", uih_pf_bit_received );
		}
	}
//let's not use too big frames
	length = min(cmux_N1, length);
	if (!cmux_mode)
	{
//Modified acording PATCH CRC checksum
//postfix[0] = frame_calc_crc (prefix + 1, prefix_length - 1);
//length
		if (length > 127)
		{
			prefix_length = 5;
			prefix[3] = (0x007F & length) << 1;
			prefix[4] = (0x7F80 & length) >> 7;
		}
		else
			prefix[3] = 1 | (length << 1);
		postfix[0] = frame_calc_crc(prefix + 1, prefix_length - 1);
		c = write(serial.fd, prefix, prefix_length);
		if (c != prefix_length)
		{
			LOG(LOG_WARNING, "Couldn't write the whole prefix to the serial port for the virtual port %d. Wrote only %d bytes",
				channel, c);
			return 0;
		}
		if (length > 0)
		{
			c = write(serial.fd, input, length);
			if (length != c)
			{
				LOG(LOG_WARNING, "Couldn't write all data to the serial port from the virtual port %d. Wrote only %d bytes",
					channel, c);
				return 0;
			}
		}
		c = write(serial.fd, postfix, 2);
		if (c != 2)
		{
			LOG(LOG_WARNING, "Couldn't write the whole postfix to the serial port for the virtual port %d. Wrote only %d bytes",
				channel, c);
			return 0;
		}
	}
	else//cmux_mode
	{
		int offs = 1;
		serial.adv_frame_buf[0] = GSM0710_FRAME_ADV_FLAG;
		offs += fill_adv_frame_buf(serial.adv_frame_buf + offs, prefix + 1, 2);// address, control
		offs += fill_adv_frame_buf(serial.adv_frame_buf + offs, input, length);// data
//CRC checksum
		postfix[0] = frame_calc_crc(prefix + 1, 2);
		offs += fill_adv_frame_buf(serial.adv_frame_buf + offs, postfix, 1);// fcs
		serial.adv_frame_buf[offs] = GSM0710_FRAME_ADV_FLAG;
		offs++;
		syslogdump(">s ", (unsigned char *)serial.adv_frame_buf, offs);
		c = write(serial.fd, serial.adv_frame_buf, offs);
		if (c != offs)
		{
			LOG(LOG_WARNING, "Couldn't write the whole advanced option packet to the serial port for the virtual port %d. Wrote only %d bytes",
				channel, c);
			return 0;
		}
	}
	if (additional_functionality & AF_WAKEUP_WITH_SYSFS)
		modem_hw_(serial.pm_base_dir, "wake", 0);
	LOG(LOG_DEBUG, "Leave");
	return length;
}

/*
 * Handles received data from device. PARAMS: buf - buffer, which
 * contains received data len - the length of the buffer channel - the
 * number of devices (logical channel), where data was received
 * RETURNS: the number of remaining bytes in partial packet
 */
static int handle_channel_data(
	unsigned char *buf,
	int len,
	int channel)
{
	int written = 0;
	int i = 0;
	int last = 0;
//try to write 5 times
	while (written != len && i < GSM0710_WRITE_RETRIES)
	{
		last = write_frame(channel, buf + written, len - written, GSM0710_TYPE_UIH);
		written += last;
		if (last == 0)
			i++;
	}
	if (i == GSM0710_WRITE_RETRIES)
		LOG(LOG_WARNING, "Couldn't write data to channel %d. Wrote only %d bytes, when should have written %d",
				channel, written, len);
	return 0;
}

static int logical_channel_close(Channel* channel)
{
	LOG(LOG_DEBUG, "Enter");
	if (channel->g_source >= 0)
		g_source_remove(channel->g_source);
	channel->g_source = -1;
	if (channel->fd >= 0)
		close(channel->fd);
	channel->fd = -1;
	if (channel->ptsname != NULL)
		free(channel->ptsname);
	channel->ptsname = NULL;
	if (channel->tmp != NULL)
		free(channel->tmp);
	channel->tmp = NULL;
	if (channel->purpose != NULL)
		free(channel->purpose);
	channel->purpose = NULL;
	channel->opened = 0;
	channel->v24_signals = 0;
	channel->remaining = 0;
	LOG(LOG_DEBUG, "Leave");
	return 0;
}

static int logical_channel_init(Channel* channel, int id)
{
	channel->id = id; // connected channel-id
	channel->devicename = id?"/dev/ptmx":NULL; // TODO do we need this to be dynamic anymore?
	channel->fd = -1;
	channel->g_source = -1;
	channel->ptsname = NULL;
	channel->tmp = NULL;
	channel->purpose = NULL;
	return logical_channel_close(channel);
}

gboolean pseudo_device_read(GIOChannel *source, GIOCondition condition, gpointer data)
{
	LOG(LOG_DEBUG, "Enter");
	Channel* channel = (Channel*)data;
	if (condition == G_IO_IN)
	{
		unsigned char buf[4096];
		//information from virtual port
		int len = read(channel->fd, buf + channel->remaining, sizeof(buf) - channel->remaining);
		if (!channel->opened)
		{
			LOG(LOG_WARNING, "Write to a channel which wasn't acked to be open.");
			write_frame(channel->id, NULL, 0, GSM0710_TYPE_SABM | GSM0710_PF);
			LOG(LOG_DEBUG, "Leave");
			return TRUE;
		}
		if (len >= 0)
		{
			LOG(LOG_DEBUG, "Data from channel %d, %d bytes", channel->id, len);
			if (channel->remaining > 0)
			{
				memcpy(buf, channel->tmp, channel->remaining);
				free(channel->tmp);
				channel->tmp = NULL;
			}
			if (len + channel->remaining > 0)
				channel->remaining = handle_channel_data(buf, len + channel->remaining, channel->id);
			//copy remaining bytes from last packet into tmp
			if (channel->remaining > 0)
			{
				channel->tmp = malloc(channel->remaining);
				memcpy(channel->tmp, buf + sizeof(buf) - channel->remaining, channel->remaining);
			}
			LOG(LOG_DEBUG, "Leave");
			return TRUE;
		}
		// dropped connection
		if (cmux_mode)
			write_frame(channel->id, NULL, 0, GSM0710_CONTROL_CLD | GSM0710_CR);
		else
			write_frame(channel->id, close_channel_cmd, 2, GSM0710_TYPE_UIH);
		LOG(LOG_INFO, "Logical channel %d for %s closed", channel->id, channel->purpose);
		logical_channel_close(channel);
	}
	else if (condition == G_IO_HUP)
	{
		if (cmux_mode)
			write_frame(channel->id, NULL, 0, GSM0710_CONTROL_CLD | GSM0710_CR);
		else
			write_frame(channel->id, close_channel_cmd, 2, GSM0710_TYPE_UIH);
		LOG(LOG_INFO, "Logical channel %d for %s closed", channel->id, channel->purpose);
		logical_channel_close(channel);
	}
	LOG(LOG_DEBUG, "Leave");
	return FALSE;
}

static gboolean watchdog(gpointer data);
static int close_devices();

gboolean c_get_power(const char* purpose)
{
	LOG(LOG_DEBUG, "Enter");
	LOG(LOG_DEBUG, "Leave");
	return serial.state != MUX_STATE_OFF;
}

void c_set_power(const char* purpose, gboolean on)
{
	LOG(LOG_DEBUG, "Enter");
	if (on)
	{
		if (serial.state == MUX_STATE_OFF)
		{
			LOG(LOG_INFO, "power on");
			serial.state = MUX_STATE_OPENING;
			serial.g_source_watchdog = g_timeout_add_seconds(5, watchdog, &serial); // let the dog watch every 5 sec
		}
		else
			LOG(LOG_WARNING, "power on request received but was already on");
	}
	else
	{
		if (serial.state == MUX_STATE_MUXING)
		{
			LOG(LOG_INFO, "power off");
			close_devices();
		}
		else
			LOG(LOG_WARNING, "power off received but wasn't on/muxing");
	}
	LOG(LOG_DEBUG, "Leave");
}

const int c_alloc_channel(const char* purpose, char** url)
{
	LOG(LOG_DEBUG, "Enter");
	int i;
	if (serial.state == MUX_STATE_MUXING)
		for (i=1;i<GSM0710_MAX_CHANNELS;i++)
			if (channellist[i].fd < 0) // is this channel free?
			{
				LOG(LOG_DEBUG, "Found channel %d fd %d on %s", i, channellist[i].fd, channellist[i].devicename);
				channellist[i].purpose = strdup(purpose);
				SYSCHECK(channellist[i].fd = open(channellist[i].devicename, O_RDWR | O_NONBLOCK)); //open devices
				char* pts = ptsname(channellist[i].fd);
				if (pts == NULL) SYSCHECK(-1);
				channellist[i].ptsname = strdup(pts);
				struct termios options;
				tcgetattr(channellist[i].fd, &options); //get the parameters
				options.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG); //set raw input
				options.c_iflag &= ~(INLCR | ICRNL | IGNCR);
				options.c_oflag &= ~(OPOST| OLCUC| ONLRET| ONOCR| OCRNL); //set raw output
				tcsetattr(channellist[i].fd, TCSANOW, &options);
				if (!strcmp(channellist[i].devicename, "/dev/ptmx"))
				{
					//Otherwise programs cannot access the pseudo terminals
					SYSCHECK(grantpt(channellist[i].fd));
					SYSCHECK(unlockpt(channellist[i].fd));
				}
				channellist[i].v24_signals = GSM0710_SIGNAL_DV | GSM0710_SIGNAL_RTR | GSM0710_SIGNAL_RTC | GSM0710_EA;
				GIOChannel* g_channel = g_io_channel_unix_new(channellist[i].fd);
				channellist[i].g_source = g_io_add_watch(g_channel, G_IO_IN | G_IO_HUP, pseudo_device_read, channellist+i);
				write_frame(i, NULL, 0, GSM0710_TYPE_SABM | GSM0710_PF);
				LOG(LOG_INFO, "Connecting %s to virtual channel %d for %s on %s",
					channellist[i].ptsname, channellist[i].id, channellist[i].purpose, serial.devicename);
				*url = channellist[i].ptsname;
				return 0;
			}
	LOG(LOG_WARNING, "not muxing or no free channel found");
	errno = ENOBUFS;
	return -1;
}

void g_log_handler(
	const gchar *log_domain,
	GLogLevelFlags log_level,
	const gchar *message,
	gpointer user_data)
{
	LOG(LOG_INFO, "%d %s %s", log_level, log_domain, message);
}

/**
	GPtrArray *table = parse("(14-5),(3-7),(4-9),(33)");

	for(int i = 0; i < table->len; i++)
	{
		int *arr = g_ptr_array_index(table, i);
		printf("(%d, %d)\n", arr[0], arr[1]);
	}
*/
GPtrArray* parse(const gchar* str)
{
	GPtrArray *table = g_ptr_array_new();
	int i = -1;
	int first = -1;
	int pos = 0;
	while(str[++i] != 0)
	{
		if(str[i] == '(')
		{
			pos = i+1;
			first = -1;
		}
		else if(str[i] == ')')
		{
			int *tuple = malloc(sizeof(int)*2);
			
			tuple[1] = atoi(str + pos);
			tuple[0] = first == -1 ? tuple[1] : first;
			
			g_ptr_array_add(table, (gpointer)tuple);
		}
		else if(str[i] == '-')
		{
			first = atoi(str + pos);
			pos = i+1;
		}
	}
	return table;
}

//////////////////////////////////////////////// real functions
/* Allocates memory for a new buffer and initializes it.
 *
 * RETURNS:
 * the pointer to a new buufer
 */
static GSM0710_Buffer *gsm0710_buffer_init(
	)
{
	GSM0710_Buffer* buf = (GSM0710_Buffer*)malloc(sizeof(GSM0710_Buffer));
	if (buf)
	{
		memset(buf, 0, sizeof(GSM0710_Buffer));
		buf->readp = buf->data;
		buf->writep = buf->data;
		buf->endp = buf->data + GSM0710_BUFFER_SIZE;
	}
	return buf;
}

/* Destroys the buffer (i.e. frees up the memory
 *
 * PARAMS:
 * buf - buffer to be destroyed
 */
static void gsm0710_buffer_destroy(
	GSM0710_Buffer* buf)
{
	free(buf);
}

/* Writes data to the buffer
 *
 * PARAMS
 * buf - pointer to the buffer
 * input - input data (in user memory)
 * length - how many characters should be written
 * RETURNS
 * number of characters written
 */
static int gsm0710_buffer_write(
	GSM0710_Buffer* buf,
	const unsigned char *input,
	int length)
{
	LOG(LOG_DEBUG, "Enter");
	int c = buf->endp - buf->writep;
	length = min(length, gsm0710_buffer_free(buf));
	if (length > c)
	{
		memcpy(buf->writep, input, c);
		memcpy(buf->data, input + c, length - c);
		buf->writep = buf->data + (length - c);
	}
	else
	{
		memcpy(buf->writep, input, length);
		buf->writep += length;
		if (buf->writep == buf->endp)
			buf->writep = buf->data;
	}
	LOG(LOG_DEBUG, "Leave");
	return length;
}

/**
 * destroys a frame
 */
static void destroy_frame(
	GSM0710_Frame * frame)
{
	if (frame->length > 0)
		free(frame->data);
	free(frame);
}

/* Gets a frame from buffer. You have to remember to free this frame
 * when it's not needed anymore
 *
 * PARAMS:
 * buf - the buffer, where the frame is extracted
 * RETURNS:
 * frame or null, if there isn't ready frame with given index
 */
static GSM0710_Frame* gsm0710_base_buffer_get_frame(
	GSM0710_Buffer * buf)
{
	int end;
	int length_needed = 5;// channel, type, length, fcs, flag
	unsigned char *data;
	unsigned char fcs = 0xFF;
	GSM0710_Frame *frame = NULL;
//Find start flag
	while (!buf->flag_found && gsm0710_buffer_length(buf) > 0)
	{
		if (*buf->readp == GSM0710_FRAME_FLAG)
			buf->flag_found = 1;
		gsm0710_buffer_inc(buf, buf->readp);
	}
	if (!buf->flag_found)// no frame started
		return NULL;
//skip empty frames (this causes troubles if we're using DLC 62)
	while (gsm0710_buffer_length(buf) > 0 && (*buf->readp == GSM0710_FRAME_FLAG))
	{
		gsm0710_buffer_inc(buf, buf->readp);
	}
	if (gsm0710_buffer_length(buf) >= length_needed)
	{
		data = buf->readp;
		if ((frame = (GSM0710_Frame*)malloc(sizeof(GSM0710_Frame))) != NULL)
		{
			frame->channel = ((*data & 252) >> 2);
			fcs = r_crctable[fcs ^ *data];
			gsm0710_buffer_inc(buf, data);
			frame->control = *data;
			fcs = r_crctable[fcs ^ *data];
			gsm0710_buffer_inc(buf, data);
			frame->length = (*data & 254) >> 1;
			fcs = r_crctable[fcs ^ *data];
		}
		else
			LOG(LOG_ALERT, "Out of memory, when allocating space for frame");
		if ((*data & 1) == 0)
		{
//Current spec (version 7.1.0) states these kind of
//frames to be invalid Long lost of sync might be
//caused if we would expect a long frame because of an
//error in length field.
			/*
			gsm0710_buffer_inc(buf,data);
			frame->length += (*data*128);
			fcs = r_crctable[fcs^*data];
			length_needed++;
			*/
			free(frame);
			buf->readp = data;
			buf->flag_found = 0;
			return gsm0710_base_buffer_get_frame(buf);
		}
		length_needed += frame->length;
		if (!(gsm0710_buffer_length(buf) >= length_needed))
		{
			free(frame);
			return NULL;
		}
		gsm0710_buffer_inc(buf, data);
//extract data
		if (frame->length > 0)
		{
			if ((frame->data = malloc(sizeof(char) * frame->length)) != NULL)
			{
				end = buf->endp - data;
				if (frame->length > end)
				{
					memcpy(frame->data, data, end);
					memcpy(frame->data + end, buf->data, frame->length - end);
					data = buf->data + (frame->length - end);
				}
				else
				{
					memcpy(frame->data, data, frame->length);
					data += frame->length;
					if (data == buf->endp)
						data = buf->data;
				}
				if (GSM0710_FRAME_IS(GSM0710_TYPE_UI, frame))
				{
					for (end = 0; end < frame->length; end++)
						fcs = r_crctable[fcs ^ (frame->data[end])];
				}
			}
			else
			{
				LOG(LOG_ALERT, "Out of memory, when allocating space for frame data");
				frame->length = 0;
			}
		}
//check FCS
		if (r_crctable[fcs ^ (*data)] != 0xCF)
		{
			LOG(LOG_WARNING, "Dropping frame: FCS doesn't match");
			destroy_frame(frame);
			buf->flag_found = 0;
			buf->dropped_count++;
			buf->readp = data;
			return gsm0710_base_buffer_get_frame(buf);
		}
		else
		{
//check end flag
			gsm0710_buffer_inc(buf, data);
			if (*data != GSM0710_FRAME_FLAG)
			{
				LOG(LOG_WARNING, "Dropping frame: End flag not found. Instead: %d", *data);
				destroy_frame(frame);
				buf->flag_found = 0;
				buf->dropped_count++;
				buf->readp = data;
				return gsm0710_base_buffer_get_frame(buf);
			}
			else
				buf->received_count++;
			gsm0710_buffer_inc(buf, data);
		}
		buf->readp = data;
	}
	return frame;
}

/* Gets a advanced option frame from buffer. You have to remember to free this frame
 * when it's not needed anymore
 *
 * PARAMS:
 * buf - the buffer, where the frame is extracted
 * RETURNS:
 * frame or null, if there isn't ready frame with given index
 */
static GSM0710_Frame *gsm0710_advanced_buffer_get_frame(
	GSM0710_Buffer * buf)
{
	LOG(LOG_DEBUG, "Enter");
l_begin:
//Find start flag
	while (!buf->flag_found && gsm0710_buffer_length(buf) > 0)
	{
		if (*buf->readp == GSM0710_FRAME_ADV_FLAG)
		{
			buf->flag_found = 1;
			buf->adv_length = 0;
			buf->adv_found_esc = 0;
		}
		gsm0710_buffer_inc(buf, buf->readp);
	}
	if (!buf->flag_found)// no frame started
		return NULL;
	if (0 == buf->adv_length)
//skip empty frames (this causes troubles if we're using DLC 62)
		while (gsm0710_buffer_length(buf) > 0 && (*buf->readp == GSM0710_FRAME_ADV_FLAG))
			gsm0710_buffer_inc(buf, buf->readp);
	while (gsm0710_buffer_length(buf) > 0)
	{
		if (!buf->adv_found_esc && GSM0710_FRAME_ADV_FLAG == *(buf->readp))
		{// closing flag found
			GSM0710_Frame *frame = NULL;
			unsigned char *data = buf->adv_data;
			unsigned char fcs = 0xFF;
			gsm0710_buffer_inc(buf, buf->readp);
			if (buf->adv_length < 3)
			{
				LOG(LOG_WARNING, "Too short adv frame, length:%d", buf->adv_length);
				buf->flag_found = 0;
				goto l_begin;
			}
			if ((frame = (GSM0710_Frame*)malloc(sizeof(GSM0710_Frame))) != NULL)
			{
				frame->channel = ((data[0] & 252) >> 2);
				fcs = r_crctable[fcs ^ data[0]];
				frame->control = data[1];
				fcs = r_crctable[fcs ^ data[1]];
				frame->length = buf->adv_length - 3;
			}
			else
				LOG(LOG_ALERT, "Out of memory, when allocating space for frame");
//extract data
			if (frame->length > 0)
			{
				if ((frame->data = (unsigned char *) malloc(sizeof(char) * frame->length)))
				{
					memcpy(frame->data, data + 2, frame->length);
					if (GSM0710_FRAME_IS(GSM0710_TYPE_UI, frame))
					{
						int i;
						for (i = 0; i < frame->length; ++i)
							fcs = r_crctable[fcs ^ (frame->data[i])];
					}
				}
				else
				{
					LOG(LOG_ALERT, "Out of memory, when allocating space for frame data");
					buf->flag_found = 0;
					goto l_begin;
				}
			}
//check FCS
			if (r_crctable[fcs ^ data[buf->adv_length - 1]] != 0xCF)
			{
				LOG(LOG_WARNING, "Dropping frame: FCS doesn't match");
				destroy_frame(frame);
				buf->flag_found = 0;
				buf->dropped_count++;
				goto l_begin;
			}
			else
			{
				buf->received_count++;
				buf->flag_found = 0;
				LOG(LOG_DEBUG, "Leave success");
				return frame;
			}
		}
		if (buf->adv_length >= sizeof(buf->adv_data))
		{
			LOG(LOG_WARNING, "Too long adv frame, length:%d", buf->adv_length);
			buf->flag_found = 0;
			buf->dropped_count++;
			goto l_begin;
		}
		if (buf->adv_found_esc)
		{
			buf->adv_data[buf->adv_length] = *(buf->readp) ^ GSM0710_FRAME_ADV_ESC_COPML;
			buf->adv_length++;
			buf->adv_found_esc = 0;
		}
		else if (GSM0710_FRAME_ADV_ESC == *(buf->readp))
			buf->adv_found_esc = 1;
		else
		{
			buf->adv_data[buf->adv_length] = *(buf->readp);
			buf->adv_length++;
		}
		gsm0710_buffer_inc(buf, buf->readp);
	}
	return NULL;
}

/**
 * Returns 1 if found, 0 otherwise. needle must be null-terminated.
 * strstr might not work because WebBox sends garbage before the first
 * OK
 */
static int memstr(
	const char *haystack,
	int length,
	const char *needle)
{
	int i;
	int j = 0;
	if (needle[0] == '\0')
		return 1;
	for (i = 0; i < length; i++)
		if (needle[j] == haystack[i])
		{
			j++;
			if (needle[j] == '\0') // Entire needle was found
				return 1;
		}
		else
			j = 0;
	return 0;
}

/*
 * Sends an AT-command to a given serial port and waits for reply. 
 * PARAMS: fd - file descriptor cmd - command to - how many
 * seconds to wait for response RETURNS: 
 * 0 on success (OK-response), -1 otherwise
 */
static int chat(
	int serial_device_fd,
	char *cmd,
	int to)
{
	LOG(LOG_DEBUG, "Enter");
	unsigned char buf[1024];
	int sel;
	int len;
	int wrote = 0;
	syslogdump(">s ", (unsigned char *) cmd, strlen(cmd));
	SYSCHECK(wrote = write(serial_device_fd, WAKEUP, strlen(WAKEUP)));
	usleep(50);
	SYSCHECK(wrote = write(serial_device_fd, cmd, strlen(cmd)));
	LOG(LOG_DEBUG, "Wrote %d bytes", wrote);
//	SYSCHECK(tcdrain(serial_device_fd));

	fd_set rfds;
	FD_ZERO(&rfds);
	FD_SET(serial_device_fd, &rfds);
	struct timeval timeout;
	timeout.tv_sec = to;
	timeout.tv_usec = 0;
	do
	{
		SYSCHECK(sel = select(serial_device_fd + 1, &rfds, NULL, NULL, &timeout));
		LOG(LOG_DEBUG, "Selected %d", sel);
		if (FD_ISSET(serial_device_fd, &rfds))
		{
			memset(buf, 0, sizeof(buf));
			len = read(serial_device_fd, buf, sizeof(buf));
			SYSCHECK(len);
			LOG(LOG_DEBUG, "Read %d bytes from serial device", len);
			syslogdump("<s ", buf, len);
			errno = 0;
			if (memstr((char *) buf, len, "OK"))
			{
				LOG(LOG_DEBUG, "Received OK");
				return 0;
			}
			if (memstr((char *) buf, len, "ERROR"))
			{
				LOG(LOG_DEBUG, "Received ERROR");
				return -1;
			}
			if (memstr((char *) buf, len, "*MRDY: 1"))
			{
				LOG(LOG_DEBUG, "Received *MRDY: 1");
				return 0;
			}
		}
	} while (sel);
	errno = ETIMEDOUT;
	return -1;
}

/*
 * Handles commands received from the control channel.
 */
static int handle_command(
	GSM0710_Frame * frame)
{
	LOG(LOG_DEBUG, "Enter");
	unsigned char type, signals;
	int length = 0, i, type_length, channel, supported = 1;
	unsigned char *response;
//struct ussp_operation op;
	if (frame->length > 0)
	{
		type = frame->data[0];// only a byte long types are handled now skip extra bytes
		for (i = 0; (frame->length > i && (frame->data[i] & GSM0710_EA) == 0); i++);
		i++;
		type_length = i;
		if ((type & GSM0710_CR) == GSM0710_CR)
		{
//command not ack extract frame length
			while (frame->length > i)
			{
				length = (length * 128) + ((frame->data[i] & 254) >> 1);
				if ((frame->data[i] & 1) == 1)
					break;
				i++;
			}
			i++;
			switch ((type & ~GSM0710_CR))
			{
			case GSM0710_CONTROL_CLD:
				LOG(LOG_INFO, "The mobile station requested mux-mode termination");
				serial.state = MUX_STATE_CLOSING;				
				break;
			case GSM0710_CONTROL_PSC:
				LOG(LOG_DEBUG, "Power Service Control command: ***");
				LOG(LOG_DEBUG, "Frame->data = %s / frame->length = %d", frame->data + i, frame->length - i);
			break;
			case GSM0710_CONTROL_TEST:
				LOG(LOG_DEBUG, "Test command: ");
				LOG(LOG_DEBUG, "Frame->data = %s / frame->length = %d", frame->data + i, frame->length - i);
				//serial->ping_number = 0;
				break;
			case GSM0710_CONTROL_MSC:
				if (i + 1 < frame->length)
				{
					channel = ((frame->data[i] & 252) >> 2);
					i++;
					signals = (frame->data[i]);
//op.op = USSP_MSC;
//op.arg = USSP_RTS;
//op.len = 0;
					LOG(LOG_DEBUG, "Modem status command on channel %d", channel);
					if ((signals & GSM0710_SIGNAL_FC) == GSM0710_SIGNAL_FC)
						LOG(LOG_DEBUG, "No frames allowed");
					else
					{
//op.arg |= USSP_CTS;
						LOG(LOG_DEBUG, "Frames allowed");
					}
					if ((signals & GSM0710_SIGNAL_RTC) == GSM0710_SIGNAL_RTC)
					{
//op.arg |= USSP_DSR;
						LOG(LOG_DEBUG, "Signal RTC");
					}
					if ((signals & GSM0710_SIGNAL_IC) == GSM0710_SIGNAL_IC)
					{
//op.arg |= USSP_RI;
						LOG(LOG_DEBUG, "Signal Ring");
					}
					if ((signals & GSM0710_SIGNAL_DV) == GSM0710_SIGNAL_DV)
					{
//op.arg |= USSP_DCD;
						LOG(LOG_DEBUG, "Signal DV");
					}
//if (channel > 0)
//write(channellist[channel].fd, &op,
//sizeof(op));
				}
				else
					LOG(LOG_ERR, "Modem status command, but no info. i: %d, len: %d, data-len: %d",
						i, length, frame->length);
				break;
			default:
				LOG(LOG_ALERT, "Unknown command (%d) from the control channel", type);
				if ((response = malloc(sizeof(char) * (2 + type_length))) != NULL)
				{
					i = 0;
					response[i++] = GSM0710_CONTROL_NSC;
					type_length &= 127; //supposes that type length is less than 128
					response[i++] = GSM0710_EA | (type_length << 1);
					while (type_length--)
					{
						response[i] = frame->data[i - 2];
						i++;
					}
					write_frame(0, response, i, GSM0710_TYPE_UIH);
					free(response);
					supported = 0;
				}
				else
					LOG(LOG_ALERT, "Out of memory, when allocating space for response");
				break;
			}
			if (supported)
			{
//acknowledge the command
				frame->data[0] = frame->data[0] & ~GSM0710_CR;
				write_frame(0, frame->data, frame->length, GSM0710_TYPE_UIH);
				if (additional_functionality & AF_ENFORA)
				{
					switch ((type & ~GSM0710_CR)){
					case GSM0710_CONTROL_MSC:
						if (frame->control & GSM0710_PF){ //Check if the P/F var needs to be set again (cleared in write_frame)
						uih_pf_bit_received = 1;
						}
						LOG(LOG_DEBUG, "Sending 1st MSC command App->Modem");
						frame->data[0] = frame->data[0] | GSM0710_CR; //setting the C/R bit to "command"
						write_frame(0, frame->data, frame->length, GSM0710_TYPE_UIH);
						break;
					default:
						break;
					}
				}
			}
		}
		else
		{
//received ack for a command
			if (GSM0710_COMMAND_IS(type, GSM0710_CONTROL_NSC))
				LOG(LOG_ERR, "The mobile station didn't support the command sent");
			else
				LOG(LOG_DEBUG, "Command acknowledged by the mobile station");
		}
	}
	return 0;
}

/*
 * Extracts and handles frames from the receiver buffer. PARAMS: buf
 * - the receiver buffer
 */
int extract_frames(
	GSM0710_Buffer* buf)
{
	LOG(LOG_DEBUG, "Enter");
//version test for Siemens terminals to enable version 2 functions
	int frames_extracted = 0;
	GSM0710_Frame *frame;
	while ((frame = cmux_mode
		? gsm0710_advanced_buffer_get_frame(buf)
		: gsm0710_base_buffer_get_frame(buf)))
	{
		frames_extracted++;
		if ((GSM0710_FRAME_IS(GSM0710_TYPE_UI, frame) || GSM0710_FRAME_IS(GSM0710_TYPE_UIH, frame)))
		{
			LOG(LOG_DEBUG, "Frame is UI or UIH");
			if (additional_functionality & AF_ENFORA)
			{
				if (frame->control & GSM0710_PF){
				uih_pf_bit_received = 1;
				}
			}
			if (frame->channel > 0)
			{
				LOG(LOG_DEBUG, "Frame channel > 0, pseudo channel");
//data from logical channel
				write(channellist[frame->channel].fd, frame->data, frame->length);
			}
			else
			{
//control channel command
				LOG(LOG_DEBUG, "Frame channel == 0, control channel command");
				handle_command(frame);
			}
		}
		else
		{
//not an information frame
			LOG(LOG_DEBUG, "Not an information frame");
			switch ((frame->control & ~GSM0710_PF))
			{
			case GSM0710_TYPE_UA:
				LOG(LOG_DEBUG, "Frame is UA");
				if (channellist[frame->channel].opened)
				{
					LOG(LOG_INFO, "Logical channel %d for %s closed",
						frame->channel, channellist[frame->channel].purpose);
					SYSCHECK(logical_channel_close(channellist+frame->channel));
				}
				else
				{
					channellist[frame->channel].opened = 1;
					if (frame->channel == 0)
					{
						LOG(LOG_DEBUG, "Control channel opened");
						//send version Siemens version test
						//static unsigned char version_test[] = "\x23\x21\x04TEMUXVERSION2\0";
						//write_frame(0, version_test, sizeof(version_test), GSM0710_TYPE_UIH);
					}
					else
						LOG(LOG_INFO, "Logical channel %d opened", frame->channel);
				}
				break;
			case GSM0710_TYPE_DM:
				if (channellist[frame->channel].opened)
				{
					LOG(LOG_INFO, "DM received, so the channel %d for %s was already closed",
						frame->channel, channellist[frame->channel].purpose);
					SYSCHECK(logical_channel_close(channellist+frame->channel));
				}
				else
				{
					if (frame->channel == 0)
					{
						LOG(LOG_INFO, "Couldn't open control channel.\n->Terminating");
						serial.state = MUX_STATE_CLOSING;				
//close channels
					}
					else
						LOG(LOG_INFO, "Logical channel %d for %s couldn't be opened", frame->channel, channellist[frame->channel].purpose);
				}
				break;
			case GSM0710_TYPE_DISC:
				if (channellist[frame->channel].opened)
				{
					channellist[frame->channel].opened = 0;
					write_frame(frame->channel, NULL, 0, GSM0710_TYPE_UA | GSM0710_PF);
					if (frame->channel == 0)
					{
						serial.state = MUX_STATE_CLOSING;				
						LOG(LOG_INFO, "Control channel closed");
					}
					else
						LOG(LOG_INFO, "Logical channel %d for %s closed", frame->channel, channellist[frame->channel].purpose);
				}
				else
				{
//channel already closed
					LOG(LOG_WARNING, "Received DISC even though channel %d for %s was already closed",
							frame->channel, channellist[frame->channel].purpose);
					write_frame(frame->channel, NULL, 0, GSM0710_TYPE_DM | GSM0710_PF);
				}
				break;
			case GSM0710_TYPE_SABM:
//channel open request
				if (channellist[frame->channel].opened)
				{
					if (frame->channel == 0)
						LOG(LOG_INFO, "Control channel opened");
					else
						LOG(LOG_INFO, "Logical channel %d for %s opened",
							frame->channel, channellist[frame->channel].purpose);
				}
				else
//channel already opened
					LOG(LOG_WARNING, "Received SABM even though channel %d for %s was already closed",
						frame->channel, channellist[frame->channel].purpose);
				channellist[frame->channel].opened = 1;
				write_frame(frame->channel, NULL, 0, GSM0710_TYPE_UA | GSM0710_PF);
				break;
			}
		}
		destroy_frame(frame);
	}
	LOG(LOG_DEBUG, "Leave");
	return frames_extracted;
}

/**
 * Function responsible by all signal handlers treatment
 * any new signal must be added here
 */
void signal_treatment(
	int param)
{
	switch (param)
	{
	case SIGPIPE:
		exit(0);
	break;
	case SIGHUP:
		//reread the configuration files
	break;
	case SIGINT:
	case SIGTERM:
	case SIGUSR1:
		//g_main_loop_quit(main_loop);
		exit(0);
		//sig_term(param);
	break;
	case SIGKILL:
	default:
		exit(0);
	break;
	}
}

static int modem_hw_off(const char* pm_base_dir)
{
	LOG(LOG_DEBUG, "Enter");
	SYSCHECK(modem_hw_(pm_base_dir, "power_on", 0));
	modem_hw_(pm_base_dir, "reset", 0);
	LOG(LOG_DEBUG, "Leave");
	return 0;
}

static int modem_hw_on(const char* pm_base_dir)
{
	LOG(LOG_DEBUG, "Enter");
	SYSCHECK(modem_hw_off(pm_base_dir));
	sleep(1);
	SYSCHECK(modem_hw_(pm_base_dir, "power_on", 1));
	sleep(1);
	if (modem_hw_(pm_base_dir, "reset", 1) == 0)
	{
		sleep(1);
		SYSCHECK(modem_hw_(pm_base_dir, "reset", 0));
	}
	sleep(7);
	LOG(LOG_DEBUG, "Leave");
	return 0;
}

gboolean serial_device_read(GIOChannel *source, GIOCondition condition, gpointer data)
{
	Serial* serial = (Serial*)data;
	LOG(LOG_DEBUG, "Enter");
	if (condition == G_IO_IN)
	{
		switch (serial->state)
		{
		case MUX_STATE_MUXING:
		{
			unsigned char buf[4096];
			int len;
			//input from serial port
			LOG(LOG_DEBUG, "Serial Data");
			int length;
			if ((length = gsm0710_buffer_free(serial->in_buf)) > 0
			&& (len = read(serial->fd, buf, min(length, sizeof(buf)))) > 0)
			{
				syslogdump("<s ", buf, len);
				gsm0710_buffer_write(serial->in_buf, buf, len);
				//extract and handle ready frames
				if (extract_frames(serial->in_buf) > 0)
				{
					time(&serial->frame_receive_time); //get the current time
					serial->ping_number = 0;
				}
			}
			LOG(LOG_DEBUG, "Leave keep watching");
			return TRUE;
		}
		break;
		default:
			LOG(LOG_WARNING, "Don't know how to handle reading in state %d", serial->state);
		break;
		}
	}
	else if (condition == G_IO_HUP)
	{
		LOG(LOG_WARNING, "hup on serial file, closing");
		serial->state = MUX_STATE_CLOSING;				
	}
	LOG(LOG_DEBUG, "Leave stop watching");
	return FALSE;
}

int open_serial_device(
	Serial* serial
	)
{
	LOG(LOG_DEBUG, "Enter");
	SYSCHECK(modem_hw_on(serial->pm_base_dir));
	int i;
	for (i=0;i<GSM0710_MAX_CHANNELS;i++)
		SYSCHECK(logical_channel_init(channellist+i, i));
//open the serial port
	SYSCHECK(serial->fd = open(serial->devicename, O_RDWR | O_NOCTTY | O_NONBLOCK));
	LOG(LOG_INFO, "Opened serial port");
	int fdflags;
	SYSCHECK(fdflags = fcntl(serial->fd, F_GETFL));
	SYSCHECK(fcntl(serial->fd, F_SETFL, fdflags & ~O_NONBLOCK));
	struct termios t;
	tcgetattr(serial->fd, &t);
	t.c_cflag &= ~(CSIZE | CSTOPB | PARENB | PARODD);
	t.c_cflag |= CREAD | CLOCAL | CS8 | CRTSCTS;
	t.c_lflag &= ~(ICANON | ECHO | ECHOE | ECHOK | ECHONL | ISIG);
	t.c_iflag &= ~(INPCK | IGNPAR | PARMRK | ISTRIP | IXANY | ICRNL);
	t.c_iflag &= ~(IXON | IXOFF);
	t.c_oflag &= ~(OPOST | OCRNL);
	t.c_cc[VMIN] = 0;
	t.c_cc[VINTR] = _POSIX_VDISABLE;
	t.c_cc[VQUIT] = _POSIX_VDISABLE;
	t.c_cc[VSTART] = _POSIX_VDISABLE;
	t.c_cc[VSTOP] = _POSIX_VDISABLE;
	t.c_cc[VSUSP] = _POSIX_VDISABLE;
	speed_t speed = baud_bits[at_port_speed];
	cfsetispeed(&t, speed);
	cfsetospeed(&t, speed);
	SYSCHECK(tcsetattr(serial->fd, TCSANOW, &t));
	int status = TIOCM_DTR | TIOCM_RTS;
	ioctl(serial->fd, TIOCMBIS, &status);
	LOG(LOG_INFO, "Configured serial device");
	serial->ping_number = 0;
	time(&serial->frame_receive_time); //get the current time
	serial->state = MUX_STATE_INITILIZING;
	return 0;
}

int start_muxer(
	Serial* serial
	)
{
	LOG(LOG_INFO, "Configuring modem");
	char gsm_command[100];
	if (chat(serial->fd, "AT\r\n", 1) < 0)
	{
		LOG(LOG_WARNING, "Modem does not respond to AT commands, trying close mux mode");
		//if (cmux_mode) we do not know now so write both
			write_frame(0, NULL, 0, GSM0710_CONTROL_CLD | GSM0710_CR);
		//else
			write_frame(0, close_channel_cmd, 2, GSM0710_TYPE_UIH);
		SYSCHECK(chat(serial->fd, "AT\r\n", 1));
	}
	SYSCHECK(chat(serial->fd, "ATZ\r\n", 3));
	SYSCHECK(chat(serial->fd, "ATE0\r\n", 1));
	if (additional_functionality & AF_SIEMENS_C35)// additional siemens c35 init
	{
		SYSCHECK(snprintf(gsm_command, sizeof(gsm_command), "AT+IPR=%d\r\n", baud_rates[cmux_port_speed]));
		SYSCHECK(chat(serial->fd, gsm_command, 1));
		SYSCHECK(chat(serial->fd, "AT\r\n", 1));
		SYSCHECK(chat(serial->fd, "AT&S0\r\n", 1));
		SYSCHECK(chat(serial->fd, "AT\\Q3\r\n", 1));
	}
	if (pin_code >= 0)
	{
		LOG(LOG_DEBUG, "send pin %04d", pin_code);
//Some modems, such as webbox, will sometimes hang if SIM code
//is given in virtual channel
		SYSCHECK(snprintf(gsm_command, sizeof(gsm_command), "AT+CPIN=%04d\r\n", pin_code));
		SYSCHECK(chat(serial->fd, gsm_command, 10));
	}
	if (additional_functionality & AF_SHORT_INIT)
		SYSCHECK(snprintf(gsm_command, sizeof(gsm_command), "AT+CMUX=1\r\n"));
	else
	{
		LOG(LOG_INFO, "Switching modem on");
		SYSCHECK(chat(serial->fd, "AT+CFUN=0\r\n", 10));
		SYSCHECK(snprintf(gsm_command, sizeof(gsm_command), "AT+CMUX=%d,%d,%d,%d"
			//",%d,%d,%d,%d,%d"
			"\r\n"
			, cmux_mode
			, cmux_subset
			, cmux_port_speed
			, cmux_N1
			//, cmux_T1
			//, cmux_N2
			//, cmux_T2
			//, cmux_T3
			//, cmux_k
			));
	}
	LOG(LOG_INFO, "Starting mux mode");
	SYSCHECK(chat(serial->fd, gsm_command, 3));
	muxer_trigger(1);
	serial->state = MUX_STATE_MUXING;
	LOG(LOG_INFO, "Waiting for mux-mode");
	sleep(1);
	LOG(LOG_INFO, "Init control channel");
	write_frame(0, NULL, 0, GSM0710_TYPE_SABM | GSM0710_PF);
	GIOChannel* channel = g_io_channel_unix_new(serial->fd);
	serial->g_source = g_io_add_watch(channel, G_IO_IN | G_IO_HUP, serial_device_read, serial);
	return 0;
}

static int close_devices()
{
	LOG(LOG_DEBUG, "Enter");
	g_source_remove(serial.g_source_watchdog);
	g_source_remove(serial.g_source);
	serial.g_source = -1;
	int i;
	for (i=1;i<GSM0710_MAX_CHANNELS;i++)
	{
//terminate command given. Close channels one by one and finaly close
//the mux mode
		if (channellist[i].fd >= 0)
		{
			//SYSCHECK(dbus_signal_send_deactivate(channellist[i].ptsname)); // TODO
			if (channellist[i].opened)
			{
				LOG(LOG_INFO, "Closing down the logical channel %d", i);
				if (cmux_mode)
					write_frame(i, NULL, 0, GSM0710_CONTROL_CLD | GSM0710_CR);
//multiplexer close down command doesn't work with benqM22a module, use: write_frame(0, NULL, 0, GSM0710_TYPE_DISC | GSM0710_PF);
				else
					write_frame(i, close_channel_cmd, 2, GSM0710_TYPE_UIH);
				SYSCHECK(logical_channel_close(channellist+i));
			}
			LOG(LOG_INFO, "Logical channel %d closed", channellist[i].id);
		}
	}
	if (serial.fd >= 0)
	{
		if (cmux_mode)
			write_frame(0, NULL, 0, GSM0710_CONTROL_CLD | GSM0710_CR);
		else
			write_frame(0, close_channel_cmd, 2, GSM0710_TYPE_UIH);
		static const char* poff = "AT@POFF\r\n";
		syslogdump(">s ", (unsigned char *)poff, strlen(poff));
		write(serial.fd, poff, strlen(poff));
		SYSCHECK(close(serial.fd));
		serial.fd = -1;
	}
	SYSCHECK(modem_hw_off(serial.pm_base_dir));
	muxer_trigger(0);
	serial.state = MUX_STATE_OFF;
	return 0;
}

static gboolean watchdog(gpointer data)
{
	LOG(LOG_DEBUG, "Enter");
	Serial* serial = (Serial*)data;
	LOG(LOG_DEBUG, "Serial state is %d", serial->state);
	switch (serial->state)
	{
	case MUX_STATE_OPENING:
		if (open_serial_device(serial) < 0)
			LOG(LOG_WARNING, "Could not open all devices and start muxer");
		LOG(LOG_INFO, "Watchdog started");
	case MUX_STATE_INITILIZING:
		if (start_muxer(serial) < 0)
			LOG(LOG_WARNING, "Could not open all devices and start muxer errno=%d", errno);
	break;
	case MUX_STATE_MUXING:
		if (use_ping)
		{
			if (serial->ping_number > use_ping)
			{
				LOG(LOG_DEBUG, "no ping reply for %d times, resetting modem", serial->ping_number);
				serial->state = MUX_STATE_CLOSING;
			}
			else
			{
				LOG(LOG_DEBUG, "Sending PING to the modem");
				//write_frame(0, psc_channel_cmd, sizeof(psc_channel_cmd), GSM0710_TYPE_UI);
				write_frame(0, test_channel_cmd, sizeof(test_channel_cmd), GSM0710_TYPE_UI);
				serial->ping_number++;
			}
		}
		if (use_timeout)
		{
			time_t current_time;
			time(&current_time); //get the current time
			if (current_time - serial->frame_receive_time > use_timeout)
			{
				LOG(LOG_DEBUG, "timeout, resetting modem");
				serial->state = MUX_STATE_CLOSING;
			}
		}
	break;
	case MUX_STATE_CLOSING:
		close_devices();
		serial->state = MUX_STATE_OPENING;
	break;
	default:
		LOG(LOG_WARNING, "Don't know how to handle state %d", serial->state);
	break;
	}
	return 1;
}

/**
 * shows how to use this program
 */
static int usage(
	char *_name)
{
	fprintf(stderr, "\tUsage: %s [options]\n", _name);
	fprintf(stderr, "Options:\n");
	// process control
	fprintf(stderr, "\t-d:\tFork, get a daemon [%s]\n", no_daemon?"no":"yes");
	fprintf(stderr, "\t-v:\tverboser logging\n");
	// modem control
	fprintf(stderr, "\t-s <serial port name>:\tSerial port device to connect to [%s]\n", serial.devicename);
	fprintf(stderr, "\t-t <timeout>:\treset modem after this number of seconds of silence [%d]\n", use_timeout);
	fprintf(stderr, "\t-P <pin-code>:\tPIN code to unlock SIM [%d]\n", pin_code);
	fprintf(stderr, "\t-p <number>:\tuse ping and reset modem after this number of unanswered pings [%d]\n", use_ping);
	fprintf(stderr, "\t-l <filename>:\tset logfile name [<not set>]\n");
	fprintf(stderr, "\t-x <dir>:\tpower managment base dir [%s]\n", serial.pm_base_dir?serial.pm_base_dir:"<not set>");
	fprintf(stderr, "\t-a <mode>:\tadditional_functionality [%d]\n", additional_functionality);
	// legacy - will be removed
	fprintf(stderr, "\t-b <baudrate>:\tmuxer-mode baudrate [%d]\n", baud_rates[cmux_port_speed]);
	fprintf(stderr, "\t-B <baudrate>:\tserial port baudrate [%d]\n", baud_rates[at_port_speed]);
	fprintf(stderr, "\t-m <modem>:\tMode (basic, advanced) [%s]\n", cmux_mode?"advanced":"basic");
	fprintf(stderr, "\t-f <framsize>:\tFrame size [%d]\n", cmux_N1);
	//
	fprintf(stderr, "\t-h:\tShow this help message and show current settings.\n");
	return -1;
}

/**
 * The main program
 */
int main(
	int argc,
	char *argv[],
	char *env[])
{
	LOG(LOG_DEBUG, "Enter");
	int opt;
	pid_t parent_pid;
//for fault tolerance
	serial.devicename = "/dev/modem";
	while ((opt = getopt(argc, argv, "dvs:t:p:f:h?m:b:B:P:a:x:")) > 0)
	{
		switch (opt)
		{
		case 'v':
			syslog_level++;
			break;
		case 'd':
			no_daemon = !no_daemon;
			break;
		case 'x':
			serial.pm_base_dir = optarg;
			break;
		case 'l':
			mylogfile = fopen(optarg, "w+");
			if (!mylogfile) SYSCHECK(-1);
			break;
		case 's':
			serial.devicename = optarg;
			break;
		case 't':
			use_timeout = atoi(optarg);
			break;
		case 'p':
			use_ping = atoi(optarg);
			break;
		case 'P':
			pin_code = atoi(optarg);
			break;
		// will be removed if +CMUX? works
		case 'f':
			cmux_N1 = atoi(optarg);
			break;
		case 'm':
			if (!strcmp(optarg, "basic"))
				cmux_mode = 0;
			else if (!strcmp(optarg, "advanced"))
				cmux_mode = 1;
			else
				cmux_mode = 0;
			break;
		case 'a':
			additional_functionality = atoi(optarg);
			break;
		case 'b':
			cmux_port_speed = baud_rate_index(atoi(optarg));
			break;
		case 'B':
			at_port_speed = baud_rate_index(atoi(optarg));
			break;
		default:
		case '?':
		case 'h':
			usage(argv[0]);
			exit(0);
			break;
		}
	}
//daemonize show time
	parent_pid = getpid();
	if (!no_daemon && daemon(0, 0))
	{
		fprintf(stderr, "Failed to daemonize: %s (%d)", strerror(errno), errno);
		exit(1);
	}
	umask(0);
//signals treatment
	signal(SIGHUP, signal_treatment);
	signal(SIGPIPE, signal_treatment);
	signal(SIGKILL, signal_treatment);
	signal(SIGINT, signal_treatment);
	signal(SIGUSR1, signal_treatment);
	signal(SIGTERM, signal_treatment);
	if (no_daemon)
		openlog(argv[0], LOG_NDELAY | LOG_PID | LOG_PERROR, LOG_LOCAL0);
	else
		openlog(argv[0], LOG_NDELAY | LOG_PID, LOG_LOCAL0);
//allocate memory for data structures
	if ((serial.in_buf = gsm0710_buffer_init()) == NULL
	 || (serial.adv_frame_buf = (unsigned char*)malloc((cmux_N1 + 3) * 2 + 2)) == NULL)
	{
		LOG(LOG_ALERT, "Out of memory");
		exit(-1);
	}
	LOG(LOG_DEBUG, "%s starting", *argv);
//Initialize modem and virtual ports
	serial.state = MUX_STATE_OFF;
//start waiting for input and forwarding it back and forth --
	g_type_init();
	SYSCHECK(muxer_run());
//finalize everything
	SYSCHECK(close_devices());
	free(serial.adv_frame_buf);
	gsm0710_buffer_destroy(serial.in_buf);
	LOG(LOG_INFO, "Received %ld frames and dropped %ld received frames during the mux-mode",
		serial.in_buf->received_count, serial.in_buf->dropped_count);
	LOG(LOG_DEBUG, "%s finished", argv[0]);
	closelog();// close syslog
	return 0;
}
// vim:path=/usr/include,/usr/include/glib-2.0,/usr/include/dbus-1.0,src:
