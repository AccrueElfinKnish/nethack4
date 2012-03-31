/* Copyright (c) Stichting Mathematisch Centrum, Amsterdam, 1985. */
/* NetHack may be freely redistributed.  See license for details. */

/*
 * Incrementing EDITLEVEL can be used to force invalidation of old bones
 * and save files.
 */
#define EDITLEVEL	0

#define COPYRIGHT_BANNER_A \
"NetHack 4, Copyright (C) 2009-2012; based on NetHack, Copyright (C) 1985-2003"

#define COPYRIGHT_BANNER_B \
"  NetHack 4 by Alex Smith, Daniel Thaler and many other contributors."

#define COPYRIGHT_BANNER_C \
"  NetHack by Stichting Mathematisch Centrum and M. Stephenson. See dat/licence."

/*
 * If two or more successive releases have compatible data files, define
 * this with the version number of the oldest such release so that the
 * new release will accept old save and bones files.  The format is
 *	0xMMmmPPeeL
 * 0x = literal prefix "0x", MM = major version, mm = minor version,
 * PP = patch level, ee = edit level, L = literal suffix "L",
 * with all four numbers specified as two hexadecimal digits.
 * Keep this consistent with nethack.h.
 */
#define VERSION_COMPATIBILITY 0x04020000L	/* 4.2.0-0 */


/*patchlevel.h*/
