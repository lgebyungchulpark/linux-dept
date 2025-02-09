/* SPDX-License-Identifier: GPL-2.0 */
/*
 * DEPT(DEPendency Tracker) - runtime dependency tracker
 *
 * Started by Byungchul Park <max.byungchul.park@gmail.com>:
 *
 *  Copyright (c) 2020 LG Electronics, Inc., Byungchul Park
 */

#ifndef __LINUX_DEPT_H
#define __LINUX_DEPT_H

#ifdef CONFIG_DEPT

#include <linux/types.h>

struct task_struct;

#define DEPT_MAX_STACK_ENTRY		16
#define DEPT_MAX_WAIT_HIST		64
#define DEPT_MAX_ECXT_HELD		48

#define DEPT_MAX_SUBCLASSES		16
#define DEPT_MAX_SUBCLASSES_EVT		2
#define DEPT_MAX_SUBCLASSES_USR		(DEPT_MAX_SUBCLASSES / DEPT_MAX_SUBCLASSES_EVT)
#define DEPT_MAX_SUBCLASSES_CACHE	2

enum {
	DEPT_CXT_SIRQ = 0,
	DEPT_CXT_HIRQ,
	DEPT_CXT_IRQS_NR,
	DEPT_CXT_PROCESS = DEPT_CXT_IRQS_NR,
	DEPT_CXTS_NR
};

#define DEPT_SIRQF			(1UL << DEPT_CXT_SIRQ)
#define DEPT_HIRQF			(1UL << DEPT_CXT_HIRQ)

struct dept_ecxt;
struct dept_iecxt {
	struct dept_ecxt		*ecxt;
	int				enirq;
	/*
	 * for preventing to add a new ecxt
	 */
	bool				staled;
};

struct dept_wait;
struct dept_iwait {
	struct dept_wait		*wait;
	int				irq;
	/*
	 * for preventing to add a new wait
	 */
	bool				staled;
	bool				touched;
};

struct dept_class {
	union {
		struct llist_node	pool_node;
		struct {
			/*
			 * reference counter for object management
			 */
			atomic_t	ref;

			/*
			 * unique information about the class
			 */
			const char	*name;
			unsigned long	key;
			int		sub_id;

			/*
			 * for BFS
			 */
			unsigned int	bfs_gen;
			int		bfs_dist;
			struct dept_class *bfs_parent;

			/*
			 * for hashing this object
			 */
			struct hlist_node hash_node;

			/*
			 * for linking all classes
			 */
			struct list_head all_node;

			/*
			 * for associating its dependencies
			 */
			struct list_head dep_head;
			struct list_head dep_rev_head;

			/*
			 * for tracking IRQ dependencies
			 */
			struct dept_iecxt iecxt[DEPT_CXT_IRQS_NR];
			struct dept_iwait iwait[DEPT_CXT_IRQS_NR];

			/*
			 * classified by a map embedded in task_struct,
			 * not an explicit map
			 */
			bool		sched_map;
		};
	};
};

struct dept_key {
	union {
		/*
		 * Each byte-wise address will be used as its key.
		 */
		char			base[DEPT_MAX_SUBCLASSES];

		/*
		 * for caching the main class pointer
		 */
		struct dept_class	*classes[DEPT_MAX_SUBCLASSES_CACHE];
	};
};

struct dept_map {
	const char			*name;
	struct dept_key			*keys;

	/*
	 * subclass that can be set from user
	 */
	int				sub_u;

	/*
	 * It's local copy for fast access to the associated classes.
	 * Also used for dept_key for static maps.
	 */
	struct dept_key			map_key;

	/*
	 * wait timestamp associated to this map
	 */
	unsigned int			wgen;

	/*
	 * whether this map should be going to be checked or not
	 */
	bool				nocheck;
};

#define DEPT_MAP_INITIALIZER(n, k)					\
{									\
	.name = #n,							\
	.keys = (struct dept_key *)(k),					\
	.sub_u = 0,							\
	.map_key = { .classes = { NULL, } },				\
	.wgen = 0U,							\
	.nocheck = false,						\
}

struct dept_stack {
	union {
		struct llist_node	pool_node;
		struct {
			/*
			 * reference counter for object management
			 */
			atomic_t	ref;

			/*
			 * backtrace entries
			 */
			unsigned long	raw[DEPT_MAX_STACK_ENTRY];
			int nr;
		};
	};
};

struct dept_ecxt {
	union {
		struct llist_node	pool_node;
		struct {
			/*
			 * reference counter for object management
			 */
			atomic_t	ref;

			/*
			 * function that entered to this ecxt
			 */
			const char	*ecxt_fn;

			/*
			 * event function
			 */
			const char	*event_fn;

			/*
			 * associated class
			 */
			struct dept_class *class;

			/*
			 * flag indicating which IRQ has been
			 * enabled within the event context
			 */
			unsigned long	enirqf;

			/*
			 * where the IRQ-enabled happened
			 */
			unsigned long	enirq_ip[DEPT_CXT_IRQS_NR];
			struct dept_stack *enirq_stack[DEPT_CXT_IRQS_NR];

			/*
			 * where the event context started
			 */
			unsigned long	ecxt_ip;
			struct dept_stack *ecxt_stack;

			/*
			 * where the event triggered
			 */
			unsigned long	event_ip;
			struct dept_stack *event_stack;

			/*
			 * wait that this event ttwu
			 */
			struct dept_stack *ewait_stack;
		};
	};
};

struct dept_wait {
	union {
		struct llist_node	pool_node;
		struct {
			/*
			 * reference counter for object management
			 */
			atomic_t	ref;

			/*
			 * function causing this wait
			 */
			const char	*wait_fn;

			/*
			 * the associated class
			 */
			struct dept_class *class;

			/*
			 * which IRQ the wait was placed in
			 */
			unsigned long	irqf;

			/*
			 * where the IRQ wait happened
			 */
			unsigned long	irq_ip[DEPT_CXT_IRQS_NR];
			struct dept_stack *irq_stack[DEPT_CXT_IRQS_NR];

			/*
			 * where the wait happened
			 */
			unsigned long	wait_ip;
			struct dept_stack *wait_stack;

			/*
			 * whether this wait is for commit in scheduler
			 */
			bool		sched_sleep;

			/*
			 * whether a timeout is set
			 */
			bool				timeout;
		};
	};
};

struct dept_dep {
	union {
		struct llist_node	pool_node;
		struct {
			/*
			 * reference counter for object management
			 */
			atomic_t	ref;

			/*
			 * key data of dependency
			 */
			struct dept_ecxt *ecxt;
			struct dept_wait *wait;

			/*
			 * This object can be referred without dept_lock
			 * held but with IRQ disabled, e.g. for hash
			 * lookup. So deferred deletion is needed.
			 */
			struct rcu_head rh;

			/*
			 * for BFS
			 */
			struct list_head bfs_node;

			/*
			 * for hashing this object
			 */
			struct hlist_node hash_node;

			/*
			 * for linking to a class object
			 */
			struct list_head dep_node;
			struct list_head dep_rev_node;
		};
	};
};

struct dept_hash {
	/*
	 * hash table
	 */
	struct hlist_head		*table;

	/*
	 * size of the table e.i. 2^bits
	 */
	int				bits;
};

struct dept_pool {
	const char			*name;

	/*
	 * object size
	 */
	size_t				obj_sz;

	/*
	 * the remaining number of the object in spool
	 */
	int				obj_nr;

	/*
	 * the number of the object in spool
	 */
	int				tot_nr;

	/*
	 * accumulated amount of memory used by the object in byte
	 */
	atomic_t			acc_sz;

	/*
	 * offset of ->pool_node
	 */
	size_t				node_off;

	/*
	 * pointer to the pool
	 */
	void				*spool; /* static pool */
	void				*rpool; /* reserved pool */
	struct llist_head		boot_pool;
	struct llist_head __percpu	*lpool; /* local pool */
};

struct dept_ecxt_held {
	/*
	 * associated event context
	 */
	struct dept_ecxt		*ecxt;

	/*
	 * unique key for this dept_ecxt_held
	 */
	struct dept_map			*map;

	/*
	 * class of the ecxt of this dept_ecxt_held
	 */
	struct dept_class		*class;

	/*
	 * the wgen when the event context started
	 */
	unsigned int			wgen;

	/*
	 * subclass that only works in the local context
	 */
	int				sub_l;
};

struct dept_wait_hist {
	/*
	 * associated wait
	 */
	struct dept_wait		*wait;

	/*
	 * unique id of all waits system-wise until wrapped
	 */
	unsigned int			wgen;

	/*
	 * local context id to identify IRQ context
	 */
	unsigned int			ctxt_id;
};

struct dept_task {
	/*
	 * all event contexts that have entered and before exiting
	 */
	struct dept_ecxt_held		ecxt_held[DEPT_MAX_ECXT_HELD];
	int				ecxt_held_pos;

	/*
	 * ring buffer holding all waits that have happened
	 */
	struct dept_wait_hist		wait_hist[DEPT_MAX_WAIT_HIST];
	int				wait_hist_pos;

	/*
	 * sequential id to identify each context
	 */
	unsigned int			cxt_id[DEPT_CXTS_NR];

	/*
	 * for tracking IRQ-enabled points with cross-event
	 */
	unsigned int			wgen_enirq[DEPT_CXT_IRQS_NR];

	/*
	 * for keeping up-to-date IRQ-enabled points
	 */
	unsigned long			enirq_ip[DEPT_CXT_IRQS_NR];

	/*
	 * for reserving a current stack instance at each operation
	 */
	struct dept_stack		*stack;

	/*
	 * for preventing recursive call into DEPT engine
	 */
	int				recursive;

	/*
	 * for staging data to commit a wait
	 */
	struct dept_map			stage_m;
	bool				stage_sched_map;
	const char			*stage_w_fn;
	unsigned long			stage_ip;
	bool				stage_timeout;
	struct dept_stack		*stage_wait_stack;

	/*
	 * the number of missing ecxts
	 */
	int				missing_ecxt;

	/*
	 * for tracking IRQ-enable state
	 */
	bool				hardirqs_enabled;
	bool				softirqs_enabled;

	/*
	 * whether the current is on do_exit()
	 */
	bool				task_exit;

	/*
	 * whether the current is running __schedule()
	 */
	bool				in_sched;
};

/*
 * for subsystems that requires compact use of memory e.g. struct page
 */
struct dept_ext_wgen{
	unsigned int wgen;
};

#define DEPT_TASK_INITIALIZER(t)				\
{								\
	.wait_hist = { { .wait = NULL, } },			\
	.ecxt_held_pos = 0,					\
	.wait_hist_pos = 0,					\
	.cxt_id = { 0U },					\
	.wgen_enirq = { 0U },					\
	.enirq_ip = { 0UL },					\
	.stack = NULL,						\
	.recursive = 0,						\
	.stage_m = DEPT_MAP_INITIALIZER((t)->stage_m, NULL),	\
	.stage_sched_map = false,				\
	.stage_w_fn = NULL,					\
	.stage_ip = 0UL,					\
	.stage_timeout = false,					\
	.missing_ecxt = 0,					\
	.hardirqs_enabled = false,				\
	.softirqs_enabled = false,				\
	.task_exit = false,					\
	.in_sched = false,					\
}

extern void dept_on(void);
extern void dept_off(void);
extern void dept_init(void);
extern void dept_task_init(struct task_struct *t);
extern void dept_task_exit(struct task_struct *t);
extern void dept_free_range(void *start, unsigned int sz);
extern void dept_map_init(struct dept_map *m, struct dept_key *k, int sub_u, const char *n);
extern void dept_map_reinit(struct dept_map *m, struct dept_key *k, int sub_u, const char *n);
extern void dept_ext_wgen_init(struct dept_ext_wgen *ewg);
extern void dept_map_copy(struct dept_map *to, struct dept_map *from);

extern void dept_wait(struct dept_map *m, unsigned long w_f, unsigned long ip, const char *w_fn, int sub_l, long timeout);
extern void dept_stage_wait(struct dept_map *m, struct dept_key *k, unsigned long ip, const char *w_fn, long timeout);
extern void dept_request_event_wait_commit(void);
extern void dept_clean_stage(void);
extern void dept_stage_event(struct task_struct *t, unsigned long ip);
extern void dept_ecxt_enter(struct dept_map *m, unsigned long e_f, unsigned long ip, const char *c_fn, const char *e_fn, int sub_l);
extern bool dept_ecxt_holding(struct dept_map *m, unsigned long e_f);
extern void dept_request_event(struct dept_map *m, struct dept_ext_wgen *ewg);
extern void dept_event(struct dept_map *m, unsigned long e_f, unsigned long ip, const char *e_fn, struct dept_ext_wgen *ewg);
extern void dept_ecxt_exit(struct dept_map *m, unsigned long e_f, unsigned long ip);
extern void dept_sched_enter(void);
extern void dept_sched_exit(void);
extern void dept_update_cxt(void);

static inline void dept_ecxt_enter_nokeep(struct dept_map *m)
{
	dept_ecxt_enter(m, 0UL, 0UL, NULL, NULL, 0);
}

/*
 * for users who want to manage external keys
 */
extern void dept_key_init(struct dept_key *k);
extern void dept_key_destroy(struct dept_key *k);
extern void dept_map_ecxt_modify(struct dept_map *m, unsigned long e_f, struct dept_key *new_k, unsigned long new_e_f, unsigned long new_ip, const char *new_c_fn, const char *new_e_fn, int new_sub_l);

extern void dept_softirq_enter(void);
extern void dept_hardirq_enter(void);
extern void dept_softirqs_on_ip(unsigned long ip);
extern void dept_hardirqs_on(void);
extern void dept_softirqs_off(void);
extern void dept_hardirqs_off(void);
#else /* !CONFIG_DEPT */
struct dept_key  { };
struct dept_map  { };
struct dept_task { };
struct dept_ext_wgen { };

#define DEPT_MAP_INITIALIZER(n, k) { }
#define DEPT_TASK_INITIALIZER(t)   { }

#define dept_on()					do { } while (0)
#define dept_off()					do { } while (0)
#define dept_init()					do { } while (0)
#define dept_task_init(t)				do { } while (0)
#define dept_task_exit(t)				do { } while (0)
#define dept_free_range(s, sz)				do { } while (0)
#define dept_map_init(m, k, su, n)			do { (void)(n); (void)(k); } while (0)
#define dept_map_reinit(m, k, su, n)			do { (void)(n); (void)(k); } while (0)
#define dept_ext_wgen_init(wg)				do { } while (0)
#define dept_map_copy(t, f)				do { } while (0)

#define dept_wait(m, w_f, ip, w_fn, sl, t)		do { (void)(w_fn); } while (0)
#define dept_stage_wait(m, k, ip, w_fn, t)		do { (void)(k); (void)(w_fn); } while (0)
#define dept_request_event_wait_commit()		do { } while (0)
#define dept_clean_stage()				do { } while (0)
#define dept_stage_event(t, ip)				do { } while (0)
#define dept_ecxt_enter(m, e_f, ip, c_fn, e_fn, sl)	do { (void)(c_fn); (void)(e_fn); } while (0)
#define dept_ecxt_holding(m, e_f)			false
#define dept_request_event(m, wg)			do { } while (0)
#define dept_event(m, e_f, ip, e_fn, wg)		do { (void)(e_fn); } while (0)
#define dept_ecxt_exit(m, e_f, ip)			do { } while (0)
#define dept_sched_enter()				do { } while (0)
#define dept_sched_exit()				do { } while (0)
#define dept_update_cxt()				do { } while (0)
#define dept_ecxt_enter_nokeep(m)			do { } while (0)
#define dept_key_init(k)				do { (void)(k); } while (0)
#define dept_key_destroy(k)				do { (void)(k); } while (0)
#define dept_map_ecxt_modify(m, e_f, n_k, n_e_f, n_ip, n_c_fn, n_e_fn, n_sl) do { (void)(n_k); (void)(n_c_fn); (void)(n_e_fn); } while (0)

#define dept_softirq_enter()				do { } while (0)
#define dept_hardirq_enter()				do { } while (0)
#define dept_softirqs_on_ip(ip)				do { } while (0)
#define dept_hardirqs_on()				do { } while (0)
#define dept_softirqs_off()				do { } while (0)
#define dept_hardirqs_off()				do { } while (0)
#endif
#endif /* __LINUX_DEPT_H */
