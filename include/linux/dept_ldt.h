/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Lock Dependency Tracker
 *
 * Started by Byungchul Park <max.byungchul.park@gmail.com>:
 *
 *  Copyright (c) 2020 LG Electronics, Inc., Byungchul Park
 */

#ifndef __LINUX_DEPT_LDT_H
#define __LINUX_DEPT_LDT_H

#include <linux/dept.h>

#ifdef CONFIG_DEPT
#define LDT_EVT_L			1UL
#define LDT_EVT_R			2UL
#define LDT_EVT_W			1UL
#define LDT_EVT_RW			(LDT_EVT_R | LDT_EVT_W)
#define LDT_EVT_ALL			(LDT_EVT_L | LDT_EVT_RW)

#define ldt_init(m, k, su, n)		dept_map_init(m, k, su, n)
#define ldt_lock(m, sl, t, n, i)					\
	do {								\
		if (n)							\
			dept_ecxt_enter_nokeep(m);			\
		else if (t)						\
			dept_ecxt_enter(m, LDT_EVT_L, i, "trylock", "unlock", sl);\
		else {							\
			dept_wait(m, LDT_EVT_L, i, "lock", sl);		\
			dept_ecxt_enter(m, LDT_EVT_L, i, "lock", "unlock", sl);\
		}							\
	} while (0)

#define ldt_rlock(m, sl, t, n, i, q)					\
	do {								\
		if (n)							\
			dept_ecxt_enter_nokeep(m);			\
		else if (t)						\
			dept_ecxt_enter(m, LDT_EVT_R, i, "read_trylock", "read_unlock", sl);\
		else {							\
			dept_wait(m, q ? LDT_EVT_RW : LDT_EVT_W, i, "read_lock", sl);\
			dept_ecxt_enter(m, LDT_EVT_R, i, "read_lock", "read_unlock", sl);\
		}							\
	} while (0)

#define ldt_wlock(m, sl, t, n, i)					\
	do {								\
		if (n)							\
			dept_ecxt_enter_nokeep(m);			\
		else if (t)						\
			dept_ecxt_enter(m, LDT_EVT_W, i, "write_trylock", "write_unlock", sl);\
		else {							\
			dept_wait(m, LDT_EVT_RW, i, "write_lock", sl);	\
			dept_ecxt_enter(m, LDT_EVT_W, i, "write_lock", "write_unlock", sl);\
		}							\
	} while (0)

#define ldt_unlock(m, i)		dept_ecxt_exit(m, LDT_EVT_ALL, i)

#define ldt_downgrade(m, i)						\
	do {								\
		if (dept_ecxt_holding(m, LDT_EVT_W))			\
			dept_map_ecxt_modify(m, LDT_EVT_W, NULL, LDT_EVT_R, i, "downgrade", "read_unlock", -1);\
	} while (0)

#define ldt_set_class(m, n, k, sl, i)	dept_map_ecxt_modify(m, LDT_EVT_ALL, k, 0UL, i, "lock_set_class", "(any)unlock", sl)
#else /* !CONFIG_DEPT */
#define ldt_init(m, k, su, n)		do { (void)(k); } while (0)
#define ldt_lock(m, sl, t, n, i)	do { } while (0)
#define ldt_rlock(m, sl, t, n, i, q)	do { } while (0)
#define ldt_wlock(m, sl, t, n, i)	do { } while (0)
#define ldt_unlock(m, i)		do { } while (0)
#define ldt_downgrade(m, i)		do { } while (0)
#define ldt_set_class(m, n, k, sl, i)	do { } while (0)
#endif
#endif /* __LINUX_DEPT_LDT_H */
