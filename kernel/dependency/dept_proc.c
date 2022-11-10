// SPDX-License-Identifier: GPL-2.0
/*
 * Procfs knobs for Dept(DEPendency Tracker)
 *
 * Started by Byungchul Park <max.byungchul.park@gmail.com>:
 *
 *  Copyright (C) 2021 LG Electronics, Inc. , Byungchul Park
 */
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/dept.h>
#include "dept_internal.h"

static void *l_next(struct seq_file *m, void *v, loff_t *pos)
{
	/*
	 * XXX: Serialize list traversal if needed. The following might
	 * give a wrong information on contention.
	 */
	return seq_list_next(v, &dept_classes, pos);
}

static void *l_start(struct seq_file *m, loff_t *pos)
{
	/*
	 * XXX: Serialize list traversal if needed. The following might
	 * give a wrong information on contention.
	 */
	return seq_list_start_head(&dept_classes, *pos);
}

static void l_stop(struct seq_file *m, void *v)
{
}

static int l_show(struct seq_file *m, void *v)
{
	struct dept_class *fc = list_entry(v, struct dept_class, all_node);
	struct dept_dep *d;
	const char *prefix;

	if (v == &dept_classes) {
		seq_puts(m, "All classes:\n\n");
		return 0;
	}

	prefix = fc->sched_map ? "<sched> " : "";
	seq_printf(m, "[%p] %s%s\n", (void *)fc->key, prefix, fc->name);

	/*
	 * XXX: Serialize list traversal if needed. The following might
	 * give a wrong information on contention.
	 */
	list_for_each_entry(d, &fc->dep_head, dep_node) {
		struct dept_class *tc = d->wait->class;

		prefix = tc->sched_map ? "<sched> " : "";
		seq_printf(m, " -> [%p] %s%s\n", (void *)tc->key, prefix, tc->name);
	}
	seq_puts(m, "\n");

	return 0;
}

static const struct seq_operations dept_deps_ops = {
	.start	= l_start,
	.next	= l_next,
	.stop	= l_stop,
	.show	= l_show,
};

static int dept_stats_show(struct seq_file *m, void *v)
{
	int r;

	seq_puts(m, "Accumulated amount of memory used by pools:\n\n");
#define OBJECT(id, nr)							\
	r = atomic_read(&dept_pool[OBJECT_##id].acc_sz);		\
	seq_printf(m, "%s\t%d KB\n", #id, r / 1024);
	#include "dept_object.h"
#undef  OBJECT

	return 0;
}

static int __init dept_proc_init(void)
{
	proc_create_seq("dept_deps", S_IRUSR, NULL, &dept_deps_ops);
	proc_create_single("dept_stats", S_IRUSR, NULL, dept_stats_show);
	return 0;
}

__initcall(dept_proc_init);
