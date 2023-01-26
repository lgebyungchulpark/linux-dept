/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Single-event Dependency Tracker
 *
 * Started by Byungchul Park <max.byungchul.park@gmail.com>:
 *
 *  Copyright (c) 2020 LG Electronics, Inc., Byungchul Park
 */

#ifndef __LINUX_DEPT_SDT_H
#define __LINUX_DEPT_SDT_H

#include <linux/kernel.h>
#include <linux/dept.h>

#ifdef CONFIG_DEPT
#define sdt_map_init(m)							\
	do {								\
		static struct dept_key __key;				\
		dept_map_init(m, &__key, 0, #m);			\
	} while (0)

#define sdt_map_init_key(m, k)		dept_map_init(m, k, 0, #m)

#define sdt_wait_timeout(m, t)						\
	do {								\
		dept_request_event(m, NULL);				\
		dept_wait(m, 1UL, _THIS_IP_, __func__, 0, t);		\
	} while (0)
#define sdt_wait(m) sdt_wait_timeout(m, -1L)

/*
 * sdt_might_sleep() and its family will be committed in __schedule()
 * when it actually gets to __schedule(). Both dept_request_event() and
 * dept_wait() will be performed on the commit.
 */

/*
 * Use the code location as the class key if an explicit map is not used.
 */
#define sdt_might_sleep_start_timeout(m, t)				\
	do {								\
		struct dept_map *__m = m;				\
		static struct dept_key __key;				\
		dept_stage_wait(__m, __m ? NULL : &__key, _THIS_IP_, __func__, t);\
	} while (0)
#define sdt_might_sleep_start(m) sdt_might_sleep_start_timeout(m, -1L)

#define sdt_might_sleep_end()		dept_clean_stage()

#define sdt_ecxt_enter(m)		dept_ecxt_enter(m, 1UL, _THIS_IP_, "start", "event", 0)
#define sdt_event(m)			dept_event(m, 1UL, _THIS_IP_, __func__, NULL)
#define sdt_ecxt_exit(m)		dept_ecxt_exit(m, 1UL, _THIS_IP_)
#else /* !CONFIG_DEPT */
#define sdt_map_init(m)			do { } while (0)
#define sdt_map_init_key(m, k)		do { (void)(k); } while (0)
#define sdt_wait_timeout(m, t)		do { } while (0)
#define sdt_wait(m)			do { } while (0)
#define sdt_might_sleep_start_timeout(m, t) do { } while (0)
#define sdt_might_sleep_start(m)	do { } while (0)
#define sdt_might_sleep_end()		do { } while (0)
#define sdt_ecxt_enter(m)		do { } while (0)
#define sdt_event(m)			do { } while (0)
#define sdt_ecxt_exit(m)		do { } while (0)
#endif
#endif /* __LINUX_DEPT_SDT_H */
