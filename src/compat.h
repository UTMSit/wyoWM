#ifndef WYO_COMPAT_H
#define WYO_COMPAT_H

#ifdef __NetBSD__

#define KEY_ESC   1
#define KEY_F1    59
#define KEY_F2    60
#define KEY_F3    61
#define KEY_F4    62
#define KEY_F5    63
#define KEY_F6    64
#define KEY_F7    65
#define KEY_F8    66
#define KEY_F9    67
#define KEY_F10   68
#define KEY_F11   87
#define KEY_F12   88
#define BTN_LEFT  0x110
#define BTN_RIGHT 0x111

#else

#include <linux/input-event-codes.h>

#endif

#endif
