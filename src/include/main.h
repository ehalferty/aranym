/*
 * $Header$
 *
 * 2001 MJ
 */

/*
 *  main.h - General definitions
 *
 *  Basilisk II (C) 1997-2000 Christian Bauer
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef MAIN_H
#define MAIN_H

// CPU type (0 = 68000, 1 = 68010, 2 = 68020, 3 = 68030, 4 = 68040/060)
extern int CPUType;
// MJ extern bool CPUIs68060;		// Flag to distinguish 68040 and 68060

// FPU type (0 = no FPU, 1 = 68881, 2 = 68882)
extern int FPUType;

// Flag: 24-bit-addressing?
extern bool TwentyFourBitAddressing;

// 68k register structure (for Execute68k())
struct M68kRegisters {
	uint32 d[8];
	uint32 a[8];
	uint16 sr;
};

// General functions
extern bool InitAll(void);
extern void ExitAll(void);

// Platform-specific functions
extern void FlushCodeCache(void *start, uint32 size);	// Code was patched, flush caches if neccessary
extern void QuitEmulator(void);				// Quit emulator
extern void ErrorAlert(const char *text);		// Display error alert
extern void WarningAlert(const char *text);		// Display warning alert
extern bool ChoiceAlert(const char *text, const char *pos, const char *neg);	// Display choice alert

// Interrupt flags
enum {
	INTFLAG_200HZ = 1,	// 200Hz VBL
	INTFLAG_1HZ = 2,	// ~1Hz interrupt
// MJ	INTFLAG_SERIAL = 4,	// Serial driver
// MJ	INTFLAG_ETHER = 8,	// Ethernet driver
// MJ	INTFLAG_AUDIO = 16,	// Audio block read
	INTFLAG_TIMER = 32,	// Time Manager
	INTFLAG_NMI = 64	// NMI
};

extern uint32 InterruptFlags;			// Currently pending interrupts
extern void SetInterruptFlag(uint32 flag);	// Set/clear interrupt flags
extern void ClearInterruptFlag(uint32 flag);

#endif


/*
 * $Log$
 *
 */
