/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_DEPT_PAGE_H
#define __LINUX_DEPT_PAGE_H

#ifdef CONFIG_DEPT
#include <linux/dept.h>

extern struct page_ext_operations dept_pglocked_ops;
extern struct page_ext_operations dept_pgwriteback_ops;
extern struct dept_map_common pglocked_mc;
extern struct dept_map_common pgwriteback_mc;

extern void dept_page_init(void);
extern struct dept_map_each *get_pglocked_me(struct page *page, void **cookie);
extern struct dept_map_each *get_pgwriteback_me(struct page *page, void **cookie);
extern void put_pglocked_me(void *cookie);
extern void put_pgwriteback_me(void *cookie);

#define dept_pglocked_wait(f)					\
do {								\
	void *cookie;						\
	struct dept_map_each *me = get_pglocked_me(&(f)->page, &cookie);\
								\
	if (likely(me))						\
		dept_wait_split_map(me, &pglocked_mc, _RET_IP_, __func__, 0, true);\
								\
	put_pglocked_me(cookie);				\
} while (0)

#define dept_pglocked_set_bit(f)				\
do {								\
	void *cookie;						\
	struct dept_map_each *me = get_pglocked_me(&(f)->page, &cookie);\
								\
	if (likely(me))						\
		dept_ask_event_split_map(me, &pglocked_mc);	\
								\
	put_pglocked_me(cookie);				\
} while (0)

#define dept_pglocked_event(f)					\
do {								\
	void *cookie;						\
	struct dept_map_each *me = get_pglocked_me(&(f)->page, &cookie);\
								\
	if (likely(me))						\
		dept_event_split_map(me, &pglocked_mc, _RET_IP_, __func__);\
								\
	put_pglocked_me(cookie);				\
} while (0)

#define dept_pgwriteback_wait(f)				\
do {								\
	void *cookie;						\
	struct dept_map_each *me = get_pgwriteback_me(&(f)->page, &cookie);\
								\
	if (likely(me))						\
		dept_wait_split_map(me, &pgwriteback_mc, _RET_IP_, __func__, 0, true);\
								\
	put_pgwriteback_me(cookie);				\
} while (0)

#define dept_pgwriteback_set_bit(f)				\
do {								\
	void *cookie;						\
	struct dept_map_each *me = get_pgwriteback_me(&(f)->page, &cookie);\
								\
	if (likely(me))						\
		dept_ask_event_split_map(me, &pgwriteback_mc);	\
								\
	put_pgwriteback_me(cookie);				\
} while (0)

#define dept_pgwriteback_event(f)				\
do {								\
	void *cookie;						\
	struct dept_map_each *me = get_pgwriteback_me(&(f)->page, &cookie);\
								\
	if (likely(me))						\
		dept_event_split_map(me, &pgwriteback_mc, _RET_IP_, __func__);\
								\
	put_pgwriteback_me(cookie);				\
} while (0)
#else
#define dept_page_init()		do { } while (0)
#define dept_pglocked_wait(f)		do { } while (0)
#define dept_pglocked_set_bit(f)	do { } while (0)
#define dept_pglocked_event(f)		do { } while (0)
#define dept_pgwriteback_wait(f)	do { } while (0)
#define dept_pgwriteback_set_bit(f)	do { } while (0)
#define dept_pgwriteback_event(f)	do { } while (0)
#endif

#endif /* __LINUX_DEPT_PAGE_H */
