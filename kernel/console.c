// Console input and output.
// Input is from the keyboard or serial port.
// Output is written to the screen and serial port.

#include "types.h"
#include "defs.h"
#include "traps.h"
#include "spinlock.h"
#include "file.h"
#include "memlayout.h"
#include "proc.h"
#include "x86.h"
#include "printf.h"
#include "kbd.h"

#include <stdarg.h>

static void consputc(int);

static int panicked = 0;

static struct {
	struct spinlock lock;
	int locking;
} cons;


// Print to the console. only understands %d, %x, %p, %s.
void
cprintf(char *fmt, ...)
{
	int locking;
	va_list args;

	locking = cons.locking;
	if(locking)
		acquire(&cons.lock);

	if (fmt == 0)
		panic("null fmt");

	va_start(args, fmt);

	fnvprintf(consputc, fmt, args);

	va_end(args);

	if(locking)
		release(&cons.lock);
}

void
panic(char *s)
{
	int i;
	uint pcs[10];

	cli();
	cons.locking = 0;
	// use lapiccpunum so that we can call panic from mycpu()
	cprintf("lapicid %d: panic: ", lapicid());
	cprintf(s);
	cprintf("\n");
	getcallerpcs(&s, pcs);
	for(i=0; i<10; i++)
		cprintf(" %p", pcs[i]);
	panicked = 1; // freeze other CPU
	for(;;)
		;
}



#define BACKSPACE 0x100
#define CRTPORT 0x3d4
static ushort *crt = (ushort*)P2V(0xb8000);  // CGA memory

#define BLACK 0
#define BLUE 1
#define GREEN 2
#define CYAN 3
#define RED 4
#define MAGENTA 5
#define YELLOW 6
#define WHITE 7

// (bg) << 4 Takes the background color value and shifts its bits 4 places to the left. This moves the 4-bit bg value into the higher 4 bits. SHIFT: 0x1 << 4 becomes 0x10
// | (fg): Takes the foreground color value and performs a bitwise OR with the shifted background value. This places the 4-bit fg value into the lower 4 bits of the byte. OR: 0x10 | 0x0F becomes 0x1F
#define ATTR(bg, fg)  (((bg) << 4) | (fg))

// Menu rows: {foreground label, fg color, background label, bg color}
static struct {
	char *flabel;
	int fg;
	char *blabel;
	int bg;
} rows[4] = {
	{ "WHT", WHITE,   "BLK", BLACK   },
	{ "PUR", MAGENTA, "WHT", WHITE   },
	{ "RED", RED,     "AQU", CYAN    },
	{ "WHT", WHITE,   "YEL", YELLOW  }
};

static int menu_active = 0; // 0 - inactive, 1 - menu_active
static int menu_selected = 0;
static uchar console_color = ATTR(BLACK, WHITE); // default colors

static void
vga_putchar(int row, int col, char c, uchar attr)
{
	int pos = row * 80 + col;
	crt[pos] = (attr << 8) | (uchar) c;
}


static void
draw_menu(int show, int selected)
{

	// Draw starting at screen row 1, col 60 (top-right area)
	int start_row = 1, start_col = 60;
	uchar menu_color = ATTR(WHITE, BLACK);
	uchar def = ATTR(BLACK, WHITE);


	if(!show){
		// Erase: write spaces with default attribute over menu area
		for(int r = 0; r < 6; r++) {
			for(int c = 0; c < 12; c++) {
				vga_putchar(start_row + r, start_col + c, ' ', def);
			}
		}
		return;
	}

	// Draw top border:
	for(int i = 0; i < 9; i ++){
		vga_putchar(start_row, start_col + i, '-', menu_color);
	}


	for(int i = 0; i < 4; i++){
		int r = start_row + 1 + i;
		uchar row_color = (i == selected) ? ATTR(GREEN, BLACK) : ATTR(WHITE, BLACK);

		vga_putchar(r, start_col, '|', menu_color);

		// Foreground swatch
		char *fl = rows[i].flabel;
		vga_putchar(r, start_col + 1, fl[0], row_color);
		vga_putchar(r, start_col + 2, fl[1], row_color);
		vga_putchar(r, start_col + 3, fl[2], row_color);

		vga_putchar(r, start_col + 4, ' ', row_color);

		// Background swatch
		char *bl = rows[i].blabel;
		vga_putchar(r, start_col + 5,  bl[0], row_color);
		vga_putchar(r, start_col + 6,  bl[1], row_color);
		vga_putchar(r, start_col + 7,  bl[2], row_color);

		vga_putchar(r, start_col + 8, '|', menu_color);
	}

	// Draw top border:
	for(int i = 0; i < 9; i ++){
		vga_putchar(start_row + 5, start_col + i, '-', menu_color);
	}


}


static void
cgaputc(int c)
{
	int pos;

	// Cursor position: col + 80*row.
	outb(CRTPORT, 14);
	pos = inb(CRTPORT+1) << 8;
	outb(CRTPORT, 15);
	pos |= inb(CRTPORT+1);

	if(c == '\n')
		pos += 80 - pos%80;
	else if(c == BACKSPACE){
		if(pos > 0) --pos;
	} else
		//use console_color instead of 0x0700
		crt[pos++] = (c&0xff) | ((uint)console_color<<8);  // black on white

	if(pos < 0 || pos > 25*80)
		panic("pos under/overflow");

	if((pos/80) >= 24){  // Scroll up.
		memmove(crt, crt+80, sizeof(crt[0])*23*80);
		pos -= 80;
		memset(crt+pos, 0, sizeof(crt[0])*(24*80 - pos));
	}

	outb(CRTPORT, 14);
	outb(CRTPORT+1, pos>>8);
	outb(CRTPORT, 15);
	outb(CRTPORT+1, pos);
	crt[pos] = ' ' | 0x0700;
}

void
consputc(int c)
{
	if(panicked){
		cli();
		for(;;)
			;
	}

	if(c == BACKSPACE){
		uartputc('\b'); uartputc(' '); uartputc('\b');
	} else
		uartputc(c);
	cgaputc(c);
}

#define INPUT_BUF 128
struct {
	char buf[INPUT_BUF];
	uint r;  // Read index
	uint w;  // Write index
	uint e;  // Edit index
} input;

#define C(x)  ((x)-'@')  // Control-x

void
consoleintr(int (*getc)(void))
{
	int c, doprocdump = 0;

	acquire(&cons.lock);
	while((c = getc()) >= 0){

		if(c == KEY_ALT_C) {
			menu_active = !menu_active;
			draw_menu(menu_active, menu_selected);
			continue; // consume the key, do nothing else
		}
		if(menu_active){
			if(c == 'w' || c == 'W'){
				menu_selected = (menu_selected - 1 + 4) % 4;  // wraps to bottom (0 - 1 + 4) % 4 = 3
				draw_menu(1, menu_selected);
			} else if(c == 's' || c == 'S'){
				menu_selected = (menu_selected + 1) % 4;       // (3 + 1) % 4 = 0 wraps to top
				draw_menu(1, menu_selected);
			} else if (c == '\n' || c == '\r'){
				console_color = ATTR(rows[menu_selected].bg, rows[menu_selected].fg);
				menu_active = 0;
				draw_menu(0, menu_selected);
			}
			continue; // discart any other key
		}

		switch(c){
		case C('P'):  // Process listing.
			// procdump() locks cons.lock indirectly; invoke later
			doprocdump = 1;
			break;
		case C('U'):  // Kill line.
			while(input.e != input.w &&
			      input.buf[(input.e-1) % INPUT_BUF] != '\n'){
				input.e--;
				consputc(BACKSPACE);
			}
			break;
		case C('H'): case '\x7f':  // Backspace
			if(input.e != input.w){
				input.e--;
				consputc(BACKSPACE);
			}
			break;
		default:
			if(c != 0 && input.e-input.r < INPUT_BUF){
				c = (c == '\r') ? '\n' : c;
				input.buf[input.e++ % INPUT_BUF] = c;
				consputc(c);
				if(c == '\n' || c == C('D') || input.e == input.r+INPUT_BUF){
					input.w = input.e;
					wakeup(&input.r);
				}
			}
			break;
		}
	}
	release(&cons.lock);
	if(doprocdump) {
		procdump();  // now call procdump() wo. cons.lock held
	}
}

int
consoleread(struct inode *ip, char *dst, int n)
{
	uint target;
	int c;

	iunlock(ip);
	target = n;
	acquire(&cons.lock);
	while(n > 0){
		while(input.r == input.w){
			if(myproc()->killed){
				release(&cons.lock);
				ilock(ip);
				return -1;
			}
			sleep(&input.r, &cons.lock);
		}
		c = input.buf[input.r++ % INPUT_BUF];
		if(c == C('D')){  // EOF
			if(n < target){
				// Save ^D for next time, to make sure
				// caller gets a 0-byte result.
				input.r--;
			}
			break;
		}
		*dst++ = c;
		--n;
		if(c == '\n')
			break;
	}
	release(&cons.lock);
	ilock(ip);

	return target - n;
}

int
consolewrite(struct inode *ip, char *buf, int n)
{
	int i;

	iunlock(ip);
	acquire(&cons.lock);
	for(i = 0; i < n; i++)
		consputc(buf[i] & 0xff);
	release(&cons.lock);
	ilock(ip);

	return n;
}

void
consoleinit(void)
{
	initlock(&cons.lock, "console");

	devsw[CONSOLE].write = consolewrite;
	devsw[CONSOLE].read = consoleread;
	cons.locking = 1;

	ioapicenable(IRQ_KBD, 0);
}

