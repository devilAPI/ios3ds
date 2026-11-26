/* Link-time stand-in for 5H11a's MBX2D.framework: the names egl2.c
 * interposes. Never installed. SPDX-License-Identifier: GPL-3.0-or-later */
#define S(n) void n(void); void n(void) {}
S(mbx2DSwapNotification) S(mbx2DFlush)
