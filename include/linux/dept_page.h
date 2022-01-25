#ifndef __LINUX_DEPT_PAGE_H
#define __LINUX_DEPT_PAGE_H

#ifdef CONFIG_DEPT
#include <linux/dept.h>

extern struct page_ext_operations dept_pglocked_ops;
extern struct page_ext_operations dept_pgwriteback_ops;
extern struct dept_map_common pglocked_mc;
extern struct dept_map_common pgwriteback_mc;

extern void dept_page_init(void);
extern struct dept_map_each *get_pglocked_me(struct page *page);
extern struct dept_map_each *get_pgwriteback_me(struct page *page);

#define dept_pglocked_wait(p)					\
do {								\
	struct dept_map_each *me = get_pglocked_me(p);		\
	if (likely(me))						\
		dept_wait_split_map(me, &pglocked_mc, _RET_IP_, \
				    __func__, 0);		\
} while (0)

#define dept_pglocked_set_bit(p)				\
do {								\
	struct dept_map_each *me = get_pglocked_me(p);		\
	if (likely(me))						\
		dept_asked_event_split_map(me, &pglocked_mc);	\
} while (0)

#define dept_pglocked_event(p)					\
do {								\
	struct dept_map_each *me = get_pglocked_me(p);		\
	if (likely(me))						\
		dept_event_split_map(me, &pglocked_mc, _RET_IP_,\
				     __func__);			\
} while (0)

#define dept_pgwriteback_wait(p)				\
do {								\
	struct dept_map_each *me = get_pgwriteback_me(p);	\
	if (likely(me))						\
		dept_wait_split_map(me, &pgwriteback_mc, _RET_IP_,\
				    __func__, 0);		\
} while (0)

#define dept_pgwriteback_set_bit(p)				\
do {								\
	struct dept_map_each *me = get_pgwriteback_me(p);	\
	if (likely(me))						\
		dept_asked_event_split_map(me, &pgwriteback_mc);\
} while (0)

#define dept_pgwriteback_event(p)				\
do {								\
	struct dept_map_each *me = get_pgwriteback_me(p);	\
	if (likely(me))						\
		dept_event_split_map(me, &pgwriteback_mc, _RET_IP_,\
				     __func__);			\
} while (0)
#else
#define dept_page_init()		do { } while (0)
#define dept_pglocked_wait(p)		do { } while (0)
#define dept_pglocked_set_bit(p)	do { } while (0)
#define dept_pglocked_event(p)		do { } while (0)
#define dept_pgwriteback_wait(p)	do { } while (0)
#define dept_pgwriteback_set_bit(p)	do { } while (0)
#define dept_pgwriteback_event(p)	do { } while (0)
#endif

#endif /* __LINUX_DEPT_PAGE_H */
