/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Dept Single-event Dependency Tracker
 *
 * Started by Byungchul Park <max.byungchul.park@gmail.com>:
 *
 *  Copyright (c) 2020 LG Electronics, Inc., Byungchul Park
 */

#ifndef __LINUX_DEPT_SDT_H
#define __LINUX_DEPT_SDT_H

#ifdef CONFIG_DEPT

#include <linux/dept.h>

#define DEPT_SDT_MAP_INIT(dname)	{ .name = #dname }
#define DEFINE_DEPT_SDT(x)		\
	struct dept_map x = DEPT_SDT_MAP_INIT(x)

/*
 * SDT(Single-event Dependency Tracker) APIs
 *
 * In case that one dept_map instance maps to a single event, SDT APIs
 * can be used.
 */
#define sdt_map_init(m)							\
	do {								\
		static struct dept_key __key;				\
		dept_map_init(m, &__key, 0, #m);			\
	} while (0)
#define sdt_map_init_key(m, k)		dept_map_init(m, k, 0, #m)

#define sdt_wait(m)							\
	do {								\
		dept_asked_event(m);					\
		dept_wait(m, 1UL, _THIS_IP_, "wait", 0);		\
	} while (0)
#define sdt_ecxt_enter(m)		dept_ecxt_enter(m, 1UL, _THIS_IP_, "start", "event", 0)
#define sdt_event(m)			dept_event(m, 1UL, _THIS_IP_, "event")
#define sdt_ecxt_exit(m)		dept_ecxt_exit(m, _THIS_IP_)
#else /* !CONFIG_DEPT */
#define DEPT_SDT_MAP_INIT(dname)
#define DEFINE_DEPT_SDT(x)

#define sdt_map_init(m)					do { } while (0)
#define sdt_map_init_key(m, k)				do { (void)(k); } while (0)
#define sdt_wait(m)					do { } while (0)
#define sdt_ecxt_enter(m)				do { } while (0)
#define sdt_event(m)					do { } while (0)
#define sdt_ecxt_exit(m)				do { } while (0)
#endif
#endif /* __LINUX_DEPT_SDT_H */
