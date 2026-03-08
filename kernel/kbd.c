#include "types.h"
#include "x86.h"
#include "defs.h"
#include "kbd.h"

int
kbdgetc(void)
{
	static uint shift;
	static uint alt;
	static uchar *charcode[4] = {
		normalmap, shiftmap, ctlmap, ctlmap
	};
	uint st, data, c;

	st = inb(KBSTATP);
	if((st & KBS_DIB) == 0)
		return -1;
	data = inb(KBDATAP);

	if(data == 0xE0){
		shift |= E0ESC;
		return 0;
	} else if(data & 0x80){
		// Key released
		data = (shift & E0ESC ? data : data & 0x7F);
		shift &= ~(shiftcode[data] | E0ESC);
		return 0;
	} else if(shift & E0ESC){
		// Last character was an E0 escape; or with 0x80
		data |= 0x80;
		shift &= ~E0ESC;
	}

	if (data == 0x38) { // Left ALT pressed
		alt = ALT_PRESSED;
	} else if (data == 0xB8) { //Left ALT released
		alt = 0;
	}

	shift |= shiftcode[data];
	shift ^= togglecode[data]; // set/clear the  shift  flag on SHIFT down/up.
	c = charcode[shift & (CTL | SHIFT)][data];
	if(shift & CAPSLOCK){
		if('a' <= c && c <= 'z')
			c += 'A' - 'a';
		else if('A' <= c && c <= 'Z')
			c += 'a' - 'A';
	}

	if(alt && c == 'c'){
		return KEY_ALT_C;
	}

	return c;
}

void
kbdintr(void)
{
	consoleintr(kbdgetc);
}
