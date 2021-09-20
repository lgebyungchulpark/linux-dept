/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Dept(DEPendency Tracker) - runtime dependency tracker
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
#define DEPT_MAX_WAIT_HIST		16
#define DEPT_MAX_ECXT_HELD		48

#define DEPT_MAX_SUBCLASSES		16
#define DEPT_MAX_SUBCLASSES_EVT		2
#define DEPT_MAX_SUBCLASSES_USR		(DEPT_MAX_SUBCLASSES / DEPT_MAX_SUBCLASSES_EVT)
#define DEPT_MAX_SUBCLASSES_CACHE	2

#define DEPT_SIRQ			0
#define DEPT_HIRQ			1
#define DEPT_IRQS_NR			2
#define DEPT_SIRQF			(1UL << DEPT_SIRQ)
#define DEPT_HIRQF			(1UL << DEPT_HIRQ)

enum dept_type {
	DEPT_TYPE_NO_CHECK,
	DEPT_TYPE_SPIN,
	DEPT_TYPE_MUTEX,
	DEPT_TYPE_RWSEM,
	DEPT_TYPE_RW,
	DEPT_TYPE_WFC,
	DEPT_TYPE_SDT,
	DEPT_TYPE_OTHER,
};

struct dept_class {
	union {
		struct llist_node pool_node;
		struct {
			/*
			 * reference counter for object management
			 */
			atomic_t ref;

			/*
			 * unique information about the class
			 */
			const char *name;
			unsigned long key;
			int sub;

			/*
			 * for BFS
			 */
			unsigned int bfs_gen;
			int bfs_dist;
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
			int iwait_dist[DEPT_IRQS_NR];
			struct dept_ecxt *iecxt[DEPT_IRQS_NR];
			struct dept_wait *iwait[DEPT_IRQS_NR];
		};
	};
};

struct dept_stack {
	union {
		struct llist_node pool_node;
		struct {
			/*
			 * reference counter for object management
			 */
			atomic_t ref;

			/*
			 * backtrace entries
			 */
			unsigned long raw[DEPT_MAX_STACK_ENTRY];
			int nr;
		};
	};
};

struct dept_ecxt {
	union {
		struct llist_node pool_node;
		struct {
			/*
			 * reference counter for object management
			 */
			atomic_t ref;

			/*
			 * function that entered to this ecxt
			 */
			const char *ecxt_fn;

			/*
			 * event function
			 */
			const char *event_fn;

			/*
			 * associated class
			 */
			struct dept_class *class;

			/*
			 * flag indicating which IRQ has been
			 * enabled within the event context
			 */
			unsigned long enirqf;

			/*
			 * where the IRQ-enabled happened
			 */
			unsigned long enirq_ip[DEPT_IRQS_NR];
			struct dept_stack *enirq_stack[DEPT_IRQS_NR];

			/*
			 * where the event context started
			 */
			unsigned long ecxt_ip;
			struct dept_stack *ecxt_stack;

			/*
			 * where the event triggered
			 */
			unsigned long event_ip;
			struct dept_stack *event_stack;
		};
	};
};

struct dept_wait {
	union {
		struct llist_node pool_node;
		struct {
			/*
			 * reference counter for object management
			 */
			atomic_t ref;

			/*
			 * function causing this wait
			 */
			const char *wait_fn;

			/*
			 * the associated class
			 */
			struct dept_class *class;

			/*
			 * which IRQ the wait was placed in
			 */
			unsigned long irqf;

			/*
			 * where the wait happened
			 */
			unsigned long ip;
			struct dept_stack *stack;
		};
	};
};

struct dept_dep {
	union {
		struct llist_node pool_node;
		struct {
			/*
			 * reference counter for object management
			 */
			atomic_t ref;

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
	struct hlist_head *table;

	/*
	 * size of the table e.i. 2^bits
	 */
	int bits;
};

struct dept_pool {
	const char *name;

	/*
	 * object size
	 */
	size_t obj_sz;

	/*
	 * the number of the static array
	 */
	atomic_t obj_nr;

	/*
	 * offset of ->pool_node
	 */
	size_t node_off;

	/*
	 * pointer to the pool
	 */
	void *spool;
	struct llist_head boot_pool;
	struct llist_head __percpu *lpool;
};

struct dept_ecxt_held {
	/*
	 * associated event context
	 */
	struct dept_ecxt *ecxt;

	/*
	 * unique key for this dept_ecxt_held
	 */
	unsigned long key;

	/*
	 * the wgen when the event context started
	 */
	unsigned int wgen;

	/*
	 * whether the event context has been started along with a wait
	 */
	bool with_wait;

	/*
	 * for allowing user aware nesting
	 */
	int nest;
};

struct dept_wait_hist {
	/*
	 * associated wait
	 */
	struct dept_wait *wait;

	/*
	 * unique id of all waits system-wise until wrapped
	 */
	unsigned int wgen;

	/*
	 * local context id to identify IRQ context
	 */
	unsigned int ctxt_id;
};

struct dept_task {
	/*
	 * all event contexts that have entered and before exiting
	 */
	struct dept_ecxt_held ecxt_held[DEPT_MAX_ECXT_HELD];
	int ecxt_held_pos;

	/*
	 * ring buffer holding all waits that have happened
	 */
	struct dept_wait_hist wait_hist[DEPT_MAX_WAIT_HIST];
	int wait_hist_pos;

	/*
	 * sequencial id to identify each IRQ context
	 */
	unsigned int irq_id[DEPT_IRQS_NR];

	/*
	 * for tracking IRQ-enabled points with cross-event
	 */
	unsigned int wgen_enirq[DEPT_IRQS_NR];

	/*
	 * for keeping up-to-date IRQ-enabled points
	 */
	unsigned long enirq_ip[DEPT_IRQS_NR];

	/*
	 * current effective IRQ-enabled flag
	 */
	unsigned long eff_enirqf;

	/*
	 * for reserving a current stack instance at each operation
	 */
	struct dept_stack *stack;

	/*
	 * for preventing recursive call into Dept engine
	 */
	int recursive;

	/*
	 * for tracking IRQ-enable state
	 */
	bool hardirqs_enabled;
	bool softirqs_enabled;
};

struct dept_key {
	union {
		/*
		 * Each byte-wise address will be used as its key.
		 */
		char subkeys[DEPT_MAX_SUBCLASSES];

		/*
		 * for caching the main class pointer
		 */
		struct dept_class *classes[DEPT_MAX_SUBCLASSES_CACHE];
	};
};

struct dept_map {
	const char *name;
	struct dept_key *keys;
	int sub_usr;

	/*
	 * It's local copy for fast acces to the associated classes. And
	 * Also used for dept_key instance for statically defined map.
	 */
	struct dept_key keys_local;

	/*
	 * wait timestamp associated to this map
	 */
	unsigned int wgen;

	/*
	 * for handling the map differently according to the type
	 */
	enum dept_type type;
};

#define DEPT_TASK_INITIALIZER(t)					\
	.dept_task.wait_hist = { { .wait = NULL, } },			\
	.dept_task.ecxt_held_pos = 0,					\
	.dept_task.wait_hist_pos = 0,					\
	.dept_task.irq_id = { 0 },					\
	.dept_task.wgen_enirq = { 0 },					\
	.dept_task.enirq_ip = { 0 },					\
	.dept_task.recursive = 0,					\
	.dept_task.hardirqs_enabled = false,				\
	.dept_task.softirqs_enabled = false,

extern void dept_on(void);
extern void dept_off(void);
extern void dept_init(void);
extern void dept_task_init(struct task_struct *t);
extern void dept_task_exit(struct task_struct *t);
extern void dept_free_range(void *start, unsigned int sz);
extern void dept_map_init(struct dept_map *m, struct dept_key *k, int sub, const char *n, enum dept_type t);
extern void dept_map_reinit(struct dept_map *m, struct dept_key *k, int sub, const char *n);
extern void dept_map_nocheck(struct dept_map *m);

extern void dept_wait(struct dept_map *m, unsigned long w_f, unsigned long ip, const char *w_fn, int ne);
extern void dept_wait_ecxt_enter(struct dept_map *m, unsigned long w_f, unsigned long e_f, unsigned long ip, const char *w_fn, const char *c_fn, const char *e_fn, int ne);
extern void dept_ecxt_enter(struct dept_map *m, unsigned long e_f, unsigned long ip, const char *c_fn, const char *e_fn, int ne);
extern void dept_event(struct dept_map *m, unsigned long e_f, unsigned long ip, const char *e_fn);
extern void dept_ecxt_exit(struct dept_map *m, unsigned long ip);
extern struct dept_map *dept_top_map(void);

/*
 * for users who want to manage external keys
 */
extern void dept_key_init(struct dept_key *k);
extern void dept_key_destroy(struct dept_key *k);

#define DEPT_MAP_INIT(dname)	{ .name = #dname, .type = DEPT_TYPE_SDT }
#define DEFINE_DEPT_SDT(x)	struct dept_map x = DEPT_MAP_INIT(x)

/*
 * SDT(Simple version of Dependency Tracker) APIs
 *
 * In case that one dept_map instance maps to a single event, SDT APIs
 * can be used.
 */
#define sdt_map_init(m)							\
	do {								\
		static struct dept_key __key;				\
		dept_map_init(m, &__key, 0, #m, DEPT_TYPE_SDT);		\
	} while (0)
#define sdt_map_init_key(m, k)		dept_map_init(m, k, 0, #m)

#define sdt_wait(m)			dept_wait(m, 1UL, _THIS_IP_, "wait", 0)
#define sdt_wait_ecxt_enter(m)		dept_wait_ecxt_enter(m, 1UL, 1UL, _THIS_IP_, "wait", "start", "event", 0)
#define sdt_ecxt_enter(m)		dept_ecxt_enter(m, 1UL, _THIS_IP_, "start", "event", 0)
#define sdt_event(m)			dept_event(m, 1UL, _THIS_IP_, "event")
#define sdt_ecxt_exit(m)		dept_ecxt_exit(m, _THIS_IP_)
#else /* !CONFIG_DEPT */
struct dept_task { };
struct dept_key  { };
struct dept_map  { };

#define DEPT_TASK_INITIALIZER(t)

#define dept_on()					do { } while (0)
#define dept_off()					do { } while (0)
#define dept_init()					do { } while (0)
#define dept_task_init(t)				do { } while (0)
#define dept_task_exit(t)				do { } while (0)
#define dept_free_range(s, sz)				do { } while (0)
#define dept_map_init(m, k, s, n, t)			do { (void)(n); (void)(k); } while (0)
#define dept_map_reinit(m, k, s, n)			do { (void)(n); (void)(k); } while (0)
#define dept_map_nocheck(m)				do { } while (0)

#define dept_wait(m, w_f, ip, w_fn, ne)			do { } while (0)
#define dept_wait_ecxt_enter(m, w_f, e_f, ip, w_fn, c_fn, e_fn, ne) do { } while (0)
#define dept_ecxt_enter(m, e_f, ip, c_fn, e_fn, ne)	do { } while (0)
#define dept_event(m, e_f, ip, e_fn)			do { } while (0)
#define dept_ecxt_exit(m, ip)				do { } while (0)
#define dept_top_map()					NULL
#define dept_key_init(k)				do { (void)(k); } while (0)
#define dept_key_destroy(k)				do { (void)(k); } while (0)

#define DEFINE_DEPT_SDT(x)

#define sdt_map_init(m)					do { } while (0)
#define sdt_map_init_key(m, k)				do { (void)(k); } while (0)
#define sdt_wait(m)					do { } while (0)
#define sdt_wait_ecxt_enter(m)				do { } while (0)
#define sdt_ecxt_enter(m)				do { } while (0)
#define sdt_event(m)					do { } while (0)
#define sdt_ecxt_exit(m)				do { } while (0)
#endif
#endif /* __LINUX_DEPT_H */
