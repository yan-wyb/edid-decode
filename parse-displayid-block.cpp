// SPDX-License-Identifier: MIT
/*
 * Copyright 2006-2012 Red Hat, Inc.
 * Copyright 2018-2019 Cisco Systems, Inc. and/or its affiliates. All rights reserved.
 *
 * Author: Adam Jackson <ajax@nwnk.net>
 * Maintainer: Hans Verkuil <hverkuil-cisco@xs4all.nl>
 */

#include <string.h>
#include <math.h>

#include "edid-decode.h"

static const char *bpc444[] = {"6", "8", "10", "12", "14", "16", NULL, NULL};
static const char *bpc4xx[] = {"8", "10", "12", "14", "16", NULL, NULL, NULL};
static const char *audiorates[] = {"32", "44.1", "48", NULL, NULL, NULL, NULL, NULL};

// misc functions

static void print_flags(const char *label, unsigned char flag_byte,
			const char **flags, bool reverse = false)
{
	if (!flag_byte)
		return;

	unsigned countflags = 0;

	printf("%s: ", label);
	for (unsigned i = 0; i < 8; i++) {
		if (flag_byte & (1 << (reverse ? 7 - i : i))) {
			if (countflags)
				printf(", ");
			if (flags[i])
				printf("%s", flags[i]);
			else
				printf("Undefined (%u)", i);
			countflags++;
		}
	}
	printf("\n");
}

static void check_displayid_datablock_revision(const unsigned char *x)
{
	unsigned char revisionflags = x[1];

	if (revisionflags)
		warn("Unexpected revision and flags (0x%02x != 0)\n", revisionflags);
}

static bool check_displayid_datablock_length(const unsigned char *x,
					     unsigned expectedlenmin = 0,
					     unsigned expectedlenmax = 128 - 2 - 5 - 3,
					     unsigned payloaddumpstart = 0)
{
	unsigned char len = x[2];

	if (expectedlenmin == expectedlenmax && len != expectedlenmax)
		fail("DisplayID payload length is different than expected (%d != %d)\n", len, expectedlenmax);
	else if (len > expectedlenmax)
		fail("DisplayID payload length is greater than expected (%d > %d)\n", len, expectedlenmax);
	else if (len < expectedlenmin)
		fail("DisplayID payload length is less than expected (%d < %d)\n", len, expectedlenmin);
	else
		return true;

	if (len > payloaddumpstart)
		hex_block("    ", x + 3 + payloaddumpstart, len - payloaddumpstart);
	return false;
}

// tag 0x00 and 0x20

static void parse_displayid_product_id(const unsigned char *x, bool is_v2)
{
	check_displayid_datablock_revision(x);

	if (is_v2)
		printf("    Vendor IEEE OUI: %02X-%02X-%02X\n", x[3], x[4], x[5]);
	else
		printf("    Vendor ID: %c%c%c\n", x[3], x[4], x[5]);
	printf("    Product Code: %u\n", x[6] | (x[7] << 8));
	unsigned sn = x[8] | (x[9] << 8) | (x[10] << 16) | (x[11] << 24);
	if (sn)
		printf("    Serial Number: %u\n", sn);
	unsigned week = x[12];
	unsigned year = 2000 + x[13];
	printf("    %s: %u",
	       week == 0xff ? "Model Year" : "Year of Manufacture", year);
	if (week && week <= 0x36)
		printf(", Week %u", week);
	printf("\n");
	if (x[14]) {
		char buf[256];

		memcpy(buf, x + 15, x[14]);
		buf[x[14]] = 0;
		printf("    Product ID: %s\n", buf);
	}
}

// tag 0x01

static const char *feature_support_flags[] = {
	"De-interlacing",
	"Support ACP, ISRC1, or ISRC2packets",
	"Fixed pixel format",
	"Fixed timing",
	"Power management (DPM)",
	"Audio input override",
	"Separate audio inputs provided",
	"Audio support on video interface"
};

static void print_flag_lines(const char *indent, const char *label,
			     unsigned char flag_byte, const char **flags)
{
	if (flag_byte) {
		printf("%s\n", label);

		for (int i = 0; i < 8; i++)
			if (flag_byte & (1 << i))
				printf("%s%s\n", indent, flags[i]);
	}
}

static void parse_displayid_parameters(const unsigned char *x)
{
	check_displayid_datablock_revision(x);

	if (!check_displayid_datablock_length(x, 12, 12))
		return;

	printf("    Image size: %.1f mm x %.1f mm\n",
	       ((x[4] << 8) + x[3]) / 10.0,
	       ((x[6] << 8) + x[5]) / 10.0);
	printf("    Pixels: %d x %d\n",
	       (x[8] << 8) + x[7], (x[10] << 8) + x[9]);
	print_flag_lines("      ", "    Feature support flags:",
			 x[11], feature_support_flags);

	if (x[12] != 0xff)
		printf("    Gamma: %.2f\n", ((x[12] + 100.0) / 100.0));
	printf("    Aspect ratio: %.2f\n", ((x[13] + 100.0) / 100.0));
	printf("    Dynamic bpc native: %d\n", (x[14] & 0xf) + 1);
	printf("    Dynamic bpc overall: %d\n", ((x[14] >> 4) & 0xf) + 1);
}

// tag 0x02

static const char *std_colorspace_ids[] = {
	"sRGB",
	"BT.601",
	"BT.709",
	"Adobe RGB",
	"DCI-P3",
	"NTSC",
	"EBU",
	"Adobe Wide Gamut RGB",
	"DICOM"
};

static double fp2d(unsigned short fp)
{
	return fp / 4096.0;
}

void edid_state::parse_displayid_color_characteristics(const unsigned char *x)
{
	check_displayid_datablock_revision(x);

	unsigned cie_year = (x[1] & 0x80) ? 1976 : 1931;
	unsigned xfer_id = (x[1] >> 3) & 0x0f;
	unsigned num_whitepoints = x[3] & 0x0f;
	unsigned num_primaries = (x[3] >> 4) & 0x07;
	bool temporal_color = x[3] & 0x80;
	unsigned offset = 4;

	printf("    Uses %s color\n", temporal_color ? "temporal" : "spatial");
	printf("    Uses %u CIE (x, y) coordinates\n", cie_year);
	if (xfer_id) {
		printf("    Associated with Transfer Characteristics Data Block with Identifier %u\n", xfer_id);
		if (!(preparse_xfer_ids & (1 << xfer_id)))
			fail("Missing Transfer Characteristics Data Block with Identifier %u\n", xfer_id);
	}
	if (!num_primaries) {
		printf("    Uses color space %s\n",
		       x[4] >= ARRAY_SIZE(std_colorspace_ids) ? "Reserved" :
								std_colorspace_ids[x[4]]);
		offset++;
	}
	for (unsigned i = 0; i < num_primaries; i++) {
		unsigned idx = offset + 3 * i;

		printf("    Primary #%u: (%.6f, %.6f)\n", i,
		       fp2d(x[idx] | ((x[idx + 1] & 0x0f) << 8)),
		       fp2d(((x[idx + 1] & 0xf0) >> 4) | (x[idx + 2] << 4)));
	}
	offset += 3 * num_primaries;
	for (unsigned i = 0; i < num_whitepoints; i++) {
		unsigned idx = offset + 3 * i;

		printf("    White point #%u: (%.6f, %.6f)\n", i,
		       fp2d(x[idx] | ((x[idx + 1] & 0x0f) << 8)),
		       fp2d(((x[idx + 1] & 0xf0) >> 4) | (x[idx + 2] << 4)));
	}
}

// tag 0x03 and 0x22

void edid_state::parse_displayid_type_1_7_timing(const unsigned char *x, bool type7)
{
	struct timings t = {};
	unsigned hbl, vbl;
	std::string s("aspect ");

	t.pixclk_khz = (type7 ? 1 : 10) * (1 + (x[0] + (x[1] << 8) + (x[2] << 16)));
	switch (x[3] & 0xf) {
	case 0:
		s += "1:1";
		t.hratio = t.vratio = 1;
		break;
	case 1:
		s += "5:4";
		t.hratio = 5;
		t.vratio = 4;
		break;
	case 2:
		s += "4:3";
		t.hratio = 4;
		t.vratio = 3;
		break;
	case 3:
		s += "15:9";
		t.hratio = 15;
		t.vratio = 9;
		break;
	case 4:
		s += "16:9";
		t.hratio = 16;
		t.vratio = 9;
		break;
	case 5:
		s += "16:10";
		t.hratio = 16;
		t.vratio = 10;
		break;
	case 6:
		s += "64:27";
		t.hratio = 64;
		t.vratio = 27;
		break;
	case 7:
		s += "256:135";
		t.hratio = 256;
		t.vratio = 135;
		break;
	default:
		s += "undefined";
		fail("Unknown aspect 0x%02x\n", x[3] & 0xf);
		break;
	}
	switch ((x[3] >> 5) & 0x3) {
	case 0:
		s += ", no 3D stereo";
		break;
	case 1:
		s += ", 3D stereo";
		break;
	case 2:
		s += ", 3D stereo depends on user action";
		break;
	case 3:
		s += ", reserved";
		fail("Reserved stereo 0x03\n");
		break;
	}

	t.hact = 1 + (x[4] | (x[5] << 8));
	hbl = 1 + (x[6] | (x[7] << 8));
	t.hfp = 1 + (x[8] | ((x[9] & 0x7f) << 8));
	t.hsync = 1 + (x[10] | (x[11] << 8));
	t.hbp = hbl - t.hfp - t.hsync;
	if ((x[9] >> 7) & 0x1)
		t.pos_pol_hsync = true;
	t.vact = 1 + (x[12] | (x[13] << 8));
	vbl = 1 + (x[14] | (x[15] << 8));
	t.vfp = 1 + (x[16] | ((x[17] & 0x7f) << 8));
	t.vsync = 1 + (x[18] | (x[19] << 8));
	t.vbp = vbl - t.vfp - t.vsync;
	if ((x[17] >> 7) & 0x1)
		t.pos_pol_vsync = true;

	if (x[3] & 0x10) {
		t.interlaced = true;
		t.vfp /= 2;
		t.vsync /= 2;
		t.vbp /= 2;
	}
	if (x[3] & 0x80)
		s += ", preferred";

	print_detailed_timings("    ", t, s.c_str());
}

// tag 0x04

void edid_state::parse_displayid_type_2_timing(const unsigned char *x)
{
	struct timings t = {};
	unsigned hbl, vbl;
	std::string s("aspect ");

	t.pixclk_khz = 10 * (1 + (x[0] + (x[1] << 8) + (x[2] << 16)));
	t.hact = 8 + 8 * (x[4] | ((x[5] & 0x01) << 8));
	hbl = 8 + 8 * ((x[5] & 0xfe) >> 1);
	t.hfp = 8 + 8 * ((x[6] & 0xf0) >> 4);
	t.hsync = 8 + 8 * (x[6] & 0xf);
	t.hbp = hbl - t.hfp - t.hsync;
	if ((x[3] >> 3) & 0x1)
		t.pos_pol_hsync = true;
	t.vact = 1 + (x[7] | ((x[8] & 0xf) << 8));
	vbl = 1 + x[9];
	t.vfp = 1 + (x[10] >> 4);
	t.vsync = 1 + (x[10] & 0xf);
	t.vbp = vbl - t.vfp - t.vsync;
	if ((x[17] >> 2) & 0x1)
		t.pos_pol_vsync = true;

	if (x[3] & 0x10) {
		t.interlaced = true;
		t.vfp /= 2;
		t.vsync /= 2;
		t.vbp /= 2;
	}

	calc_ratio(&t);

	s += std::to_string(t.hratio) + ":" + std::to_string(t.vratio);

	switch ((x[3] >> 5) & 0x3) {
	case 0:
		s += ", no 3D stereo";
		break;
	case 1:
		s += ", 3D stereo";
		break;
	case 2:
		s += ", 3D stereo depends on user action";
		break;
	case 3:
		s += ", reserved";
		fail("Reserved stereo 0x03\n");
		break;
	}
	if (x[3] & 0x80)
		s += ", preferred";

	print_detailed_timings("    ", t, s.c_str());
}

// tag 0x05

void edid_state::parse_displayid_type_3_timing(const unsigned char *x)
{
	struct timings t = {};
	std::string s("aspect ");

	switch (x[0] & 0xf) {
	case 0:
		s += "1:1";
		t.hratio = t.vratio = 1;
		break;
	case 1:
		s += "5:4";
		t.hratio = 5;
		t.vratio = 4;
		break;
	case 2:
		s += "4:3";
		t.hratio = 4;
		t.vratio = 3;
		break;
	case 3:
		s += "15:9";
		t.hratio = 15;
		t.vratio = 9;
		break;
	case 4:
		s += "16:9";
		t.hratio = 16;
		t.vratio = 9;
		break;
	case 5:
		s += "16:10";
		t.hratio = 16;
		t.vratio = 10;
		break;
	case 6:
		s += "64:27";
		t.hratio = 64;
		t.vratio = 27;
		break;
	case 7:
		s += "256:135";
		t.hratio = 256;
		t.vratio = 135;
		break;
	default:
		s += "undefined";
		fail("Unknown aspect 0x%02x\n", x[3] & 0xf);
		break;
	}

	t.rb = ((x[0] & 0x70) >> 4) == 1;
	t.hact = 8 + 8 * x[1];
	t.vact = t.hact * t.vratio / t.hratio;

	edid_cvt_mode(1 + (x[2] & 0x7f), t);

	if (x[0] & 0x80)
		s += ", preferred";

	print_detailed_timings("    ", t, s.c_str());
}

// tag 0x06 and 0x23

void edid_state::parse_displayid_type_4_8_timing(unsigned char type, unsigned short id)
{
	const struct timings *t = NULL;
	char suffix[16];

	switch (type) {
	case 0: t = find_dmt_id(id); strcpy(suffix, "DMT"); break;
	case 1: t = find_vic_id(id); sprintf(suffix, "VIC %3u", id); break;
	case 2: t = find_hdmi_vic_id(id); sprintf(suffix, "HDMI VIC %u", id); break;
	default: break;
	}
	if (t)
		print_timings("    ", t, suffix);
}

// tag 0x09

static void parse_displayid_video_timing_range_limits(const unsigned char *x)
{
	check_displayid_datablock_revision(x);

	if (!check_displayid_datablock_length(x, 15, 15))
		return;
	printf("    Pixel Clock: %.3f-%.3f MHz\n",
	       (double)(x[3] | (x[4] << 8) | (x[5] << 16)) / 100.0,
	       (double)(x[6] | (x[7] << 8) | (x[8] << 16)) / 100.0);
	printf("    Horizontal Frequency: %u-%u kHz\n", x[9], x[10]);
	printf("    Minimum Horizontal Blanking: %u pixels\n", x[11] | (x[12] << 8));
	printf("    Vertical Refresh: %u-%u Hz\n", x[13], x[14]);
	printf("    Minimum Vertical Blanking: %u lines\n", x[15] | (x[16] << 8));
	if (x[17] & 0x80)
		printf("    Supports Interlaced\n");
	if (x[17] & 0x40)
		printf("    Supports CVT\n");
	if (x[17] & 0x20)
		printf("    Supports CVT Reduced Blanking\n");
	if (x[17] & 0x10)
		printf("    Discrete frequency display device\n");
}

// tag 0x0a and 0x0b

static void parse_displayid_string(const unsigned char *x)
{
	check_displayid_datablock_revision(x);
	if (check_displayid_datablock_length(x))
		printf("    %s\n", extract_string(x + 3, x[2]));
}

// tag 0x0c

static void parse_displayid_display_device(const unsigned char *x)
{
	check_displayid_datablock_revision(x);

	if (!check_displayid_datablock_length(x, 13, 13))
		return;

	printf("    Display Device Technology: ");
	switch (x[3]) {
	case 0x00: printf("Monochrome CRT\n"); break;
	case 0x01: printf("Standard tricolor CRT\n"); break;
	case 0x02: printf("Other/undefined CRT\n"); break;
	case 0x10: printf("Passive matrix TN\n"); break;
	case 0x11: printf("Passive matrix cholesteric LC\n"); break;
	case 0x12: printf("Passive matrix ferroelectric LC\n"); break;
	case 0x13: printf("Other passive matrix LC type\n"); break;
	case 0x14: printf("Active-matrix TN\n"); break;
	case 0x15: printf("Active-matrix IPS (all types)\n"); break;
	case 0x16: printf("Active-matrix VA (all types)\n"); break;
	case 0x17: printf("Active-matrix OCB\n"); break;
	case 0x18: printf("Active-matrix ferroelectric\n"); break;
	case 0x1f: printf("Other LC type\n"); break;
	case 0x20: printf("DC plasma\n"); break;
	case 0x21: printf("AC plasma\n"); break;
	}
	switch (x[3] & 0xf0) {
	case 0x30: printf("Electroluminescent, except OEL/OLED\n"); break;
	case 0x40: printf("Inorganic LED\n"); break;
	case 0x50: printf("Organic LED/OEL\n"); break;
	case 0x60: printf("FED or sim. \"cold-cathode,\" phosphor-based types\n"); break;
	case 0x70: printf("Electrophoretic\n"); break;
	case 0x80: printf("Electrochromic\n"); break;
	case 0x90: printf("Electromechanical\n"); break;
	case 0xa0: printf("Electrowetting\n"); break;
	case 0xf0: printf("Other type not defined here\n"); break;
	}
	printf("    Display operating mode: ");
	switch (x[4] >> 4) {
	case 0x00: printf("Direct-view reflective, ambient light\n"); break;
	case 0x01: printf("Direct-view reflective, ambient light, also has light source\n"); break;
	case 0x02: printf("Direct-view reflective, uses light source\n"); break;
	case 0x03: printf("Direct-view transmissive, ambient light\n"); break;
	case 0x04: printf("Direct-view transmissive, ambient light, also has light source\n"); break;
	case 0x05: printf("Direct-view transmissive, uses light source\n"); break;
	case 0x06: printf("Direct-view emissive\n"); break;
	case 0x07: printf("Direct-view transflective, backlight off by default\n"); break;
	case 0x08: printf("Direct-view transflective, backlight on by default\n"); break;
	case 0x09: printf("Transparent display, ambient light\n"); break;
	case 0x0a: printf("Transparent emissive display\n"); break;
	case 0x0b: printf("Projection device using reflective light modulator\n"); break;
	case 0x0c: printf("Projection device using transmissive light modulator\n"); break;
	case 0x0d: printf("Projection device using emissive image transducer\n"); break;
	default: printf("Reserved\n"); break;
	}
	if (x[4] & 0x08)
		printf("    The backlight may be switched on and off\n");
	if (x[4] & 0x04)
		printf("    The backlight's intensity can be controlled\n");
	printf("    Display native pixel format: %ux%u\n",
	       x[5] | (x[6] << 8), x[7] | (x[8] << 8));
	printf("    Aspect ratio and orientation:\n");
	printf("      Aspect Ratio: %.2f\n", (100 + x[9]) / 100.0);
	unsigned char v = x[0x0a];
	printf("      Default Orientation: ");
	switch ((v & 0xc0) >> 6) {
	case 0x00: printf("Landscape\n"); break;
	case 0x01: printf("Portrait\n"); break;
	case 0x02: printf("Not Fixed\n"); break;
	case 0x03: printf("Undefined\n"); break;
	}
	printf("      Rotation Capability: ");
	switch ((v & 0x30) >> 4) {
	case 0x00: printf("None\n"); break;
	case 0x01: printf("Can rotate 90 degrees clockwise\n"); break;
	case 0x02: printf("Can rotate 90 degrees counterclockwise\n"); break;
	case 0x03: printf("Can rotate 90 degrees in either direction)\n"); break;
	}
	printf("      Zero Pixel Location: ");
	switch ((v & 0x0c) >> 2) {
	case 0x00: printf("Upper Left\n"); break;
	case 0x01: printf("Upper Right\n"); break;
	case 0x02: printf("Lower Left\n"); break;
	case 0x03: printf("Lower Right\n"); break;
	}
	printf("      Scan Direction: ");
	switch (v & 0x03) {
	case 0x00: printf("Not defined\n"); break;
	case 0x01: printf("Fast Scan is on the Major (Long) Axis and Slow Scan is on the Minor Axis\n"); break;
	case 0x02: printf("Fast Scan is on the Minor (Short) Axis and Slow Scan is on the Major Axis\n"); break;
	case 0x03: printf("Reserved\n");
		   fail("Scan Direction used the reserved value 0x03\n");
		   break;
	}
	printf("    Sub-pixel layout/configuration/shape: ");
	switch (x[0x0b]) {
	case 0x00: printf("Not defined\n"); break;
	case 0x01: printf("RGB vertical stripes\n"); break;
	case 0x02: printf("RGB horizontal stripes\n"); break;
	case 0x03: printf("Vertical stripes using primary order\n"); break;
	case 0x04: printf("Horizontal stripes using primary order\n"); break;
	case 0x05: printf("Quad sub-pixels, red at top left\n"); break;
	case 0x06: printf("Quad sub-pixels, red at bottom left\n"); break;
	case 0x07: printf("Delta (triad) RGB sub-pixels\n"); break;
	case 0x08: printf("Mosaic\n"); break;
	case 0x09: printf("Quad sub-pixels, RGB + 1 additional color\n"); break;
	case 0x0a: printf("Five sub-pixels, RGB + 2 additional colors\n"); break;
	case 0x0b: printf("Six sub-pixels, RGB + 3 additional colors\n"); break;
	case 0x0c: printf("Clairvoyante, Inc. PenTile Matrix (tm) layout\n"); break;
	default: printf("Reserved\n"); break;
	}
	printf("    Horizontal and vertical dot/pixel pitch: %.2fx%.2f mm\n",
	       (double)(x[0x0c]) / 100.0, (double)(x[0x0d]) / 100.0);
	printf("    Color bit depth: %u\n", x[0x0e] & 0x0f);
	v = x[0x0f];
	printf("    Response time for %s transition: %u ms\n",
	       (v & 0x80) ? "white-to-black" : "black-to-white", v & 0x7f);
}

// tag 0x0d

static void parse_displayid_intf_power_sequencing(const unsigned char *x)
{
	check_displayid_datablock_revision(x);

	if (!check_displayid_datablock_length(x, 6, 6))
		return;

	printf("    Power Sequence T1: %.1f-%u.0 ms\n", (x[3] >> 4) / 10.0, (x[3] & 0xf) * 2);
	printf("    Power Sequence T2: 0.0-%u.0 ms\n", (x[4] & 0x3f) * 2);
	printf("    Power Sequence T3: 0.0-%u.0 ms\n", (x[5] & 0x3f) * 2);
	printf("    Power Sequence T4: 0.0-%u.0 ms\n", (x[6] & 0x7f) * 10);
	printf("    Power Sequence T5: 0.0-%u.0 ms\n", (x[7] & 0x3f) * 10);
	printf("    Power Sequence T6: 0.0-%u.0 ms\n", (x[8] & 0x3f) * 10);
}

// tag 0x0e

void edid_state::parse_displayid_transfer_characteristics(const unsigned char *x)
{
	check_displayid_datablock_revision(x);

	unsigned xfer_id = x[1] >> 4;
	bool first_is_white = x[3] & 0x80;
	bool four_param = x[3] & 0x20;

	if (xfer_id) {
		printf("    Transfer Characteristics Data Block Identifier: %u\n", xfer_id);
		if (!(preparse_color_ids & (1 << xfer_id)))
			fail("Missing Color Characteristics Data Block using Identifier %u\n", xfer_id);
	}
	if (first_is_white)
		printf("    The first curve is the 'white' transfer characteristic\n");
	if (x[3] & 0x40)
		printf("    Individual response curves\n");

	unsigned offset = 4;
	unsigned len = x[2] - 1;

	for (unsigned i = 0; len; i++) {
		if ((x[3] & 0x80) && !i)
			printf("    White curve:      ");
		else
			printf("    Response curve #%u:",
			       i - first_is_white);
		unsigned samples = x[offset];
		if (four_param) {
			printf(" A0=%u A1=%u A2=%u A3=%u Gamma=%.2f\n",
			       x[offset + 1], x[offset + 2], x[offset + 3], x[offset + 4],
			       (double)(x[offset + 5] + 100.0) / 100.0);
			samples++;
		} else {
			for (unsigned j = offset + 1; j < offset + samples; j++)
				printf(" %3u", x[j]);
			printf(" 255\n");
		}
		offset += samples;
		len -= samples;
	}
}

// tag 0x0f

static void parse_displayid_display_intf(const unsigned char *x)
{
	check_displayid_datablock_revision(x);

	if (!check_displayid_datablock_length(x, 10, 10))
		return;

	printf("    Interface Type: ");
	switch (x[3] >> 4) {
	case 0x00:
		switch (x[3] & 0xf) {
		case 0x00: printf("Analog 15HD/VGA\n"); break;
		case 0x01: printf("Analog VESA NAVI-V (15HD)\n"); break;
		case 0x02: printf("Analog VESA NAVI-D\n"); break;
		default: printf("Reserved\n"); break;
		}
		break;
	case 0x01: printf("LVDS\n"); break;
	case 0x02: printf("TMDS\n"); break;
	case 0x03: printf("RSDS\n"); break;
	case 0x04: printf("DVI-D\n"); break;
	case 0x05: printf("DVI-I, analog\n"); break;
	case 0x06: printf("DVI-I, digital\n"); break;
	case 0x07: printf("HDMI-A\n"); break;
	case 0x08: printf("HDMI-B\n"); break;
	case 0x09: printf("MDDI\n"); break;
	case 0x0a: printf("DisplayPort\n"); break;
	case 0x0b: printf("Proprietary Digital Interface\n"); break;
	default: printf("Reserved\n"); break;
	}
	if (x[3] >> 4)
		printf("    Number of Links: %u\n", x[3] & 0xf);
	printf("    Interface Standard Version: %u.%u\n",
	       x[4] >> 4, x[4] & 0xf);
	print_flags("    Supported bpc for RGB encoding", x[5], bpc444);
	print_flags("    Supported bpc for YCbCr 4:4:4 encoding", x[6], bpc444);
	print_flags("    Supported bpc for YCbCr 4:2:2 encoding", x[7], bpc4xx);
	printf("    Supported Content Protection: ");
	switch (x[8] & 0xf) {
	case 0x00: printf("None\n"); break;
	case 0x01: printf("HDCP "); break;
	case 0x02: printf("DTCP "); break;
	case 0x03: printf("DPCP "); break;
	default: printf("Reserved "); break;
	}
	if (x[8] & 0xf)
		printf("%u.%u\n", x[9] >> 4, x[9] & 0xf);
	unsigned char v = x[0x0a] & 0xf;
	printf("    Spread Spectrum: ");
	switch (x[0x0a] >> 6) {
	case 0x00: printf("None\n"); break;
	case 0x01: printf("Down Spread %.1f%%\n", v / 10.0); break;
	case 0x02: printf("Center Spread %.1f%%\n", v / 10.0); break;
	case 0x03: printf("Reserved\n"); break;
	}
	switch (x[3] >> 4) {
	case 0x01:
		printf("    LVDS Color Mapping: %s mode\n",
		       (x[0x0b] & 0x10) ? "6 bit compatible" : "normal");
		if (x[0x0b] & 0x08) printf("    LVDS supports 2.8V\n");
		if (x[0x0b] & 0x04) printf("    LVDS supports 12V\n");
		if (x[0x0b] & 0x02) printf("    LVDS supports 5V\n");
		if (x[0x0b] & 0x01) printf("    LVDS supports 3.3V\n");
		printf("    LVDS %s Mode\n", (x[0x0c] & 0x04) ? "Fixed" : "DE");
		if (x[0x0c] & 0x04)
			printf("    LVDS %s Signal Level\n", (x[0x0c] & 0x02) ? "Low" : "High");
		else
			printf("    LVDS DE Polarity Active %s\n", (x[0x0c] & 0x02) ? "Low" : "High");
		printf("    LVDS Shift Clock Data Strobe at %s Edge\n", (x[0x0c] & 0x01) ? "Rising" : "Falling");
		break;
	case 0x0b:
		printf("    PDI %s Mode\n", (x[0x0b] & 0x04) ? "Fixed" : "DE");
		if (x[0x0b] & 0x04)
			printf("    PDI %s Signal Level\n", (x[0x0b] & 0x02) ? "Low" : "High");
		else
			printf("    PDI DE Polarity Active %s\n", (x[0x0b] & 0x02) ? "Low" : "High");
		printf("    PDI Shift Clock Data Strobe at %s Edge\n", (x[0x0b] & 0x01) ? "Rising" : "Falling");
		break;
	}
}

// tag 0x10 and 0x27

void edid_state::parse_displayid_stereo_display_intf(const unsigned char *x)
{
	check_displayid_datablock_revision(x);

	switch (x[1] >> 6) {
	case 0x00: printf("    Timings that explicitly report 3D capability\n"); break;
	case 0x01: printf("    Timings that explicitly report 3D capability & Timing Codes listed here\n"); break;
	case 0x02: printf("    All listed timings\n"); break;
	case 0x03: printf("    Only Timings Codes listed here\n"); break;
	}

	unsigned len = x[2];

	switch (x[4]) {
	case 0x00:
		printf("    Field Sequential Stereo (L/R Polarity: %s)\n",
		       (x[5] & 1) ? "0/1" : "1/0");
		break;
	case 0x01:
		printf("    Side-by-side Stereo (Left Half = %s Eye View)\n",
		       (x[5] & 1) ? "Right" : "Left");
		break;
	case 0x02:
		printf("    Pixel Interleaved Stereo:\n");
		for (unsigned y = 0; y < 8; y++) {
			unsigned char v = x[5 + y];

			printf("      ");
			for (int x = 7; x >= 0; x--)
				printf("%c", (v & (1 << x)) ? 'L' : 'R');
			printf("\n");
		}
		break;
	case 0x03:
		printf("    Dual Interface, Left and Right Separate\n");
		printf("      Carries the %s-eye view\n",
		       (x[5] & 1) ? "Right" : "Left");
		printf("      ");
		switch ((x[5] >> 1) & 3) {
		case 0x00: printf("No mirroring\n"); break;
		case 0x01: printf("Left/Right mirroring\n"); break;
		case 0x02: printf("Top/Bottom mirroring\n"); break;
		case 0x03: printf("Reserved\n"); break;
		}
		break;
	case 0x04:
		printf("    Multi-View: %u views, Interleaving Method Code: %u\n",
		       x[5], x[6]);
		break;
	case 0x05:
		printf("    Stacked Frame Stereo (Top Half = %s Eye View)\n",
		       (x[5] & 1) ? "Right" : "Left");
		break;
	case 0xff:
		printf("    Proprietary\n");
		break;
	default:
		printf("    Reserved\n");
		break;
	}
	if (!(x[1] & 0x40)) // Has No Timing Codes
		return;
	len -= 1 + x[3];
	x += 4 + x[3];
	while (1U + (x[0] & 0x1f) <= len) {
		unsigned num_codes = x[0] & 0x1f;
		unsigned type = x[0] >> 6;
		char suffix[16];

		for (unsigned i = 1; i <= num_codes; i++) {
			switch (type) {
			case 0x00:
				strcpy(suffix, "DMT");
				print_timings("    ", find_dmt_id(x[i]), suffix);
				break;
			case 0x01:
				sprintf(suffix, "VIC %3u", x[i]);
				print_timings("    ", find_vic_id(x[i]), suffix);
				break;
			case 0x02:
				sprintf(suffix, "HDMI VIC %u", x[i]);
				print_timings("    ", find_hdmi_vic_id(x[i]), suffix);
				break;
			}
		}

		len -= 1 + num_codes;
		x += 1 + num_codes;
	}
}

// tag 0x11

void edid_state::parse_displayid_type_5_timing(const unsigned char *x)
{
	struct timings t = {};
	std::string s("aspect ");

	t.hact = 1 + (x[2] | (x[3] << 8));
	t.vact = 1 + (x[4] | (x[5] << 8));
	calc_ratio(&t);
	s += std::to_string(t.hratio) + ":" + std::to_string(t.vratio);
	switch ((x[0] >> 5) & 0x3) {
	case 0:
		s += ", no 3D stereo";
		break;
	case 1:
		s += ", 3D stereo";
		break;
	case 2:
		s += ", 3D stereo depends on user action";
		break;
	case 3:
		s += ", reserved";
		fail("Reserved stereo 0x03\n");
		break;
	}
	if (x[0] & 0x10)
		s += ", refresh rate * (1000/1001) supported";
	if (x[0] & 0x80)
		s += ", preferred";

	switch (x[0] & 0x03) {
	case 0: t.rb = 2; break;
	case 1: t.rb = 1; break;
	default: break;
	}

	edid_cvt_mode(1 + x[6], t);

	print_detailed_timings("    ", t, s.c_str());
}

// tag 0x12 and 0x28

static void parse_displayid_tiled_display_topology(const unsigned char *x, bool is_v2)
{
	check_displayid_datablock_revision(x);

	if (!check_displayid_datablock_length(x, 22, 22))
		return;

	unsigned caps = x[3];
	unsigned num_v_tile = (x[4] & 0xf) | (x[6] & 0x30);
	unsigned num_h_tile = (x[4] >> 4) | ((x[6] >> 2) & 0x30);
	unsigned tile_v_location = (x[5] & 0xf) | ((x[6] & 0x3) << 4);
	unsigned tile_h_location = (x[5] >> 4) | (((x[6] >> 2) & 0x3) << 4);
	unsigned tile_width = x[7] | (x[8] << 8);
	unsigned tile_height = x[9] | (x[10] << 8);
	unsigned pix_mult = x[11];

	printf("    Capabilities:\n");
	printf("      Behavior if it is the only tile: ");
	switch (caps & 0x07) {
	case 0x00: printf("Undefined\n"); break;
	case 0x01: printf("Image is displayed at the Tile Location\n"); break;
	case 0x02: printf("Image is scaled to fit the entire tiled display\n"); break;
	case 0x03: printf("Image is cloned to all other tiles\n"); break;
	default: printf("Reserved\n"); break;
	}
	printf("      Behavior if more than one tile and fewer than total number of tiles: ");
	switch ((caps >> 3) & 0x03) {
	case 0x00: printf("Undefined\n"); break;
	case 0x01: printf("Image is displayed at the Tile Location\n"); break;
	default: printf("Reserved\n"); break;
	}
	if (caps & 0x80)
		printf("    Tiled display consists of multiple physical display enclosures\n");
	else
		printf("    Tiled display consists of a single physical display enclosure\n");
	printf("    Num horizontal tiles: %u Num vertical tiles: %u\n",
	       num_h_tile + 1, num_v_tile + 1);
	printf("    Tile location: %u, %u\n", tile_h_location, tile_v_location);
	printf("    Tile resolution: %ux%u\n", tile_width + 1, tile_height + 1);
	if (caps & 0x40) {
		if (pix_mult) {
			printf("    Top bevel size: %.1f pixels\n",
			       pix_mult * x[12] / 10.0);
			printf("    Bottom bevel size: %.1f pixels\n",
			       pix_mult * x[13] / 10.0);
			printf("    Right bevel size: %.1f pixels\n",
			       pix_mult * x[14] / 10.0);
			printf("    Left bevel size: %.1f pixels\n",
			       pix_mult * x[15] / 10.0);
		} else {
			fail("No bevel information, but the pixel multiplier is non-zero\n");
		}
		printf("    Tile resolution: %ux%u\n", tile_width + 1, tile_height + 1);
	} else if (pix_mult) {
		fail("No bevel information, but the pixel multiplier is non-zero\n");
	}
	if (is_v2)
		printf("    Tiled Display Manufacturer/Vendor ID: %02X-%02X-%02X\n",
		       x[0x10], x[0x11], x[0x12]);
	else
		printf("    Tiled Display Manufacturer/Vendor ID: %c%c%c\n",
		       x[0x10], x[0x11], x[0x12]);
	printf("    Tiled Display Product ID Code: %u\n",
	       x[0x13] | (x[0x14] << 8));
	printf("    Tiled Display Serial Number: %u\n",
	       x[0x15] | (x[0x16] << 8) | (x[0x17] << 16)| (x[0x18] << 24));
}

// tag 0x13

void edid_state::parse_displayid_type_6_timing(const unsigned char *x)
{
	struct timings t = {};
	std::string s("aspect ");

	t.pixclk_khz = 1 + (x[0] + (x[1] << 8) + ((x[2] & 0x3f) << 16));
	t.hact = 1 + (x[3] | ((x[4] & 0x3f) << 8));
	if ((x[4] >> 7) & 0x1)
		t.pos_pol_hsync = true;
	unsigned hbl = 1 + (x[7] | ((x[9] & 0xf) << 8));
	t.hfp = 1 + (x[8] | ((x[9] & 0xf0) << 4));
	t.hsync = 1 + x[10];
	t.hbp = hbl - t.hfp - t.hsync;
	t.vact = 1 + (x[5] | ((x[6] & 0x3f) << 8));
	if ((x[6] >> 7) & 0x1)
		t.pos_pol_vsync = true;
	unsigned vbl = 1 + x[11];
	t.vfp = 1 + x[12];
	t.vsync = 1 + (x[13] & 0x0f);
	t.vbp = vbl - t.vfp - t.vsync;

	if (x[13] & 0x80) {
		t.interlaced = true;
		t.vfp /= 2;
		t.vsync /= 2;
		t.vbp /= 2;
	}
	calc_ratio(&t);
	s += std::to_string(t.hratio) + ":" + std::to_string(t.vratio);
	if (x[2] & 0x40) {
		double aspect_mult = x[14] * 3.0 / 256.0;
		unsigned size_mult = 1 + (x[16] >> 4);

		t.vsize_mm = size_mult * (1 + (x[15] | ((x[16] & 0xf) << 8)));
		t.hsize_mm = t.vsize_mm * aspect_mult;
	}

	switch ((x[13] >> 5) & 0x3) {
	case 0:
		s += ", no 3D stereo";
		break;
	case 1:
		s += ", 3D stereo";
		break;
	case 2:
		s += ", 3D stereo depends on user action";
		break;
	case 3:
		s += ", reserved";
		fail("Reserved stereo 0x03\n");
		break;
	}

	if (x[2] & 0x80)
		s += ", preferred";

	print_detailed_timings("    ", t, s.c_str());
}

static std::string ieee7542d(unsigned short fp)
{
	int exp = ((fp & 0x7c00) >> 10) - 15;
	unsigned fract = (fp & 0x3ff) | 0x400;

	if (fp == 0x8000)
		return "do not use";
	if (fp & 0x8000)
		return "reserved";
	return std::to_string(pow(2, exp) * fract / 1024.0) + " cd/m^2";
}

// tag 0x21

static void parse_displayid_parameters_v2(const unsigned char *x)
{
	check_displayid_datablock_revision(x);

	if (!check_displayid_datablock_length(x, 29, 29))
		return;

	unsigned hor_size = (x[4] << 8) + x[3];
	unsigned vert_size = (x[6] << 8) + x[5];

	if (x[1] & 0x80)
		printf("    Image size: %u mm x %u mm\n",
		       hor_size, vert_size);
	else
		printf("    Image size: %.1f mm x %.1f mm\n",
		       hor_size / 10.0, vert_size / 10.0);
	printf("    Pixels: %d x %d\n",
	       (x[8] << 8) + x[7], (x[10] << 8) + x[9]);
	unsigned char v = x[11];
	printf("    Scan Orientation: ");
	switch (v & 0x07) {
	case 0x00: printf("Left to Right, Top to Bottom\n"); break;
	case 0x01: printf("Right to Left, Top to Bottom\n"); break;
	case 0x02: printf("Top to Bottom, Right to Left\n"); break;
	case 0x03: printf("Bottom to Top, Right to Left\n"); break;
	case 0x04: printf("Right to Left, Bottom to Top\n"); break;
	case 0x05: printf("Left to Right, Bottom to Top\n"); break;
	case 0x06: printf("Bottom to Top, Left to Right\n"); break;
	case 0x07: printf("Top to Bottom, Left to Right\n"); break;
	}
	printf("    Luminance Information: ");
	switch ((v >> 3) & 0x03) {
	case 0x00: printf("Minimum guaranteed value\n"); break;
	case 0x01: printf("Guidance for the Source device\n"); break;
	default: printf("Reserved\n"); break;
	}
	printf("    Color Information: CIE %u\n",
	       (v & 0x40) ? 1976 : 1931);
	printf("    Audio Speaker Information: %sintegrated\n",
	       (v & 0x80) ? "not " : "");
	printf("    Native Color Chromaticity:\n");
	printf("      Primary #1:  (%.6f, %.6f)\n",
	       fp2d(x[0x0c] | ((x[0x0d] & 0x0f) << 8)),
	       fp2d(((x[0x0d] & 0xf0) >> 4) | (x[0x0e] << 4)));
	printf("      Primary #2:  (%.6f, %.6f)\n",
	       fp2d(x[0x0f] | ((x[0x10] & 0x0f) << 8)),
	       fp2d(((x[0x10] & 0xf0) >> 4) | (x[0x11] << 4)));
	printf("      Primary #3:  (%.6f, %.6f)\n",
	       fp2d(x[0x12] | ((x[0x13] & 0x0f) << 8)),
	       fp2d(((x[0x13] & 0xf0) >> 4) | (x[0x14] << 4)));
	printf("      White Point: (%.6f, %.6f)\n",
	       fp2d(x[0x15] | ((x[0x16] & 0x0f) << 8)),
	       fp2d(((x[0x16] & 0xf0) >> 4) | (x[0x17] << 4)));
	printf("    Native Maximum Luminance (Full Coverage): %s\n",
	       ieee7542d(x[0x18] | (x[0x19] << 8)).c_str());
	printf("    Native Maximum Luminance (10%% Rectangular Coverage): %s\n",
	       ieee7542d(x[0x1a] | (x[0x1b] << 8)).c_str());
	printf("    Native Minimum Luminance: %s\n",
	       ieee7542d(x[0x1c] | (x[0x1d] << 8)).c_str());
	printf("    Native Color Depth: ");
	if (bpc444[x[0x1e] & 0x07])
		printf("%s bpc\n", bpc444[x[0x1e] & 0x07]);
	else
		printf("Reserved\n");
	printf("    Display Device Technology: ");
	switch ((x[0x1e] >> 4) & 0x07) {
	case 0x00: printf("Not Specified\n"); break;
	case 0x01: printf("Active Matrix LCD\n"); break;
	case 0x02: printf("Organic LED\n"); break;
	default: printf("Reserved\n"); break;
	}
	if (x[0x1f] != 0xff)
		printf("    Native Gamma EOTF: %.2f\n",
		       (100 + x[0x1f]) / 100.0);
}

// tag 0x24

void edid_state::parse_displayid_type_9_timing(const unsigned char *x)
{
	struct timings t = {};
	std::string s("aspect ");

	t.hact = 1 + (x[1] | (x[2] << 8));
	t.vact = 1 + (x[3] | (x[4] << 8));
	calc_ratio(&t);
	s += std::to_string(t.hratio) + ":" + std::to_string(t.vratio);
	switch ((x[0] >> 5) & 0x3) {
	case 0:
		s += ", no 3D stereo";
		break;
	case 1:
		s += ", 3D stereo";
		break;
	case 2:
		s += ", 3D stereo depends on user action";
		break;
	case 3:
		s += ", reserved";
		fail("Reserved stereo 0x03\n");
		break;
	}
	if (x[0] & 0x10)
		s += ", refresh rate * (1000/1001) supported";

	switch (x[0] & 0x07) {
	case 1: t.rb = 1; break;
	case 2: t.rb = 2; break;
	default: break;
	}

	edid_cvt_mode(1 + x[5], t);

	print_detailed_timings("    ", t, s.c_str());
}

// tag 0x25

static void parse_displayid_dynamic_video_timings_range_limits(const unsigned char *x)
{
	check_displayid_datablock_revision(x);

	if (!check_displayid_datablock_length(x, 9, 9))
		return;

	printf("    Minimum Pixel Clock: %u kHz\n",
	       1 + (x[3] | (x[4] << 8) | (x[5] << 16)));
	printf("    Maximum Pixel Clock: %u kHz\n",
	       1 + (x[6] | (x[7] << 8) | (x[8] << 16)));
	printf("    Minimum Vertical Refresh Rate: %u Hz\n", x[9]);
	printf("    Maximum Vertical Refresh Rate: %u Hz\n", x[10]);
	printf("    Seamless Dynamic Video Timing Support: %s\n",
	       (x[11] & 0x80) ? "Yes" : "No");
}

// tag 0x26

static const char *colorspace_eotf_combinations[] = {
	"sRGB",
	"BT.601",
	"BT.709/BT.1886",
	"Adobe RGB",
	"DCI-P3",
	"BT.2020",
	"BT.2020/SMPTE ST 2084"
};

static const char *colorspace_eotf_reserved[] = { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };

static const char *colorspaces[] = {
	"Undefined",
	"sRGB",
	"BT.601",
	"BT.709",
	"Adobe RGB",
	"DCI-P3",
	"BT.2020",
	"Custom"
};

static const char *eotfs[] = {
	"Undefined",
	"sRGB",
	"BT.601",
	"BT.1886",
	"Adobe RGB",
	"DCI-P3",
	"BT.2020",
	"Gamma function",
	"SMPTE ST 2084",
	"Hybrid Log",
	"Custom"
};

static void parse_displayid_interface_features(const unsigned char *x)
{
	check_displayid_datablock_revision(x);

	if (!check_displayid_datablock_length(x, 9))
		return;

	unsigned len = x[2];
	if (len > 0) print_flags("    Supported bpc for RGB encoding", x[3], bpc444);
	if (len > 1) print_flags("    Supported bpc for YCbCr 4:4:4 encoding", x[4], bpc444);
	if (len > 2) print_flags("    Supported bpc for YCbCr 4:2:2 encoding", x[5], bpc4xx);
	if (len > 3) print_flags("    Supported bpc for YCbCr 4:2:0 encoding", x[6], bpc4xx);
	if (len > 4 && x[7])
		printf("    Minimum pixel rate at which YCbCr 4:2:0 encoding is supported: %.3f MHz\n",
		       74.25 * x[7]);
	if (len > 5) print_flags("    Supported audio capability and features (kHz)",
				 x[8], audiorates, true);
	if (len > 6) print_flags("    Supported color space and EOTF standard combination 1",
				 x[9], colorspace_eotf_combinations);
	if (len > 7) print_flags("    Supported color space and EOTF standard combination 2",x[10], colorspace_eotf_reserved);

	unsigned i = 0;

	if (len > 8 && x[11]) {
		printf("    Supported color space and EOTF additional combinations:");
		for (i = 0; i < x[11]; i++) {
			if (i > 6) {
				printf("\n    Number of additional color space and EOTF combinations (%d) is greater than allowed (7).", x[11]);
				break;
			} else if (i + 10 > len) {
				printf("\n    Number of additional color space and EOTF combinations (%d) is too many to fit in block (%d).", x[11], len - 9);
				break;
			}

			const char *colorspace = "Out of range";
			const char *eotf = "Out of range";
			unsigned colorspace_index = (x[12 + i] >> 4) & 0xf;
			unsigned eotf_index = x[12 + i] & 0xf;

			if (colorspace_index < sizeof(colorspaces) / sizeof(colorspaces[0]))
				colorspace = colorspaces[colorspace_index];
			if (eotf_index < sizeof(eotfs) / sizeof(eotfs[0]))
				eotf = eotfs[eotf_index];

			if (i > 0)
				printf(", ");
			if (!strcmp(colorspace, eotf))
				printf("%s", colorspace);
			else
				printf("%s/%s", colorspace, eotf);
		}
		printf("\n");
	}
	check_displayid_datablock_length(x, 9 + i, 9 + i, 9 + i);
}

// tag 0x29

static void parse_displayid_ContainerID(const unsigned char *x)
{
	check_displayid_datablock_revision(x);

	if (check_displayid_datablock_length(x, 16, 16)) {
		x += 3;
		printf("    %02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x\n",
		       x[0], x[1], x[2], x[3],
		       x[4], x[5],
		       x[6], x[7],
		       x[8], x[9],
		       x[10], x[11], x[12], x[13], x[14], x[15]);
	}
}

// DisplayID main

static std::string product_type(unsigned version, unsigned char x, bool heading)
{
	std::string headingstr;

	if (version < 0x20) {
		headingstr = "Display Product Type";
		if (heading) return headingstr;
		switch (x) {
		case 0: return "Extension Section";
		case 1: return "Test Structure; test equipment only";
		case 2: return "Display panel or other transducer, LCD or PDP module, etc.";
		case 3: return "Standalone display device";
		case 4: return "Television receiver";
		case 5: return "Repeater/translator";
		case 6: return "DIRECT DRIVE monitor";
		default: break;
		}
	} else {
		headingstr = "Display Product Primary Use Case";
		if (heading) return headingstr;
		switch (x) {
		case 0: return "Same primary use case as the base section";
		case 1: return "Test Structure; test equipment only";
		case 2: return "None of the listed primary use cases; generic display";
		case 3: return "Television (TV) display";
		case 4: return "Desktop productivity display";
		case 5: return "Desktop gaming display";
		case 6: return "Presentation display";
		case 7: return "Head-mounted Virtual Reality (VR) display";
		case 8: return "Head-mounted Augmented Reality (AR) display";
		default: break;
		}
	}
	fail("Unknown %s 0x%02x\n", headingstr.c_str(), x);
	return std::string("Unknown " + headingstr + " (") + utohex(x) + ")";
}

void edid_state::preparse_displayid_block(const unsigned char *x)
{
	unsigned length = x[2];

	if (length > 121)
		length = 121;

	unsigned offset = 5;

	while (length > 0) {
		unsigned tag = x[offset];

		switch (tag) {
		case 0x02:
			preparse_color_ids |= 1 << ((x[offset + 1] >> 3) & 0x0f);
			break;
		case 0x0e:
			preparse_xfer_ids |= 1 << ((x[offset + 1] >> 4) & 0x0f);
			break;
		default:
			break;
		}

		if (length < 3)
			break;

		unsigned len = x[offset + 2];

		if (length < len + 3)
			break;

		if (!tag && !len)
			break;

		length -= len + 3;
		offset += len + 3;
	}
}

void edid_state::parse_displayid_block(const unsigned char *x)
{
	unsigned version = x[1];
	unsigned length = x[2];
	unsigned prod_type = x[3]; // future check: based on type, check for required data blocks
	unsigned ext_count = x[4];
	unsigned i;

	printf("%s Version %u.%u Length %u Extension Count %u\n",
	       block.c_str(), version >> 4, version & 0xf,
	       length, ext_count);

	if (ext_count > 0)
		warn("Non-0 DisplayID extension count %d\n", ext_count);

	printf("%s: %s\n", product_type(version, prod_type, true).c_str(),
	       product_type(version, prod_type, false).c_str());

	if (length > 121) {
		fail("DisplayID length %d is greater than 121\n", length);
		length = 121;
	}

	unsigned offset = 5;
	while (length > 0) {
		unsigned tag = x[offset];

		switch (tag) {
			// DisplayID 1.3:
		case 0x00: data_block = "Product Identification Data Block (" + utohex(tag) + ")"; break;
		case 0x01: data_block = "Display Parameters Data Block (" + utohex(tag) + ")"; break;
		case 0x02: data_block = "Color Characteristics Data Block"; break;
		case 0x03: data_block = "Video Timing Modes Type 1 - Detailed Timings Data Block"; break;
		case 0x04: data_block = "Video Timing Modes Type 2 - Detailed Timings Data Block"; break;
		case 0x05: data_block = "Video Timing Modes Type 3 - Short Timings Data Block"; break;
		case 0x06: data_block = "Video Timing Modes Type 4 - DMT Timings Data Block"; break;
		case 0x07: data_block = "Supported Timing Modes Type 1 - VESA DMT Timings Data Block"; break;
		case 0x08: data_block = "Supported Timing Modes Type 2 - CTA Timings Data Block"; break;
		case 0x09: data_block = "Video Timing Range Data Block"; break;
		case 0x0a: data_block = "Product Serial Number Data Block"; break;
		case 0x0b: data_block = "GP ASCII String Data Block"; break;
		case 0x0c: data_block = "Display Device Data Data Block"; break;
		case 0x0d: data_block = "Interface Power Sequencing Data Block"; break;
		case 0x0e: data_block = "Transfer Characteristics Data Block"; break;
		case 0x0f: data_block = "Display Interface Data Block"; break;
		case 0x10: data_block = "Stereo Display Interface Data Block (" + utohex(tag) + ")"; break;
		case 0x11: data_block = "Video Timing Modes Type 5 - Short Timings Data Block"; break;
		case 0x12: data_block = "Tiled Display Topology Data Block (" + utohex(tag) + ")"; break;
		case 0x13: data_block = "Video Timing Modes Type 6 - Detailed Timings Data Block"; break;
			   // 0x14 .. 0x7e RESERVED for Additional VESA-defined Data Blocks
			   // DisplayID 2.0
		case 0x20: data_block = "Product Identification Data Block (" + utohex(tag) + ")"; break;
		case 0x21: data_block = "Display Parameters Data Block (" + utohex(tag) + ")"; break;
		case 0x22: data_block = "Video Timing Modes Type 7 - Detailed Timings Data Block"; break;
		case 0x23: data_block = "Video Timing Modes Type 8 - Enumerated Timing Codes Data Block"; break;
		case 0x24: data_block = "Video Timing Modes Type 9 - Formula-based Timings Data Block"; break;
		case 0x25: data_block = "Dynamic Video Timing Range Limits Data Block"; break;
		case 0x26: data_block = "Display Interface Features Data Block"; break;
		case 0x27: data_block = "Stereo Display Interface Data Block (" + utohex(tag) + ")"; break;
		case 0x28: data_block = "Tiled Display Topology Data Block (" + utohex(tag) + ")"; break;
		case 0x29: data_block = "ContainerID Data Block"; break;
			   // 0x2a .. 0x7d RESERVED for Additional VESA-defined Data Blocks
		case 0x7e: // DisplayID 2.0
		case 0x7f: data_block = "Vendor-specific Data Block (" + utohex(tag) + ")"; break; // DisplayID 1.3
			   // 0x7f .. 0x80 RESERVED
		case 0x81: data_block = "CTA DisplayID Data Block (" + utohex(tag) + ")"; break;
			   // 0x82 .. 0xff RESERVED
		default:   data_block = "Unknown DisplayID Data Block (" + utohex(tag) + ")"; break;
		}

		if (length < 3) {
			// report a problem when the remaining bytes are not 0.
			if (tag || x[offset + 1]) {
				fail("Not enough bytes remain (%d) for a DisplayID data block or the DisplayID filler is non-0\n", length);
			}
			break;
		}

		unsigned len = x[offset + 2];

		if (length < len + 3) {
			fail("The length of this DisplayID data block (%d) exceeds the number of bytes remaining (%d)\n", len + 3, length);
			break;
		}

		if (!tag && !len) {
			// A Product Identification Data Block with no payload bytes is not valid - assume this is the end.
			if (!memchk(x + offset, length)) {
				fail("Non-0 filler bytes in the DisplayID block\n");
			}
			break;
		}

		printf("  %s\n", data_block.c_str());

		switch (tag) {
		case 0x00: parse_displayid_product_id(x + offset, false); break;
		case 0x01: parse_displayid_parameters(x + offset); break;
		case 0x02: parse_displayid_color_characteristics(x + offset); break;
		case 0x03:
			   for (i = 0; i < len / 20; i++)
				   parse_displayid_type_1_7_timing(&x[offset + 3 + (i * 20)], false);
			   break;
		case 0x04:
			   for (i = 0; i < len / 11; i++)
				   parse_displayid_type_2_timing(&x[offset + 3 + (i * 11)]);
			   break;
		case 0x05:
			   for (i = 0; i < len / 3; i++)
				   parse_displayid_type_3_timing(&x[offset + 3 + (i * 3)]);
			   break;
		case 0x06:
			   for (i = 0; i < len; i++)
				   parse_displayid_type_4_8_timing((x[offset + 1] & 0xc0) >> 6, x[offset + 3 + i]);
			   break;
		case 0x07:
			   for (i = 0; i < min(len, 10) * 8; i++)
				   if (x[offset + 3 + i / 8] & (1 << (i % 8))) {
					   print_timings("    ", find_dmt_id(i + 1), "DMT");
				   }
			   break;
		case 0x08:
			   for (i = 0; i < min(len, 8) * 8; i++)
				   if (x[offset + 3 + i / 8] & (1 << (i % 8))) {
					   char suffix[16];
					   sprintf(suffix, "VIC %3u", i + 1);
					   print_timings("    ", find_vic_id(i + 1), suffix);
				   }
			   break;
		case 0x09: parse_displayid_video_timing_range_limits(x + offset); break;
		case 0x0a:
		case 0x0b: parse_displayid_string(x + offset); break;
		case 0x0c: parse_displayid_display_device(x + offset); break;
		case 0x0d: parse_displayid_intf_power_sequencing(x + offset); break;
		case 0x0e: parse_displayid_transfer_characteristics(x + offset); break;
		case 0x0f: parse_displayid_display_intf(x + offset); break;
		case 0x10: parse_displayid_stereo_display_intf(x + offset); break;
		case 0x11:
			   for (i = 0; i < len / 7; i++)
				   parse_displayid_type_5_timing(&x[offset + 3 + (i * 7)]);
			   break;
		case 0x12: parse_displayid_tiled_display_topology(x + offset, false); break;
		case 0x13:
			   for (i = 0; i < len; i += (x[offset + 3 + i + 2] & 0x40) ? 17 : 14)
				   parse_displayid_type_6_timing(&x[offset + 3 + i]);
			   break;
		case 0x20: parse_displayid_product_id(x + offset, true); break;
		case 0x21: parse_displayid_parameters_v2(x + offset); break;
		case 0x22:
			   for (i = 0; i < len / 20; i++)
				   parse_displayid_type_1_7_timing(&x[offset + 3 + i * 20], true);
			   break;
		case 0x23:
			   if (x[offset + 1] & 0x08) {
				   for (i = 0; i < len / 2; i++)
					   parse_displayid_type_4_8_timing((x[offset + 1] & 0xc0) >> 6,
									   x[offset + 3 + i * 2] |
									   (x[offset + 4 + i * 2] << 8));
			   } else {
				   for (i = 0; i < len; i++)
					   parse_displayid_type_4_8_timing((x[offset + 1] & 0xc0) >> 6,
									   x[offset + 3 + i]);
			   }
			   break;
		case 0x24:
			   for (i = 0; i < len / 6; i++)
				   parse_displayid_type_9_timing(&x[offset + 3 + i * 6]);
			   break;
		case 0x25: parse_displayid_dynamic_video_timings_range_limits(x + offset); break;
		case 0x26: parse_displayid_interface_features(x + offset); break;
		case 0x27: parse_displayid_stereo_display_intf(x + offset); break;
		case 0x28: parse_displayid_tiled_display_topology(x + offset, true); break;
		case 0x29: parse_displayid_ContainerID(x + offset); break;
		default: hex_block("    ", x + offset + 3, len); break;
		}
		length -= len + 3;
		offset += len + 3;
	}

	/*
	 * DisplayID length field is number of following bytes
	 * but checksum is calculated over the entire structure
	 * (excluding DisplayID-in-EDID magic byte)
	 */
	data_block.clear();
	do_checksum("  ", x + 1, x[2] + 5);

	if (!memchk(x + 1 + x[2] + 5, 0x7f - (1 + x[2] + 5))) {
		data_block = "Padding";
		fail("DisplayID padding contains non-zero bytes\n");
	}
}
