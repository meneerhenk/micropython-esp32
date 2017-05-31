#include "sdkconfig.h"
#include <gde.h>
#include <gde-driver.h>

#include "badge_pins.h"
#include "badge_eink.h"

static const uint8_t xlat_curve[256] = {
    0x00,0x01,0x01,0x02,0x02,0x03,0x03,0x03,0x04,0x04,0x05,0x05,
    0x06,0x06,0x07,0x07,0x08,0x08,0x09,0x09,0x0a,0x0a,0x0a,0x0b,
    0x0b,0x0c,0x0c,0x0d,0x0d,0x0e,0x0e,0x0f,0x0f,0x10,0x10,0x11,
    0x11,0x12,0x12,0x13,0x13,0x14,0x15,0x15,0x16,0x16,0x17,0x17,
    0x18,0x18,0x19,0x19,0x1a,0x1a,0x1b,0x1b,0x1c,0x1d,0x1d,0x1e,
    0x1e,0x1f,0x1f,0x20,0x20,0x21,0x22,0x22,0x23,0x23,0x24,0x25,
    0x25,0x26,0x26,0x27,0x27,0x28,0x29,0x29,0x2a,0x2a,0x2b,0x2c,
    0x2c,0x2d,0x2e,0x2e,0x2f,0x2f,0x30,0x31,0x31,0x32,0x33,0x33,
    0x34,0x35,0x35,0x36,0x37,0x37,0x38,0x39,0x39,0x3a,0x3b,0x3b,
    0x3c,0x3d,0x3e,0x3e,0x3f,0x40,0x40,0x41,0x42,0x43,0x43,0x44,
    0x45,0x46,0x46,0x47,0x48,0x49,0x49,0x4a,0x4b,0x4c,0x4c,0x4d,
    0x4e,0x4f,0x50,0x50,0x51,0x52,0x53,0x54,0x55,0x55,0x56,0x57,
    0x58,0x59,0x5a,0x5b,0x5b,0x5c,0x5d,0x5e,0x5f,0x60,0x61,0x62,
    0x63,0x64,0x65,0x66,0x66,0x67,0x68,0x69,0x6a,0x6b,0x6c,0x6d,
    0x6e,0x6f,0x70,0x71,0x72,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,
    0x7b,0x7c,0x7d,0x7e,0x80,0x81,0x82,0x83,0x84,0x86,0x87,0x88,
    0x89,0x8a,0x8c,0x8d,0x8e,0x90,0x91,0x92,0x93,0x95,0x96,0x98,
    0x99,0x9a,0x9c,0x9d,0x9f,0xa0,0xa2,0xa3,0xa5,0xa6,0xa8,0xa9,
    0xab,0xac,0xae,0xb0,0xb1,0xb3,0xb5,0xb6,0xb8,0xba,0xbc,0xbe,
    0xbf,0xc1,0xc3,0xc5,0xc7,0xc9,0xcb,0xcd,0xcf,0xd1,0xd3,0xd6,
    0xd8,0xda,0xdc,0xdf,0xe1,0xe3,0xe6,0xe8,0xeb,0xed,0xf0,0xf3,
    0xf5,0xf8,0xfb,0xfe,
};

static void
write_bitplane(const uint8_t *img, int y_start, int y_end, int bit, int flags)
{
#ifdef EPD_ROTATED_180
	flags ^= DISPLAY_FLAG_ROTATE_180;
#endif
	setRamArea(0, DISP_SIZE_X_B - 1, 0, DISP_SIZE_Y - 1);
	setRamPointer(0, 0);
	gdeWriteCommandInit(0x24);
	int x, y;
	int pos, dx, dy;
	if (flags & DISPLAY_FLAG_ROTATE_180)
	{
		pos = DISP_SIZE_Y-1;
		dx = DISP_SIZE_Y;
		dy = -DISP_SIZE_Y*DISP_SIZE_X - 1;
	}
	else
	{
		pos = (DISP_SIZE_X-1)*DISP_SIZE_Y;
		dx = -DISP_SIZE_Y;
		dy = DISP_SIZE_Y*DISP_SIZE_X + 1;
	}
	for (y = 0; y < DISP_SIZE_Y; y++) {
		if (y < y_start || y > y_end)
		{
			for (x = 0; x < DISP_SIZE_X_B; x++) {
				gdeWriteByte(0);
			}
			pos += dx * DISP_SIZE_X;
		}
		else
		{
			for (x = 0; x < DISP_SIZE_X;) {
				int x_bits;
				uint8_t res = 0;
				for (x_bits=0; x_bits<8; x_bits++)
				{
					res <<= 1;
					if (flags & DISPLAY_FLAG_GREYSCALE)
					{
						uint8_t pixel = img[pos];
						pos += dx;
						int j = xlat_curve[pixel];
						if ((j & bit) != 0)
							res++;
					}
					else
					{
						uint8_t pixel = img[pos >> 3] >> (pos & 7);
						pos += dx;
						if ((pixel & 1) != 0)
							res++;
					}
					x++;
				}
				gdeWriteByte(res);
			}
		}
		pos += dy;
	}
	gdeWriteCommandEnd();
}

const struct badge_eink_update eink_upd_default = {
	.lut      = LUT_DEFAULT,
	.reg_0x3a = 26,   // 26 dummy lines per gate
	.reg_0x3b = 0x08, // 62us per line
	.y_start  = 0,
	.y_end    = 295,
};

void
badge_eink_update(const struct badge_eink_update *upd_conf)
{
	if (upd_conf->lut == -1)
	{
#ifndef CONFIG_SHA_BADGE_EINK_DEPG0290B1
		gdeWriteCommandStream(0x32, upd_conf->lut_custom, 30);
#else
		gdeWriteCommandStream(0x32, upd_conf->lut_custom, 70);
#endif
	}
	else
	{
		writeLUT(upd_conf->lut);
	}
	gdeWriteCommand_p1(0x3a, upd_conf->reg_0x3a);
	gdeWriteCommand_p1(0x3b, upd_conf->reg_0x3b);

	uint16_t y_len = upd_conf->y_end - upd_conf->y_start;
	gdeWriteCommand_p3(0x01, y_len & 0xff, y_len >> 8, 0x00);
	gdeWriteCommand_p2(0x0f, upd_conf->y_start & 0xff, upd_conf->y_start >> 8);
	gdeWriteCommand_p1(0x22, 0xc7);
	gdeWriteCommand(0x20);
	gdeBusyWait();
}

void
badge_eink_display(const uint8_t *img, int mode)
{
	int lut_mode =
		(mode >> DISPLAY_FLAG_LUT_BIT) & ((1 << DISPLAY_FLAG_LUT_SIZE)-1);

	// trying to get rid of all ghosting and end with a black screen.
	if (((mode & DISPLAY_FLAG_NO_UPDATE) == 0) && lut_mode == 0)
	{
		int i;
		for (i = 0; i < 3; i++) {
			/* draw initial pattern */
			setRamArea(0, DISP_SIZE_X_B - 1, 0, DISP_SIZE_Y - 1);
			setRamPointer(0, 0);
			gdeWriteCommandInit(0x24);
			int c;
			for (c = 0; c < DISP_SIZE_X_B * DISP_SIZE_Y; c++)
				gdeWriteByte((i & 1) ? 0xff : 0x00);
			gdeWriteCommandEnd();

			/* update display */
			badge_eink_update(&eink_upd_default);
		}
	}

	// is it a 1 bit per pixel image?
	if ((mode & DISPLAY_FLAG_GREYSCALE) == 0)
	{
		write_bitplane(img, 0, DISP_SIZE_Y-1, 0, (mode & DISPLAY_FLAG_ROTATE_180));
		if ((mode & DISPLAY_FLAG_NO_UPDATE) == 0)
		{
			struct badge_eink_update eink_upd = {
				.lut      = lut_mode > 0 ? lut_mode - 1 : 0,
				.reg_0x3a = 26,   // 26 dummy lines per gate
				.reg_0x3b = 0x08, // 62us per line
				.y_start  = 0,
				.y_end    = 295,
			};
			badge_eink_update(&eink_upd);
		}
		return;
	}

	int i;
	for (i = 64; i > 0; i >>= 1) {
		int ii = i;
		int p = 8;

		while ((ii & 1) == 0 && (p > 1)) {
			ii >>= 1;
			p >>= 1;
		}

		int j;
		for (j = 0; j < p; j++) {
			int y_start = 0 + j * (DISP_SIZE_Y / p);
			int y_end = y_start + (DISP_SIZE_Y / p) - 1;

			write_bitplane(img, y_start, y_end, i << 1, DISPLAY_FLAG_GREYSCALE|(mode & DISPLAY_FLAG_ROTATE_180));

#ifndef CONFIG_SHA_BADGE_EINK_DEPG0290B1
			// LUT:
			//   Ignore old state;
			//   Do nothing when bit is not set;
			//   Make pixel whiter when bit is set;
			//   Duration is <ii> cycles.
			uint8_t lut[30] = {
				0, 0x88, 0, 0, 0,         0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
				0, 0   , 0, 0, 0, (ii<<4)|1, 1, 0, 0, 0, 0, 0, 0, 0, 0,
			};
#else // CONFIG_SHA_BADGE_EINK_DEPG0290B1
			uint8_t lut[70] = {
				0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // group A (cycle 1..7)
				0xa0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // group B
				0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // group C
				0xa0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // group D
				0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // VCOM

				// timings per cycle
				ii*3, 0x00, 0x00, 0x00, 0x00,
			// empty slots:
				0x00, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00, 0x00,
			};
#endif // CONFIG_SHA_BADGE_EINK_DEPG0290B1

			/* update display */
			struct badge_eink_update eink_upd = {
				.lut        = -1,
				.lut_custom = lut,
				.reg_0x3a   = 0, // no dummy lines per gate
				.reg_0x3b   = 0, // 30us per line
				.y_start    = y_start,
				.y_end      = y_end + 1,
			};
			badge_eink_update(&eink_upd);
		}
	}

	gdeWriteCommand_p1(0x3a, 0x1a); // 26 dummy lines per gate
	gdeWriteCommand_p1(0x3b, 0x08); // 62us per line
}

void
badge_eink_init(void)
{
	// initialize spi interface to display
	gdeInit();

	// initialize display
	initDisplay();
}