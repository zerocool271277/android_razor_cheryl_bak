/*
 * Budget Fair Queueing (BFQ) disk scheduler.
 *
 * Based on ideas and code from CFQ:
 * Copyright (C) 2003 Jens Axboe <axboe@kernel.dk>
 *
 * Copyright (C) 2008 Fabio Checconi <fabio@gandalf.sssup.it>
 *		      Paolo Valente <paolo.valente@unimore.it>
 *
 * Copyright (C) 2015 Paolo Valente <paolo.valente@unimore.it>
 *
 * Copyright (C) 2016 Paolo Valente <paolo.valente@linaro.org>
 *
 * Licensed under the GPL-2 as detailed in the accompanying COPYING.BFQ
 * file.
 *
 * BFQ is a proportional-share storage-I/O scheduling algorithm based
 * on the slice-by-slice service scheme of CFQ. But BFQ assigns
 * budgets, measured in number of sectors, to processes instead of
 * time slices. The device is not granted to the in-service process
 * for a given time slice, but until it has exhausted its assigned
 * budget. This change from the time to the service domain enables BFQ
 * to distribute the device throughput among processes as desired,
 * without any distortion due to throughput fluctuations, or to device
 * internal queueing. BFQ uses an ad hoc internal scheduler, called
 * B-WF2Q+, to schedule processes according to their budgets. More
 * precisely, BFQ schedules queues associated with processes. Thanks to
 * the accurate policy of B-WF2Q+, BFQ can afford to assign high
 * budgets to I/O-bound processes issuing sequential requests (to
 * boost the throughput), and yet guarantee a low latency to
 * interactive and soft real-time applications.
 *
 * BFQ is described in [1], where also a reference to the initial, more
 * theoretical paper on BFQ can be found. The interested reader can find
 * in the latter paper full details on the main algorithm, as well as
 * formulas of the guarantees and formal proofs of all the properties.
 * With respect to the version of BFQ presented in these papers, this
 * implementation adds a few more heuristics, such as the one that
 * guarantees a low latency to soft real-time applications, and a
 * hierarchical extension based on H-WF2Q+.
 *
 * B-WF2Q+ is based on WF2Q+, that is described in [2], together with
 * H-WF2Q+, while the augmented tree used to implement B-WF2Q+ with O(log N)
 * complexity derives from the one introduced with EEVDF in [3].
 *
 * [1] P. Valente and M. Andreolini, ``Improving Application Responsiveness
 *     with the BFQ Disk I/O Scheduler'',
 *     Proceedings of the 5th Annual International Systems and Storage
 *     Conference (SYSTOR '12), June 2012.
 *
 * http://algogroup.unimo.it/people/paolo/disk_sched/bf1-v1-suite-results.pdf
 *
 * [2] Jon C.R. Bennett and H. Zhang, ``Hierarchical Packet Fair Queueing
 *     Algorithms,'' IEEE/ACM Transactions on Networking, 5(5):675-689,
 *     Oct 1997.
 *
 * http://www.cs.cmu.edu/~hzhang/papers/TON-97-Oct.ps.gz
 *
 * [3] I. Stoica and H. Abdel-Wahab, ``Earliest Eligible Virtual Deadline
 *     First: A Flexible and Accurate Mechanism for Proportional Share
 *     Resource Allocation,'' technical report.
 *
 * http://www.cs.berkeley.edu/~istoica/papers/eevdf-tr-95.pdf
 */
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/blkdev.h>
#include <linux/cgroup.h>
#include <linux/elevator.h>
#include <linux/jiffies.h>
#include <linux/rbtree.h>
#include <linux/ioprio.h>
#include "blk.h"
#include "bfq.h"

/* Expiration time of sync (0) and async (1) requests, in ns. */
static const u64 bfq_fifo_expire[2] = { NSEC_PER_SEC / 4, NSEC_PER_SEC / 8 };

/* Maximum backwards seek, in KiB. */
static const int bfq_back_max = 16 * 1024;

/* Penalty of a backwards seek, in number of sectors. */
static const int bfq_back_penalty = 2;

/* Idling period duration, in ns. */
static u32 bfq_slice_idle = NSEC_PER_SEC / 125;

/* Minimum number of assigned budgets for which stats are safe to compute. */
static const int bfq_stats_min_budgets = 194;

/* Default maximum budget values, in sectors and number of requests. */
static const int bfq_default_max_budget = 16 * 1024;

/*
 * Async to sync throughput distribution is controlled as follows:
 * when an async request is served, the entity is charged the number
 * of sectors of the request, multiplied by the factor below
 */
static const int bfq_async_charge_factor = 10;

/* Default timeout values, in jiffies, approximating CFQ defaults. */
static const int bfq_timeout = HZ / 8;

struct kmem_cache *bfq_pool;

/* Below this threshold (in ns), we consider thinktime immediate. */
#define BFQ_MIN_TT		(2 * NSEC_PER_MSEC)

/* hw_tag detection: parallel requests threshold and min samples needed. */
#define BFQ_HW_QUEUE_THRESHOLD	4
#define BFQ_HW_QUEUE_SAMPLES	32

#define BFQQ_SEEK_THR		(sector_t)(8 * 100)
#define BFQQ_CLOSE_THR		(sector_t)(8 * 1024)
#define BFQQ_SEEKY(bfqq)	(hweight32(bfqq->seek_history) > 32/8)

/* Min number of samples required to perform peak-rate update */
#define BFQ_RATE_MIN_SAMPLES	32
/* Min observation time interval required to perform a peak-rate update (us) */
#define BFQ_RATE_MIN_INTERVAL	300*USEC_PER_MSEC
/* Target observation time interval for a peak-rate update (us) */
#define BFQ_RATE_REF_INTERVAL	USEC_PER_SEC

/* Shift used for peak rate fixed precision calculations. */
#define BFQ_RATE_SHIFT		16

/*
 * By default, BFQ computes the duration of the weight raising for
 * interactive applications automatically, using the following formula:
 * duration = (R / r) * T, where r is the peak rate of the device, and
 * R and T are two reference parameters.
 * In particular, R is the peak rate of the reference device (see below),
 * and T is a reference time: given the systems that are likely to be
 * installed on the reference device according to its speed class, T is
 * about the maximum time needed, under BFQ and while reading two files in
 * parallel, to load typical large applications on these systems.
 * In practice, the slower/faster the device at hand is, the more/less it
 * takes to load applications with respect to the reference device.
 * Accordingly, the longer/shorter BFQ grants weight raising to interactive
 * applications.
 *
 * BFQ uses four different reference pairs (R, T), depending on:
 * . whether the device is rotational or non-rotational;
 * . whether the device is slow, such as old or portable HDDs, as well as
 *   SD cards, or fast, such as newer HDDs and SSDs.
 *
 * The device's speed class is dynamically (re)detected in
 * bfq_update_peak_rate() every time the estimated peak rate is updated.
 *
 * In the following definitions, R_slow[0]/R_fast[0] and
 * T_slow[0]/T_fast[0] are the reference values for a slow/fast
 * rotational device, whereas R_slow[1]/R_fast[1] and
 * T_slow[1]/T_fast[1] are the reference values for a slow/fast
 * non-rotational device. Finally, device_speed_thresh are the
 * thresholds used to switch between speed classes. The reference
 * rates are not the actual peak rates of the devices used as a
 * reference, but slightly lower values. The reason for using these
 * slightly lower values is that the peak-rate estimator tends to
 * yield slightly lower values than the actual peak rate (it can yield
 * the actual peak rate only if there is only one process doing I/O,
 * and the process does sequential I/O).
 *
 * Both the reference peak rates and the thresholds are measured in
 * sectors/usec, left-shifted by BFQ_RATE_SHIFT.
 */
static int R_slow[2] = {1000, 10700};
static int R_fast[2] = {14000, 33000};
/*
 * To improve readability, a conversion function is used to initialize the
 * following arrays, which entails that they can be initialized only in a
 * function.
 */
static int T_slow[2];
static int T_fast[2];
static int device_speed_thresh[2];

#define BFQ_SERVICE_TREE_INIT	((struct bfq_service_tree)		\
				{ RB_ROOT, RB_ROOT, NULL, NULL, 0, 0 })

#define RQ_BIC(rq)		((struct bfq_io_cq *) (rq)->elv.priv[0])
#define RQ_BFQQ(rq)		((rq)->elv.priv[1])

static void bfq_schedule_dispatch(struct bfq_data *bfqd);

#include "bfq-ioc.c"
#include "bfq-sched.c"
#include "bfq-cgroup.c"

#define bfq_class_idle(bfqq)	((bfqq)->ioprio_class == IOPRIO_CLASS_IDLE)
#define bfq_class_rt(bfqq)	((bfqq)->ioprio_class == IOPRIO_CLASS_RT)

#define bfq_sample_valid(samples)	((samples) > 80)

/*
 * We regard a request as SYNC, if either it's a read or has the SYNC bit
 * set (in which case it could also be a direct WRITE).
 */
static int bfq_bio_sync(struct bio *bio)
{
	if (bio_data_dir(bio) == READ || (bio->bi_rw & REQ_SYNC))
		return 1;

	return 0;
}

/*
 * Scheduler run of queue, if there are requests pending and no one in the
 * driver that will restart queueing.
 */
static void bfq_schedule_dispatch(struct bfq_data *bfqd)
{
	if (bfqd->queued != 0) {
		bfq_log(bfqd, "schedule dispatch");
		kblockd_schedule_work(&bfqd->unplug_work);
	}
}

/*
 * Lifted from AS - choose which of rq1 and rq2 that is best served now.
 * We choose the request that is closesr to the head right now.  Distance
 * behind the head is penalized and only allowed to a certain extent.
 */
static struct request *bfq_choose_req(struct bfq_data *bfqd,
				      struct request *rq1,
				      struct request *rq2,
				      sector_t last)
{
	sector_t s1, s2, d1 = 0, d2 = 0;
	unsigned long back_max;
#define BFQ_RQ1_WRAP	0x01 /* request 1 wraps */
#define BFQ_RQ2_WRAP	0x02 /* request 2 wraps */
	unsigned wrap = 0; /* bit mask: requests behind the disk head? */

	if (!rq1 || rq1 == rq2)
		return rq2;
	if (!rq2)
		return rq1;

	if (rq_is_sync(rq1) && !rq_is_sync(rq2))
		return rq1;
	else if (rq_is_sync(rq2) && !rq_is_sync(rq1))
		return rq2;
	if ((rq1->cmd_flags & REQ_META) && !(rq2->cmd_flags & REQ_META))
		return rq1;
	else if ((rq2->cmd_flags & REQ_META) && !(rq1->cmd_flags & REQ_META))
		return rq2;

	s1 = blk_rq_pos(rq1);
	s2 = blk_rq_pos(rq2);

	/*
	 * By definition, 1KiB is 2 sectors.
	 */
	back_max = bfqd->bfq_back_max * 2;

	/*
	 * Strict one way elevator _except_ in the case where we allow
	 * short backward seeks which are biased as twice the cost of a
	 * similar forward seek.
	 */
	if (s1 >= last)
		d1 = s1 - last;
	else if (s1 + back_max >= last)
		d1 = (last - s1) * bfqd->bfq_back_penalty;
	else
		wrap |= BFQ_RQ1_WRAP;

	if (s2 >= last)
		d2 = s2 - last;
	else if (s2 + back_max >= last)
		d2 = (last - s2) * bfqd->bfq_back_penalty;
	else
		wrap |= BFQ_RQ2_WRAP;

	/* Found required data */

	/*
	 * By doing switch() on the bit mask "wrap" we avoid having to
	 * check two variables for all permutations: --> faster!
	 */
	switch (wrap) {
	case 0: /* common case for CFQ: rq1 and rq2 not wrapped */
		if (d1 < d2)
			return rq1;
		else if (d2 < d1)
			return rq2;
		else {
			if (s1 >= s2)
				return rq1;
			else
				return rq2;
		}

	case BFQ_RQ2_WRAP:
		return rq1;
	case BFQ_RQ1_WRAP:
		return rq2;
	case (BFQ_RQ1_WRAP|BFQ_RQ2_WRAP): /* both rqs wrapped */
	default:
		/*
		 * Since both rqs are wrapped,
		 * start with the one that's further behind head
		 * (--> only *one* back seek required),
		 * since back seek takes more time than forward.
		 */
		if (s1 <= s2)
			return rq1;
		else
			return rq2;
	}
}

static struct bfq_queue *
bfq_rq_pos_tree_lookup(struct bfq_data *bfqd, struct rb_root *root,
		     sector_t sector, struct rb_node **ret_parent,
		     struct rb_node ***rb_link)
{
	struct rb_node **p, *parent;
	struct bfq_queue *bfqq = NULL;

	parent = NULL;
	p = &root->rb_node;
	while (*p) {
		struct rb_node **n;

		parent = *p;
		bfqq = rb_entry(parent, struct bfq_queue, pos_node);

		/*
		 * Sort strictly based on sector. Smallest to the left,
		 * largest to the right.
		 */
		if (sector > blk_rq_pos(bfqq->next_rq))
			n = &(*p)->rb_right;
		else if (sector < blk_rq_pos(bfqq->next_rq))
			n = &(*p)->rb_left;
		else
			break;
		p = n;
		bfqq = NULL;
	}

	*ret_parent = parent;
	if (rb_link)
		*rb_link = p;

	bfq_log(bfqd, "rq_pos_tree_lookup %llu: returning %d",
		(long long unsigned)sector,
		bfqq ? bfqq->pid : 0);

	return bfqq;
}

static void bfq_pos_tree_add_move(struct bfq_data *bfqd, struct bfq_queue *bfqq)
{
	struct rb_node **p, *parent;
	struct bfq_queue *__bfqq;

	if (bfqq->pos_root) {
		rb_erase(&bfqq->pos_node, bfqq->pos_root);
		bfqq->pos_root = NULL;
	}

	if (bfq_class_idle(bfqq))
		return;
	if (!bfqq->next_rq)
		return;

	bfqq->pos_root = &bfq_bfqq_to_bfqg(bfqq)->rq_pos_tree;
	__bfqq = bfq_rq_pos_tree_lookup(bfqd, bfqq->pos_root,
			blk_rq_pos(bfqq->next_rq), &parent, &p);
	if (!__bfqq) {
		rb_link_node(&bfqq->pos_node, parent, p);
		rb_insert_color(&bfqq->pos_node, bfqq->pos_root);
	} else
		bfqq->pos_root = NULL;
}

/*
 * Tell whether there are active queues or groups with differentiated weights.
 */
static bool bfq_differentiated_weights(struct bfq_data *bfqd)
{
	/*
	 * For weights to differ, at least one of the trees must contain
	 * at least two nodes.
	 */
	return (!RB_EMPTY_ROOT(&bfqd->queue_weights_tree) &&
		(bfqd->queue_weights_tree.rb_node->rb_left ||
		 bfqd->queue_weights_tree.rb_node->rb_right)
#ifdef CONFIG_BFQ_GROUP_IOSCHED
	       ) ||
	       (!RB_EMPTY_ROOT(&bfqd->group_weights_tree) &&
		(bfqd->group_weights_tree.rb_node->rb_left ||
		 bfqd->group_weights_tree.rb_node->rb_right)
#endif
	       );
}

/*
 * The following function returns true if every queue must receive the
 * same share of the throughput (this condition is used when deciding
 * whether idling may be disabled, see the comments in the function
 * bfq_bfqq_may_idle()).
 *
 * Such a scenario occurs when:
 * 1) all active queues have the same weight,
 * 2) all active groups at the same level in the groups tree have the same
 *    weight,
 * 3) all active groups at the same level in the groups tree have the same
 *    number of children.
 *
 * Unfortunately, keeping the necessary state for evaluating exactly the
 * above symmetry conditions would be quite complex and time-consuming.
 * Therefore this function evaluates, instead, the following stronger
 * sub-conditions, for which it is much easier to maintain the needed
 * state:
 * 1) all active queues have the same weight,
 * 2) all active groups have the same weight,
 * 3) all active groups have at most one active child each.
 * In particular, the last two conditions are always true if hierarchical
 * support and the cgroups interface are not enabled, thus no state needs
 * to be maintained in this case.
 */
static bool bfq_symmetric_scenario(struct bfq_data *bfqd)
{
	return !bfq_differentiated_weights(bfqd);
}

/*
 * If the weight-counter tree passed as input contains no counter for
 * the weight of the input entity, then add that counter; otherwise just
 * increment the existing counter.
 *
 * Note that weight-counter trees contain few nodes in mostly symmetric
 * scenarios. For example, if all queues have the same weight, then the
 * weight-counter tree for the queues may contain at most one node.
 * This holds even if low_latency is on, because weight-raised queues
 * are not inserted in the tree.
 * In most scenarios, the rate at which nodes are created/destroyed
 * should be low too.
 */
static void bfq_weights_tree_add(struct bfq_data *bfqd,
				 struct bfq_entity *entity,
				 struct rb_root *root)
{
	struct rb_node **new = &(root->rb_node), *parent = NULL;

	/*
	 * Do not insert if the entity is already associated with a
	 * counter, which happens if:
	 *   1) the entity is associated with a queue,
	 *   2) a request arrival has caused the queue to become both
	 *      non-weight-raised, and hence change its weight, and
	 *      backlogged; in this respect, each of the two events
	 *      causes an invocation of this function,
	 *   3) this is the invocation of this function caused by the
	 *      second event. This second invocation is actually useless,
	 *      and we handle this fact by exiting immediately. More
	 *      efficient or clearer solutions might possibly be adopted.
	 */
	if (entity->weight_counter)
		return;

	while (*new) {
		struct bfq_weight_counter *__counter = container_of(*new,
						struct bfq_weight_counter,
						weights_node);
		parent = *new;

		if (entity->weight == __counter->weight) {
			entity->weight_counter = __counter;
			goto inc_counter;
		}
		if (entity->weight < __counter->weight)
			new = &((*new)->rb_left);
		else
			new = &((*new)->rb_right);
	}

	entity->weight_counter = kzalloc(sizeof(struct bfq_weight_counter),
					 GFP_ATOMIC);
	entity->weight_counter->weight = entity->weight;
	rb_link_node(&entity->weight_counter->weights_node, parent, new);
	rb_insert_color(&entity->weight_counter->weights_node, root);

inc_counter:
	entity->weight_counter->num_active++;
}

/*
 * Decrement the weight counter associated with the entity, and, if the
 * counter reaches 0, remove the counter from the tree.
 * See the comments to the function bfq_weights_tree_add() for considerations
 * about overhead.
 */
static void bfq_weights_tree_remove(struct bfq_data *bfqd,
				    struct bfq_entity *entity,
				    struct rb_root *root)
{
	if (!entity->weight_counter)
		return;

	BUG_ON(RB_EMPTY_ROOT(root));
	BUG_ON(entity->weight_counter->weight != entity->weight);

	BUG_ON(!entity->weight_counter->num_active);
	entity->weight_counter->num_active--;
	if (entity->weight_counter->num_active > 0)
		goto reset_entity_pointer;

	rb_erase(&entity->weight_counter->weights_node, root);
	kfree(entity->weight_counter);

reset_entity_pointer:
	entity->weight_counter = NULL;
}

static struct request *bfq_find_next_rq(struct bfq_data *bfqd,
					struct bfq_queue *bfqq,
					struct request *last)
{
	struct rb_node *rbnext = rb_next(&last->rb_node);
	struct rb_node *rbprev = rb_prev(&last->rb_node);
	struct request *next = NULL, *prev = NULL;

	BUG_ON(RB_EMPTY_NODE(&last->rb_node));

	if (rbprev)
		prev = rb_entry_rq(rbprev);

	if (rbnext)
		next = rb_entry_rq(rbnext);
	else {
		rbnext = rb_first(&bfqq->sort_list);
		if (rbnext && rbnext != &last->rb_node)
			next = rb_entry_rq(rbnext);
	}

	return bfq_choose_req(bfqd, next, prev, blk_rq_pos(last));
}

/* see the definition of bfq_async_charge_factor for details */
static unsigned long bfq_serv_to_charge(struct request *rq,
					struct bfq_queue *bfqq)
{
	if (bfq_bfqq_sync(bfqq) || bfqq->wr_coeff > 1)
		return blk_rq_sectors(rq);

	/*
	 * If there are no weight-raised queues, then amplify service
	 * by just the async charge factor; otherwise amplify service
	 * by twice the async charge factor, to further reduce latency
	 * for weight-raised queues.
	 */
	if (bfqq->bfqd->wr_busy_queues == 0)
		return blk_rq_sectors(rq) * bfq_async_charge_factor;

	return blk_rq_sectors(rq) * 2 * bfq_async_charge_factor;
}

/**
 * bfq_updated_next_req - update the queue after a new next_rq selection.
 * @bfqd: the device data the queue belongs to.
 * @bfqq: the queue to update.
 *
 * If the first request of a queue changes we make sure that the queue
 * has enough budget to serve at least its first request (if the
 * request has grown).  We do this because if the queue has not enough
 * budget for its first request, it has to go through two dispatch
 * rounds to actually get it dispatched.
 */
static void bfq_updated_next_req(struct bfq_data *bfqd,
				 struct bfq_queue *bfqq)
{
	struct bfq_entity *entity = &bfqq->entity;
	struct bfq_service_tree *st = bfq_entity_service_tree(entity);
	struct request *next_rq = bfqq->next_rq;
	unsigned long new_budget;

	if (!next_rq)
		return;

	if (bfqq == bfqd->in_service_queue)
		/*
		 * In order not to break guarantees, budgets cannot be
		 * changed after an entity has been selected.
		 */
		return;

	BUG_ON(entity->tree != &st->active);
	BUG_ON(entity == entity->sched_data->in_service_entity);

	new_budget = max_t(unsigned long, bfqq->max_budget,
			   bfq_serv_to_charge(next_rq, bfqq));
	if (entity->budget != new_budget) {
		entity->budget = new_budget;
		bfq_log_bfqq(bfqd, bfqq, "updated next rq: new budget %lu",
					 new_budget);
		bfq_activate_bfqq(bfqd, bfqq);
	}
}

static unsigned int bfq_wr_duration(struct bfq_data *bfqd)
{
	u64 dur;

	if (bfqd->bfq_wr_max_time > 0)
		return bfqd->bfq_wr_max_time;

	dur = bfqd->RT_prod;
	do_div(dur, bfqd->peak_rate);

	/*
	 * Limit duration between 3 and 13 seconds. Tests show that
	 * higher values than 13 seconds often yield the opposite of
	 * the desired result, i.e., worsen responsiveness by letting
	 * non-interactive and non-soft-real-time applications
	 * preserve weight raising for a too long time interval.
	 *
	 * On the other end, lower values than 3 seconds make it
	 * difficult for most interactive tasks to complete their jobs
	 * before weight-raising finishes.
	 */
	if (dur > msecs_to_jiffies(13000))
		dur = msecs_to_jiffies(13000);
	else if (dur < msecs_to_jiffies(3000))
		dur = msecs_to_jiffies(3000);

	return dur;
}

static void
bfq_bfqq_resume_state(struct bfq_queue *bfqq, struct bfq_io_cq *bic)
{
	if (bic->saved_idle_window)
		bfq_mark_bfqq_idle_window(bfqq);
	else
		bfq_clear_bfqq_idle_window(bfqq);

	if (bic->saved_IO_bound)
		bfq_mark_bfqq_IO_bound(bfqq);
	else
		bfq_clear_bfqq_IO_bound(bfqq);
}

static int bfqq_process_refs(struct bfq_queue *bfqq)
{
	int process_refs, io_refs;

	lockdep_assert_held(bfqq->bfqd->queue->queue_lock);

	io_refs = bfqq->allocated[READ] + bfqq->allocated[WRITE];
	process_refs = bfqq->ref - io_refs - bfqq->entity.on_st;
	BUG_ON(process_refs < 0);
	return process_refs;
}

/* Empty burst list and add just bfqq (see comments to bfq_handle_burst) */
static void bfq_reset_burst_list(struct bfq_data *bfqd, struct bfq_queue *bfqq)
{
	struct bfq_queue *item;
	struct hlist_node *n;

	hlist_for_each_entry_safe(item, n, &bfqd->burst_list, burst_list_node)
		hlist_del_init(&item->burst_list_node);
	hlist_add_head(&bfqq->burst_list_node, &bfqd->burst_list);
	bfqd->burst_size = 1;
	bfqd->burst_parent_entity = bfqq->entity.parent;
}

/* Add bfqq to the list of queues in current burst (see bfq_handle_burst) */
static void bfq_add_to_burst(struct bfq_data *bfqd, struct bfq_queue *bfqq)
{
	/* Increment burst size to take into account also bfqq */
	bfqd->burst_size++;

	bfq_log_bfqq(bfqd, bfqq, "add_to_burst %d", bfqd->burst_size);

	BUG_ON(bfqd->burst_size > bfqd->bfq_large_burst_thresh);

	if (bfqd->burst_size == bfqd->bfq_large_burst_thresh) {
		struct bfq_queue *pos, *bfqq_item;
		struct hlist_node *n;

		/*
		 * Enough queues have been activated shortly after each
		 * other to consider this burst as large.
		 */
		bfqd->large_burst = true;
		bfq_log_bfqq(bfqd, bfqq, "add_to_burst: large burst started");

		/*
		 * We can now mark all queues in the burst list as
		 * belonging to a large burst.
		 */
		hlist_for_each_entry(bfqq_item, &bfqd->burst_list,
				     burst_list_node) {
		        bfq_mark_bfqq_in_large_burst(bfqq_item);
			bfq_log_bfqq(bfqd, bfqq_item, "marked in large burst");
		}

		bfq_mark_bfqq_in_large_burst(bfqq);
		bfq_log_bfqq(bfqd, bfqq, "marked in large burst");

		/*
		 * From now on, and until the current burst finishes, any
		 * new queue being activated shortly after the last queue
		 * was inserted in the burst can be immediately marked as
		 * belonging to a large burst. So the burst list is not
		 * needed any more. Remove it.
		 */
		hlist_for_each_entry_safe(pos, n, &bfqd->burst_list,
					  burst_list_node)
			hlist_del_init(&pos->burst_list_node);
	} else /*
		* Burst not yet large: add bfqq to the burst list. Do
		* not increment the ref counter for bfqq, because bfqq
		* is removed from the burst list before freeing bfqq
		* in put_queue.
		*/
		hlist_add_head(&bfqq->burst_list_node, &bfqd->burst_list);
}

/*
 * If many queues belonging to the same group happen to be created
 * shortly after each other, then the processes associated with these
 * queues have typically a common goal. In particular, bursts of queue
 * creations are usually caused by services or applications that spawn
 * many parallel threads/processes. Examples are systemd during boot,
 * or git grep. To help these processes get their job done as soon as
 * possible, it is usually better to not grant either weight-raising
 * or device idling to their queues.
 *
 * In this comment we describe, firstly, the reasons why this fact
 * holds, and, secondly, the next function, which implements the main
 * steps needed to properly mark these queues so that they can then be
 * treated in a different way.
 *
 * The above services or applications benefit mostly from a high
 * throughput: the quicker the requests of the activated queues are
 * cumulatively served, the sooner the target job of these queues gets
 * completed. As a consequence, weight-raising any of these queues,
 * which also implies idling the device for it, is almost always
 * counterproductive. In most cases it just lowers throughput.
 *
 * On the other hand, a burst of queue creations may be caused also by
 * the start of an application that does not consist of a lot of
 * parallel I/O-bound threads. In fact, with a complex application,
 * several short processes may need to be executed to start-up the
 * application. In this respect, to start an application as quickly as
 * possible, the best thing to do is in any case to privilege the I/O
 * related to the application with respect to all other
 * I/O. Therefore, the best strategy to start as quickly as possible
 * an application that causes a burst of queue creations is to
 * weight-raise all the queues created during the burst. This is the
 * exact opposite of the best strategy for the other type of bursts.
 *
 * In the end, to take the best action for each of the two cases, the
 * two types of bursts need to be distinguished. Fortunately, this
 * seems relatively easy, by looking at the sizes of the bursts. In
 * particular, we found a threshold such that only bursts with a
 * larger size than that threshold are apparently caused by
 * services or commands such as systemd or git grep. For brevity,
 * hereafter we call just 'large' these bursts. BFQ *does not*
 * weight-raise queues whose creation occurs in a large burst. In
 * addition, for each of these queues BFQ performs or does not perform
 * idling depending on which choice boosts the throughput more. The
 * exact choice depends on the device and request pattern at
 * hand.
 *
 * Unfortunately, false positives may occur while an interactive task
 * is starting (e.g., an application is being started). The
 * consequence is that the queues associated with the task do not
 * enjoy weight raising as expected. Fortunately these false positives
 * are very rare. They typically occur if some service happens to
 * start doing I/O exactly when the interactive task starts.
 *
 * Turning back to the next function, it implements all the steps
 * needed to detect the occurrence of a large burst and to properly
 * mark all the queues belonging to it (so that they can then be
 * treated in a different way). This goal is achieved by maintaining a
 * "burst list" that holds, temporarily, the queues that belong to the
 * burst in progress. The list is then used to mark these queues as
 * belonging to a large burst if the burst does become large. The main
 * steps are the following.
 *
 * . when the very first queue is created, the queue is inserted into the
 *   list (as it could be the first queue in a possible burst)
 *
 * . if the current burst has not yet become large, and a queue Q that does
 *   not yet belong to the burst is activated shortly after the last time
 *   at which a new queue entered the burst list, then the function appends
 *   Q to the burst list
 *
 * . if, as a consequence of the previous step, the burst size reaches
 *   the large-burst threshold, then
 *
 *     . all the queues in the burst list are marked as belonging to a
 *       large burst
 *
 *     . the burst list is deleted; in fact, the burst list already served
 *       its purpose (keeping temporarily track of the queues in a burst,
 *       so as to be able to mark them as belonging to a large burst in the
 *       previous sub-step), and now is not needed any more
 *
 *     . the device enters a large-burst mode
 *
 * . if a queue Q that does not belong to the burst is created while
 *   the device is in large-burst mode and shortly after the last time
 *   at which a queue either entered the burst list or was marked as
 *   belonging to the current large burst, then Q is immediately marked
 *   as belonging to a large burst.
 *
 * . if a queue Q that does not belong to the burst is created a while
 *   later, i.e., not shortly after, than the last time at which a queue
 *   either entered the burst list or was marked as belonging to the
 *   current large burst, then the current burst is deemed as finished and:
 *
 *        . the large-burst mode is reset if set
 *
 *        . the burst list is emptied
 *
 *        . Q is inserted in the burst list, as Q may be the first queue
 *          in a possible new burst (then the burst list contains just Q
 *          after this step).
 */
static void bfq_handle_burst(struct bfq_data *bfqd, struct bfq_queue *bfqq)
{
	/*
	 * If bfqq is already in the burst list or is part of a large
	 * burst, or finally has just been split, then there is
	 * nothing else to do.
	 */
	if (!hlist_unhashed(&bfqq->burst_list_node) ||
	    bfq_bfqq_in_large_burst(bfqq) ||
	    time_is_after_eq_jiffies(bfqq->split_time +
				     msecs_to_jiffies(10)))
		return;

	/*
	 * If bfqq's creation happens late enough, or bfqq belongs to
	 * a different group than the burst group, then the current
	 * burst is finished, and related data structures must be
	 * reset.
	 *
	 * In this respect, consider the special case where bfqq is
	 * the very first queue created after BFQ is selected for this
	 * device. In this case, last_ins_in_burst and
	 * burst_parent_entity are not yet significant when we get
	 * here. But it is easy to verify that, whether or not the
	 * following condition is true, bfqq will end up being
	 * inserted into the burst list. In particular the list will
	 * happen to contain only bfqq. And this is exactly what has
	 * to happen, as bfqq may be the first queue of the first
	 * burst.
	 */
	if (time_is_before_jiffies(bfqd->last_ins_in_burst +
	    bfqd->bfq_burst_interval) ||
	    bfqq->entity.parent != bfqd->burst_parent_entity) {
		bfqd->large_burst = false;
		bfq_reset_burst_list(bfqd, bfqq);
		bfq_log_bfqq(bfqd, bfqq,
			"handle_burst: late activation or different group");
		goto end;
	}

	/*
	 * If we get here, then bfqq is being activated shortly after the
	 * last queue. So, if the current burst is also large, we can mark
	 * bfqq as belonging to this large burst immediately.
	 */
	if (bfqd->large_burst) {
		bfq_log_bfqq(bfqd, bfqq, "handle_burst: marked in burst");
		bfq_mark_bfqq_in_large_burst(bfqq);
		goto end;
	}

	/*
	 * If we get here, then a large-burst state has not yet been
	 * reached, but bfqq is being activated shortly after the last
	 * queue. Then we add bfqq to the burst.
	 */
	bfq_add_to_burst(bfqd, bfqq);
end:
	/*
	 * At this point, bfqq either has been added to the current
	 * burst or has caused the current burst to terminate and a
	 * possible new burst to start. In particular, in the second
	 * case, bfqq has become the first queue in the possible new
	 * burst.  In both cases last_ins_in_burst needs to be moved
	 * forward.
	 */
	bfqd->last_ins_in_burst = jiffies;

}

static int bfq_bfqq_budget_left(struct bfq_queue *bfqq)
{
	struct bfq_entity *entity = &bfqq->entity;

	return entity->budget - entity->service;
}

/*
 * If enough samples have been computed, return the current max budget
 * stored in bfqd, which is dynamically updated according to the
 * estimated disk peak rate; otherwise return the default max budget
 */
static int bfq_max_budget(struct bfq_data *bfqd)
{
	if (bfqd->budgets_assigned < bfq_stats_min_budgets)
		return bfq_default_max_budget;
	else
		return bfqd->bfq_max_budget;
}

/*
 * Return min budget, which is a fraction of the current or default
 * max budget (trying with 1/32)
 */
static int bfq_min_budget(struct bfq_data *bfqd)
{
	if (bfqd->budgets_assigned < bfq_stats_min_budgets)
		return bfq_default_max_budget / 32;
	else
		return bfqd->bfq_max_budget / 32;
}

static void bfq_bfqq_expire(struct bfq_data *bfqd,
			    struct bfq_queue *bfqq,
			    bool compensate,
			    enum bfqq_expiration reason);

/*
 * The next function, invoked after the input queue bfqq switches from
 * idle to busy, updates the budget of bfqq. The function also tells
 * whether the in-service queue should be expired, by returning
 * true. The purpose of expiring the in-service queue is to give bfqq
 * the chance to possibly preempt the in-service queue, and the reason
 * for preempting the in-service queue is to achieve one of the two
 * goals below.
 *
 * 1. Guarantee to bfqq its reserved bandwidth even if bfqq has
 * expired because it has remained idle. In particular, bfqq may have
 * expired for one of the following two reasons:
 *
 * - BFQ_BFQQ_NO_MORE_REQUEST bfqq did not enjoy any device idling and
 *   did not make it to issue a new request before its last request
 *   was served;
 *
 * - BFQ_BFQQ_TOO_IDLE bfqq did enjoy device idling, but did not issue
 *   a new request before the expiration of the idling-time.
 *
 * Even if bfqq has expired for one of the above reasons, the process
 * associated with the queue may be however issuing requests greedily,
 * and thus be sensitive to the bandwidth it receives (bfqq may have
 * remained idle for other reasons: CPU high load, bfqq not enjoying
 * idling, I/O throttling somewhere in the path from the process to
 * the I/O scheduler, ...). But if, after every expiration for one of
 * the above two reasons, bfqq has to wait for the service of at least
 * one full budget of another queue before being served again, then
 * bfqq is likely to get a much lower bandwidth or resource time than
 * its reserved ones. To address this issue, two countermeasures need
 * to be taken.
 *
 * First, the budget and the timestamps of bfqq need to be updated in
 * a special way on bfqq reactivation: they need to be updated as if
 * bfqq did not remain idle and did not expire. In fact, if they are
 * computed as if bfqq expired and remained idle until reactivation,
 * then the process associated with bfqq is treated as if, instead of
 * being greedy, it stopped issuing requests when bfqq remained idle,
 * and restarts issuing requests only on this reactivation. In other
 * words, the scheduler does not help the process recover the "service
 * hole" between bfqq expiration and reactivation. As a consequence,
 * the process receives a lower bandwidth than its reserved one. In
 * contrast, to recover this hole, the budget must be updated as if
 * bfqq was not expired at all before this reactivation, i.e., it must
 * be set to the value of the remaining budget when bfqq was
 * expired. Along the same line, timestamps need to be assigned the
 * value they had the last time bfqq was selected for service, i.e.,
 * before last expiration. Thus timestamps need to be back-shifted
 * with respect to their normal computation (see [1] for more details
 * on this tricky aspect).
 *
 * Secondly, to allow the process to recover the hole, the in-service
 * queue must be expired too, to give bfqq the chance to preempt it
 * immediately. In fact, if bfqq has to wait for a full budget of the
 * in-service queue to be completed, then it may become impossible to
 * let the process recover the hole, even if the back-shifted
 * timestamps of bfqq are lower than those of the in-service queue. If
 * this happens for most or all of the holes, then the process may not
 * receive its reserved bandwidth. In this respect, it is worth noting
 * that, being the service of outstanding requests unpreemptible, a
 * little fraction of the holes may however be unrecoverable, thereby
 * causing a little loss of bandwidth.
 *
 * The last important point is detecting whether bfqq does need this
 * bandwidth recovery. In this respect, the next function deems the
 * process associated with bfqq greedy, and thus allows it to recover
 * the hole, if: 1) the process is waiting for the arrival of a new
 * request (which implies that bfqq expired for one of the above two
 * reasons), and 2) such a request has arrived soon. The first
 * condition is controlled through the flag non_blocking_wait_rq,
 * while the second through the flag arrived_in_time. If both
 * conditions hold, then the function computes the budget in the
 * above-described special way, and signals that the in-service queue
 * should be expired. Timestamp back-shifting is done later in
 * __bfq_activate_entity.
 *
 * 2. Reduce latency. Even if timestamps are not backshifted to let
 * the process associated with bfqq recover a service hole, bfqq may
 * however happen to have, after being (re)activated, a lower finish
 * timestamp than the in-service queue.  That is, the next budget of
 * bfqq may have to be completed before the one of the in-service
 * queue. If this is the case, then preempting the in-service queue
 * allows this goal to be achieved, apart from the unpreemptible,
 * outstanding requests mentioned above.
 *
 * Unfortunately, regardless of which of the above two goals one wants
 * to achieve, service trees need first to be updated to know whether
 * the in-service queue must be preempted. To have service trees
 * correctly updated, the in-service queue must be expired and
 * rescheduled, and bfqq must be scheduled too. This is one of the
 * most costly operations (in future versions, the scheduling
 * mechanism may be re-designed in such a way to make it possible to
 * know whether preemption is needed without needing to update service
 * trees). In addition, queue preemptions almost always cause random
 * I/O, and thus loss of throughput. Because of these facts, the next
 * function adopts the following simple scheme to avoid both costly
 * operations and too frequent preemptions: it requests the expiration
 * of the in-service queue (unconditionally) only for queues that need
 * to recover a hole, or that either are weight-raised or deserve to
 * be weight-raised.
 */
static bool bfq_bfqq_update_budg_for_activation(struct bfq_data *bfqd,
						struct bfq_queue *bfqq,
						bool arrived_in_time,
						bool wr_or_deserves_wr)
{
	struct bfq_entity *entity = &bfqq->entity;

	if (bfq_bfqq_non_blocking_wait_rq(bfqq) && arrived_in_time) {
		/*
		 * We do not clear the flag non_blocking_wait_rq here, as
		 * the latter is used in bfq_activate_bfqq to signal
		 * that timestamps need to be back-shifted (and is
		 * cleared right after).
		 */

		/*
		 * In next assignment we rely on that either
		 * entity->service or entity->budget are not updated
		 * on expiration if bfqq is empty (see
		 * __bfq_bfqq_recalc_budget). Thus both quantities
		 * remain unchanged after such an expiration, and the
		 * following statement therefore assigns to
		 * entity->budget the remaining budget on such an
		 * expiration. For clarity, entity->service is not
		 * updated on expiration in any case, and, in normal
		 * operation, is reset only when bfqq is selected for
		 * service (see bfq_get_next_queue).
		 */
		BUG_ON(bfqq->max_budget < 0);
		entity->budget = min_t(unsigned long,
				       bfq_bfqq_budget_left(bfqq),
				       bfqq->max_budget);

		BUG_ON(entity->budget < 0);
		return true;
	}

	BUG_ON(bfqq->max_budget < 0);
	entity->budget = max_t(unsigned long, bfqq->max_budget,
			       bfq_serv_to_charge(bfqq->next_rq, bfqq));
	BUG_ON(entity->budget < 0);

	bfq_clear_bfqq_non_blocking_wait_rq(bfqq);
	return wr_or_deserves_wr;
}

static void bfq_update_bfqq_wr_on_rq_arrival(struct bfq_data *bfqd,
					     struct bfq_queue *bfqq,
					     unsigned int old_wr_coeff,
					     bool wr_or_deserves_wr,
					     bool interactive,
					     bool in_burst,
					     bool soft_rt)
{
	if (old_wr_coeff == 1 && wr_or_deserves_wr) {
		/* start a weight-raising period */
		if (interactive) {
			bfqq->wr_coeff = bfqd->bfq_wr_coeff;
			bfqq->wr_cur_max_time = bfq_wr_duration(bfqd);
		} else {
			bfqq->wr_coeff = bfqd->bfq_wr_coeff *
				BFQ_SOFTRT_WEIGHT_FACTOR;
			bfqq->wr_cur_max_time =
				bfqd->bfq_wr_rt_max_time;
		}
		/*
		 * If needed, further reduce budget to make sure it is
		 * close to bfqq's backlog, so as to reduce the
		 * scheduling-error component due to a too large
		 * budget. Do not care about throughput consequences,
		 * but only about latency. Finally, do not assign a
		 * too small budget either, to avoid increasing
		 * latency by causing too frequent expirations.
		 */
		bfqq->entity.budget = min_t(unsigned long,
					    bfqq->entity.budget,
					    2 * bfq_min_budget(bfqd));

		bfq_log_bfqq(bfqd, bfqq,
			     "wrais starting at %lu, rais_max_time %u",
			     jiffies,
			     jiffies_to_msecs(bfqq->wr_cur_max_time));
	} else if (old_wr_coeff > 1) {
		if (interactive) { /* update wr coeff and duration */
			bfqq->wr_coeff = bfqd->bfq_wr_coeff;
			bfqq->wr_cur_max_time = bfq_wr_duration(bfqd);
		} else if (in_burst) {
			bfqq->wr_coeff = 1;
			bfq_log_bfqq(bfqd, bfqq,
				     "wrais ending at %lu, rais_max_time %u",
				     jiffies,
				     jiffies_to_msecs(bfqq->
						      wr_cur_max_time));
		} else if (time_before(
				   bfqq->last_wr_start_finish +
				   bfqq->wr_cur_max_time,
				   jiffies +
				   bfqd->bfq_wr_rt_max_time) &&
			   soft_rt) {
			/*
			 * The remaining weight-raising time is lower
			 * than bfqd->bfq_wr_rt_max_time, which means
			 * that the application is enjoying weight
			 * raising either because deemed soft-rt in
			 * the near past, or because deemed interactive
			 * a long ago.
			 * In both cases, resetting now the current
			 * remaining weight-raising time for the
			 * application to the weight-raising duration
			 * for soft rt applications would not cause any
			 * latency increase for the application (as the
			 * new duration would be higher than the
			 * remaining time).
			 *
			 * In addition, the application is now meeting
			 * the requirements for being deemed soft rt.
			 * In the end we can correctly and safely
			 * (re)charge the weight-raising duration for
			 * the application with the weight-raising
			 * duration for soft rt applications.
			 *
			 * In particular, doing this recharge now, i.e.,
			 * before the weight-raising period for the
			 * application finishes, reduces the probability
			 * of the following negative scenario:
			 * 1) the weight of a soft rt application is
			 *    raised at startup (as for any newly
			 *    created application),
			 * 2) since the application is not interactive,
			 *    at a certain time weight-raising is
			 *    stopped for the application,
			 * 3) at that time the application happens to
			 *    still have pending requests, and hence
			 *    is destined to not have a chance to be
			 *    deemed soft rt before these requests are
			 *    completed (see the comments to the
			 *    function bfq_bfqq_softrt_next_start()
			 *    for details on soft rt detection),
			 * 4) these pending requests experience a high
			 *    latency because the application is not
			 *    weight-raised while they are pending.
			 */
			bfqq->last_wr_start_finish = jiffies;
			bfqq->wr_cur_max_time =
				bfqd->bfq_wr_rt_max_time;
			bfqq->wr_coeff = bfqd->bfq_wr_coeff *
				BFQ_SOFTRT_WEIGHT_FACTOR;
			bfq_log_bfqq(bfqd, bfqq,
				     "switching to soft_rt wr, or "
				     " just moving forward duration");
		}
	}
}

static bool bfq_bfqq_idle_for_long_time(struct bfq_data *bfqd,
					struct bfq_queue *bfqq)
{
	return bfqq->dispatched == 0 &&
		time_is_before_jiffies(
			bfqq->budget_timeout +
			bfqd->bfq_wr_min_idle_time);
}

static void bfq_bfqq_handle_idle_busy_switch(struct bfq_data *bfqd,
					     struct bfq_queue *bfqq,
					     int old_wr_coeff,
					     struct request *rq,
					     bool *interactive)
{
	bool soft_rt, in_burst,	wr_or_deserves_wr,
		bfqq_wants_to_preempt,
		idle_for_long_time = bfq_bfqq_idle_for_long_time(bfqd, bfqq),
		/*
		 * See the comments on
		 * bfq_bfqq_update_budg_for_activation for
		 * details on the usage of the next variable.
		 */
		arrived_in_time =  ktime_get_ns() <=
			RQ_BIC(rq)->ttime.last_end_request +
			bfqd->bfq_slice_idle * 3;

	bfq_log_bfqq(bfqd, bfqq,
		     "bfq_add_request non-busy: "
		     "jiffies %lu, in_time %d, idle_long %d busyw %d "
		     "wr_coeff %u",
		     jiffies, arrived_in_time,
		     idle_for_long_time,
		     bfq_bfqq_non_blocking_wait_rq(bfqq),
		     old_wr_coeff);

	BUG_ON(bfqq->entity.budget < bfqq->entity.service);

	BUG_ON(bfqq == bfqd->in_service_queue);
	bfqg_stats_update_io_add(bfqq_group(RQ_BFQQ(rq)), bfqq,
				 rq->cmd_flags);

	/*
	 * bfqq deserves to be weight-raised if:
	 * - it is sync,
	 * - it does not belong to a large burst,
	 * - it has been idle for enough time or is soft real-time,
	 * - is linked to a bfq_io_cq (it is not shared in any sense)
	 */
	in_burst = bfq_bfqq_in_large_burst(bfqq);
	soft_rt = bfqd->bfq_wr_max_softrt_rate > 0 &&
		!in_burst &&
		time_is_before_jiffies(bfqq->soft_rt_next_start);
	*interactive =
		!in_burst &&
		idle_for_long_time;
	wr_or_deserves_wr = bfqd->low_latency &&
		(bfqq->wr_coeff > 1 ||
		 (bfq_bfqq_sync(bfqq) &&
		  bfqq->bic && (*interactive || soft_rt)));

	bfq_log_bfqq(bfqd, bfqq,
		     "bfq_add_request: "
		     "in_burst %d, "
		     "soft_rt %d (next %lu), inter %d, bic %p",
		     bfq_bfqq_in_large_burst(bfqq), soft_rt,
		     bfqq->soft_rt_next_start,
		     *interactive,
		     bfqq->bic);

	/*
	 * Using the last flag, update budget and check whether bfqq
	 * may want to preempt the in-service queue.
	 */
	bfqq_wants_to_preempt =
		bfq_bfqq_update_budg_for_activation(bfqd, bfqq,
						    arrived_in_time,
						    wr_or_deserves_wr);

	/*
	 * If bfqq happened to be activated in a burst, but has been
	 * idle for much more than an interactive queue, then we
	 * assume that, in the overall I/O initiated in the burst, the
	 * I/O associated with bfqq is finished. So bfqq does not need
	 * to be treated as a queue belonging to a burst
	 * anymore. Accordingly, we reset bfqq's in_large_burst flag
	 * if set, and remove bfqq from the burst list if it's
	 * there. We do not decrement burst_size, because the fact
	 * that bfqq does not need to belong to the burst list any
	 * more does not invalidate the fact that bfqq was created in
	 * a burst.
	 */
	if (likely(!bfq_bfqq_just_created(bfqq)) &&
	    idle_for_long_time &&
	    time_is_before_jiffies(
		    bfqq->budget_timeout +
		    msecs_to_jiffies(10000))) {
		hlist_del_init(&bfqq->burst_list_node);
		bfq_clear_bfqq_in_large_burst(bfqq);
	}

	bfq_clear_bfqq_just_created(bfqq);

	if (!bfq_bfqq_IO_bound(bfqq)) {
		if (arrived_in_time) {
			bfqq->requests_within_timer++;
			if (bfqq->requests_within_timer >=
			    bfqd->bfq_requests_within_timer)
				bfq_mark_bfqq_IO_bound(bfqq);
		} else
			bfqq->requests_within_timer = 0;
		bfq_log_bfqq(bfqd, bfqq, "requests in time %d",
			     bfqq->requests_within_timer);
	}

	if (bfqd->low_latency) {
		if (unlikely(time_is_after_jiffies(bfqq->split_time)))
			/* wraparound */
			bfqq->split_time =
				jiffies - bfqd->bfq_wr_min_idle_time - 1;

		if (time_is_before_jiffies(bfqq->split_time +
					   bfqd->bfq_wr_min_idle_time)) {
			bfq_update_bfqq_wr_on_rq_arrival(bfqd, bfqq,
							 old_wr_coeff,
							 wr_or_deserves_wr,
							 *interactive,
							 in_burst,
							 soft_rt);

			if (old_wr_coeff != bfqq->wr_coeff)
				bfqq->entity.prio_changed = 1;
		}
	}

	bfqq->last_idle_bklogged = jiffies;
	bfqq->service_from_backlogged = 0;
	bfq_clear_bfqq_softrt_update(bfqq);

	bfq_add_bfqq_busy(bfqd, bfqq);

	/*
	 * Expire in-service queue only if preemption may be needed
	 * for guarantees. In this respect, the function
	 * next_queue_may_preempt just checks a simple, necessary
	 * condition, and not a sufficient condition based on
	 * timestamps. In fact, for the latter condition to be
	 * evaluated, timestamps would need first to be updated, and
	 * this operation is quite costly (see the comments on the
	 * function bfq_bfqq_update_budg_for_activation).
	 */
	if (bfqd->in_service_queue && bfqq_wants_to_preempt &&
	    bfqd->in_service_queue->wr_coeff < bfqq->wr_coeff &&
	    next_queue_may_preempt(bfqd)) {
		struct bfq_queue *in_serv =
			bfqd->in_service_queue;
		BUG_ON(in_serv == bfqq);

		bfq_bfqq_expire(bfqd, bfqd->in_service_queue,
				false, BFQ_BFQQ_PREEMPTED);
		BUG_ON(in_serv->entity.budget < 0);
	}
}

static void bfq_add_request(struct request *rq)
{
	struct bfq_queue *bfqq = RQ_BFQQ(rq);
	struct bfq_data *bfqd = bfqq->bfqd;
	struct request *next_rq, *prev;
	unsigned int old_wr_coeff = bfqq->wr_coeff;
	bool interactive = false;

	bfq_log_bfqq(bfqd, bfqq, "add_request: size %u %s",
		     blk_rq_sectors(rq), rq_is_sync(rq) ? "S" : "A");

	if (bfqq->wr_coeff > 1) /* queue is being weight-raised */
		bfq_log_bfqq(bfqd, bfqq,
			"raising period dur %u/%u msec, old coeff %u, w %d(%d)",
			jiffies_to_msecs(jiffies - bfqq->last_wr_start_finish),
			jiffies_to_msecs(bfqq->wr_cur_max_time),
			bfqq->wr_coeff,
			bfqq->entity.weight, bfqq->entity.orig_weight);

	bfqq->queued[rq_is_sync(rq)]++;
	bfqd->queued++;

	elv_rb_add(&bfqq->sort_list, rq);

	/*
	 * Check if this request is a better next-to-serve candidate.
	 */
	prev = bfqq->next_rq;
	next_rq = bfq_choose_req(bfqd, bfqq->next_rq, rq, bfqd->last_position);
	BUG_ON(!next_rq);
	bfqq->next_rq = next_rq;

	/*
	 * Adjust priority tree position, if next_rq changes.
	 */
	if (prev != bfqq->next_rq)
		bfq_pos_tree_add_move(bfqd, bfqq);

	if (!bfq_bfqq_busy(bfqq)) /* switching to busy ... */
		bfq_bfqq_handle_idle_busy_switch(bfqd, bfqq, old_wr_coeff,
						 rq, &interactive);
	else {
		if (bfqd->low_latency && old_wr_coeff == 1 && !rq_is_sync(rq) &&
		    time_is_before_jiffies(
				bfqq->last_wr_start_finish +
				bfqd->bfq_wr_min_inter_arr_async)) {
			bfqq->wr_coeff = bfqd->bfq_wr_coeff;
			bfqq->wr_cur_max_time = bfq_wr_duration(bfqd);

			bfqd->wr_busy_queues++;
			bfqq->entity.prio_changed = 1;
			bfq_log_bfqq(bfqd, bfqq,
				     "non-idle wrais starting, "
				     "wr_max_time %u wr_busy %d",
				     jiffies_to_msecs(bfqq->wr_cur_max_time),
				     bfqd->wr_busy_queues);
		}
		if (prev != bfqq->next_rq)
			bfq_updated_next_req(bfqd, bfqq);
	}

	/*
	 * Assign jiffies to last_wr_start_finish in the following
	 * cases:
	 *
	 * . if bfqq is not going to be weight-raised, because, for
	 *   non weight-raised queues, last_wr_start_finish stores the
	 *   arrival time of the last request; as of now, this piece
	 *   of information is used only for deciding whether to
	 *   weight-raise async queues
	 *
	 * . if bfqq is not weight-raised, because, if bfqq is now
	 *   switching to weight-raised, then last_wr_start_finish
	 *   stores the time when weight-raising starts
	 *
	 * . if bfqq is interactive, because, regardless of whether
	 *   bfqq is currently weight-raised, the weight-raising
	 *   period must start or restart (this case is considered
	 *   separately because it is not detected by the above
	 *   conditions, if bfqq is already weight-raised)
	 *
	 * last_wr_start_finish has to be updated also if bfqq is soft
	 * real-time, because the weight-raising period is constantly
	 * restarted on idle-to-busy transitions for these queues, but
	 * this is already done in bfq_bfqq_handle_idle_busy_switch if
	 * needed.
	 */
	if (bfqd->low_latency &&
		(old_wr_coeff == 1 || bfqq->wr_coeff == 1 || interactive))
		bfqq->last_wr_start_finish = jiffies;
}

static struct request *bfq_find_rq_fmerge(struct bfq_data *bfqd,
					  struct bio *bio)
{
	struct task_struct *tsk = current;
	struct bfq_io_cq *bic;
	struct bfq_queue *bfqq;

	bic = bfq_bic_lookup(bfqd, tsk->io_context);
	if (!bic)
		return NULL;

	bfqq = bic_to_bfqq(bic, bfq_bio_sync(bio));
	if (bfqq)
		return elv_rb_find(&bfqq->sort_list, bio_end_sector(bio));

	return NULL;
}

static sector_t get_sdist(sector_t last_pos, struct request *rq)
{
	sector_t sdist = 0;

	if (last_pos) {
		if (last_pos < blk_rq_pos(rq))
			sdist = blk_rq_pos(rq) - last_pos;
		else
			sdist = last_pos - blk_rq_pos(rq);
	}

	return sdist;
}

static void bfq_activate_request(struct request_queue *q, struct request *rq)
{
	struct bfq_data *bfqd = q->elevator->elevator_data;
	bfqd->rq_in_driver++;
}

static void bfq_deactivate_request(struct request_queue *q, struct request *rq)
{
	struct bfq_data *bfqd = q->elevator->elevator_data;

	BUG_ON(bfqd->rq_in_driver == 0);
	bfqd->rq_in_driver--;
}

static void bfq_remove_request(struct request *rq)
{
	struct bfq_queue *bfqq = RQ_BFQQ(rq);
	struct bfq_data *bfqd = bfqq->bfqd;
	const int sync = rq_is_sync(rq);

	BUG_ON(bfqq->entity.service > bfqq->entity.budget &&
	       bfqq == bfqd->in_service_queue);

	if (bfqq->next_rq == rq) {
		bfqq->next_rq = bfq_find_next_rq(bfqd, bfqq, rq);
		bfq_updated_next_req(bfqd, bfqq);
	}

	if (rq->queuelist.prev != &rq->queuelist)
		list_del_init(&rq->queuelist);
	BUG_ON(bfqq->queued[sync] == 0);
	bfqq->queued[sync]--;
	bfqd->queued--;
	elv_rb_del(&bfqq->sort_list, rq);

	if (RB_EMPTY_ROOT(&bfqq->sort_list)) {
		BUG_ON(bfqq->entity.budget < 0);

		if (bfq_bfqq_busy(bfqq) && bfqq != bfqd->in_service_queue) {
			bfq_del_bfqq_busy(bfqd, bfqq, 1);

			/* bfqq emptied. In normal operation, when
			 * bfqq is empty, bfqq->entity.service and
			 * bfqq->entity.budget must contain,
			 * respectively, the service received and the
			 * budget used last time bfqq emptied. These
			 * facts do not hold in this case, as at least
			 * this last removal occurred while bfqq is
			 * not in service. To avoid inconsistencies,
			 * reset both bfqq->entity.service and
			 * bfqq->entity.budget.
			 */
			bfqq->entity.budget = bfqq->entity.service = 0;
		}

		/*
		 * Remove queue from request-position tree as it is empty.
		 */
		if (bfqq->pos_root) {
			rb_erase(&bfqq->pos_node, bfqq->pos_root);
			bfqq->pos_root = NULL;
		}
	}

	if (rq->cmd_flags & REQ_META) {
		BUG_ON(bfqq->meta_pending == 0);
		bfqq->meta_pending--;
	}
	bfqg_stats_update_io_remove(bfqq_group(bfqq), rq->cmd_flags);
}

static int bfq_merge(struct request_queue *q, struct request **req,
		     struct bio *bio)
{
	struct bfq_data *bfqd = q->elevator->elevator_data;
	struct request *__rq;

	__rq = bfq_find_rq_fmerge(bfqd, bio);
	if (__rq && elv_rq_merge_ok(__rq, bio)) {
		*req = __rq;
		return ELEVATOR_FRONT_MERGE;
	}

	return ELEVATOR_NO_MERGE;
}

static void bfq_merged_request(struct request_queue *q, struct request *req,
			       int type)
{
	if (type == ELEVATOR_FRONT_MERGE &&
	    rb_prev(&req->rb_node) &&
	    blk_rq_pos(req) <
	    blk_rq_pos(container_of(rb_prev(&req->rb_node),
				    struct request, rb_node))) {
		struct bfq_queue *bfqq = RQ_BFQQ(req);
		struct bfq_data *bfqd = bfqq->bfqd;
		struct request *prev, *next_rq;

		/* Reposition request in its sort_list */
		elv_rb_del(&bfqq->sort_list, req);
		elv_rb_add(&bfqq->sort_list, req);
		/* Choose next request to be served for bfqq */
		prev = bfqq->next_rq;
		next_rq = bfq_choose_req(bfqd, bfqq->next_rq, req,
					 bfqd->last_position);
		BUG_ON(!next_rq);
		bfqq->next_rq = next_rq;
		/*
		 * If next_rq changes, update both the queue's budget to
		 * fit the new request and the queue's position in its
		 * rq_pos_tree.
		 */
		if (prev != bfqq->next_rq) {
			bfq_updated_next_req(bfqd, bfqq);
			bfq_pos_tree_add_move(bfqd, bfqq);
		}
	}
}

#ifdef CONFIG_BFQ_GROUP_IOSCHED
static void bfq_bio_merged(struct request_queue *q, struct request *req,
			   struct bio *bio)
{
	bfqg_stats_update_io_merged(bfqq_group(RQ_BFQQ(req)), bio->bi_rw);
}
#endif

static void bfq_merged_requests(struct request_queue *q, struct request *rq,
				struct request *next)
{
	struct bfq_queue *bfqq = RQ_BFQQ(rq), *next_bfqq = RQ_BFQQ(next);

	/*
	 * If next and rq belong to the same bfq_queue and next is older
	 * than rq, then reposition rq in the fifo (by substituting next
	 * with rq). Otherwise, if next and rq belong to different
	 * bfq_queues, never reposition rq: in fact, we would have to
	 * reposition it with respect to next's position in its own fifo,
	 * which would most certainly be too expensive with respect to
	 * the benefits.
	 */
	if (bfqq == next_bfqq &&
	    !list_empty(&rq->queuelist) && !list_empty(&next->queuelist) &&
	    time_before(next->fifo_time, rq->fifo_time)) {
		list_del_init(&rq->queuelist);
		list_replace_init(&next->queuelist, &rq->queuelist);
		rq->fifo_time = next->fifo_time;
	}

	if (bfqq->next_rq == next)
		bfqq->next_rq = rq;

	bfq_remove_request(next);
	bfqg_stats_update_io_merged(bfqq_group(bfqq), next->cmd_flags);
}

/* Must be called with bfqq != NULL */
static void bfq_bfqq_end_wr(struct bfq_queue *bfqq)
{
	BUG_ON(!bfqq);

	if (bfq_bfqq_busy(bfqq))
		bfqq->bfqd->wr_busy_queues--;
	bfqq->wr_coeff = 1;
	bfqq->wr_cur_max_time = 0;
	/*
	 * Trigger a weight change on the next invocation of
	 * __bfq_entity_update_weight_prio.
	 */
	bfqq->entity.prio_changed = 1;
	bfq_log_bfqq(bfqq->bfqd, bfqq, "end_wr: wr_busy %d",
		     bfqq->bfqd->wr_busy_queues);
}

static void bfq_end_wr_async_queues(struct bfq_data *bfqd,
				    struct bfq_group *bfqg)
{
	int i, j;

	for (i = 0; i < 2; i++)
		for (j = 0; j < IOPRIO_BE_NR; j++)
			if (bfqg->async_bfqq[i][j])
				bfq_bfqq_end_wr(bfqg->async_bfqq[i][j]);
	if (bfqg->async_idle_bfqq)
		bfq_bfqq_end_wr(bfqg->async_idle_bfqq);
}

static void bfq_end_wr(struct bfq_data *bfqd)
{
	struct bfq_queue *bfqq;

	spin_lock_irq(bfqd->queue->queue_lock);

	list_for_each_entry(bfqq, &bfqd->active_list, bfqq_list)
		bfq_bfqq_end_wr(bfqq);
	list_for_each_entry(bfqq, &bfqd->idle_list, bfqq_list)
		bfq_bfqq_end_wr(bfqq);
	bfq_end_wr_async(bfqd);

	spin_unlock_irq(bfqd->queue->queue_lock);
}

static sector_t bfq_io_struct_pos(void *io_struct, bool request)
{
	if (request)
		return blk_rq_pos(io_struct);
	else
		return ((struct bio *)io_struct)->bi_iter.bi_sector;
}

static int bfq_rq_close_to_sector(void *io_struct, bool request,
				  sector_t sector)
{
	return abs(bfq_io_struct_pos(io_struct, request) - sector) <=
	       BFQQ_CLOSE_THR;
}

static struct bfq_queue *bfqq_find_close(struct bfq_data *bfqd,
					 struct bfq_queue *bfqq,
					 sector_t sector)
{
	struct rb_root *root = &bfq_bfqq_to_bfqg(bfqq)->rq_pos_tree;
	struct rb_node *parent, *node;
	struct bfq_queue *__bfqq;

	if (RB_EMPTY_ROOT(root))
		return NULL;

	/*
	 * First, if we find a request starting at the end of the last
	 * request, choose it.
	 */
	__bfqq = bfq_rq_pos_tree_lookup(bfqd, root, sector, &parent, NULL);
	if (__bfqq)
		return __bfqq;

	/*
	 * If the exact sector wasn't found, the parent of the NULL leaf
	 * will contain the closest sector (rq_pos_tree sorted by
	 * next_request position).
	 */
	__bfqq = rb_entry(parent, struct bfq_queue, pos_node);
	if (bfq_rq_close_to_sector(__bfqq->next_rq, true, sector))
		return __bfqq;

	if (blk_rq_pos(__bfqq->next_rq) < sector)
		node = rb_next(&__bfqq->pos_node);
	else
		node = rb_prev(&__bfqq->pos_node);
	if (!node)
		return NULL;

	__bfqq = rb_entry(node, struct bfq_queue, pos_node);
	if (bfq_rq_close_to_sector(__bfqq->next_rq, true, sector))
		return __bfqq;

	return NULL;
}

static struct bfq_queue *bfq_find_close_cooperator(struct bfq_data *bfqd,
						   struct bfq_queue *cur_bfqq,
						   sector_t sector)
{
	struct bfq_queue *bfqq;

	/*
	 * We shall notice if some of the queues are cooperating,
	 * e.g., working closely on the same area of the device. In
	 * that case, we can group them together and: 1) don't waste
	 * time idling, and 2) serve the union of their requests in
	 * the best possible order for throughput.
	 */
	bfqq = bfqq_find_close(bfqd, cur_bfqq, sector);
	if (!bfqq || bfqq == cur_bfqq)
		return NULL;

	return bfqq;
}

static struct bfq_queue *
bfq_setup_merge(struct bfq_queue *bfqq, struct bfq_queue *new_bfqq)
{
	int process_refs, new_process_refs;
	struct bfq_queue *__bfqq;

	/*
	 * If there are no process references on the new_bfqq, then it is
	 * unsafe to follow the ->new_bfqq chain as other bfqq's in the chain
	 * may have dropped their last reference (not just their last process
	 * reference).
	 */
	if (!bfqq_process_refs(new_bfqq))
		return NULL;

	/* Avoid a circular list and skip interim queue merges. */
	while ((__bfqq = new_bfqq->new_bfqq)) {
		if (__bfqq == bfqq)
			return NULL;
		new_bfqq = __bfqq;
	}

	process_refs = bfqq_process_refs(bfqq);
	new_process_refs = bfqq_process_refs(new_bfqq);
	/*
	 * If the process for the bfqq has gone away, there is no
	 * sense in merging the queues.
	 */
	if (process_refs == 0 || new_process_refs == 0)
		return NULL;

	bfq_log_bfqq(bfqq->bfqd, bfqq, "scheduling merge with queue %d",
		new_bfqq->pid);

	/*
	 * Merging is just a redirection: the requests of the process
	 * owning one of the two queues are redirected to the other queue.
	 * The latter queue, in its turn, is set as shared if this is the
	 * first time that the requests of some process are redirected to
	 * it.
	 *
	 * We redirect bfqq to new_bfqq and not the opposite, because we
	 * are in the context of the process owning bfqq, hence we have
	 * the io_cq of this process. So we can immediately configure this
	 * io_cq to redirect the requests of the process to new_bfqq.
	 *
	 * NOTE, even if new_bfqq coincides with the in-service queue, the
	 * io_cq of new_bfqq is not available, because, if the in-service
	 * queue is shared, bfqd->in_service_bic may not point to the
	 * io_cq of the in-service queue.
	 * Redirecting the requests of the process owning bfqq to the
	 * currently in-service queue is in any case the best option, as
	 * we feed the in-service queue with new requests close to the
	 * last request served and, by doing so, hopefully increase the
	 * throughput.
	 */
	bfqq->new_bfqq = new_bfqq;
	new_bfqq->ref += process_refs;
	return new_bfqq;
}

static bool bfq_may_be_close_cooperator(struct bfq_queue *bfqq,
					struct bfq_queue *new_bfqq)
{
	if (bfq_class_idle(bfqq) || bfq_class_idle(new_bfqq) ||
	    (bfqq->ioprio_class != new_bfqq->ioprio_class))
		return false;

	/*
	 * If either of the queues has already been detected as seeky,
	 * then merging it with the other queue is unlikely to lead to
	 * sequential I/O.
	 */
	if (BFQQ_SEEKY(bfqq) || BFQQ_SEEKY(new_bfqq))
		return false;

	/*
	 * Interleaved I/O is known to be done by (some) applications
	 * only for reads, so it does not make sense to merge async
	 * queues.
	 */
	if (!bfq_bfqq_sync(bfqq) || !bfq_bfqq_sync(new_bfqq))
		return false;

	return true;
}

/*
 * If this function returns true, then bfqq cannot be merged. The idea
 * is that true cooperation happens very early after processes start
 * to do I/O. Usually, late cooperations are just accidental false
 * positives. In case bfqq is weight-raised, such false positives
 * would evidently degrade latency guarantees for bfqq.
 */
bool wr_from_too_long(struct bfq_queue *bfqq)
{
	return bfqq->wr_coeff > 1 &&
		time_is_before_jiffies(bfqq->last_wr_start_finish +
				       msecs_to_jiffies(100));
}

/*
 * Attempt to schedule a merge of bfqq with the currently in-service
 * queue or with a close queue among the scheduled queues.  Return
 * NULL if no merge was scheduled, a pointer to the shared bfq_queue
 * structure otherwise.
 *
 * The OOM queue is not allowed to participate to cooperation: in fact, since
 * the requests temporarily redirected to the OOM queue could be redirected
 * again to dedicated queues at any time, the state needed to correctly
 * handle merging with the OOM queue would be quite complex and expensive
 * to maintain. Besides, in such a critical condition as an out of memory,
 * the benefits of queue merging may be little relevant, or even negligible.
 *
 * Weight-raised queues can be merged only if their weight-raising
 * period has just started. In fact cooperating processes are usually
 * started together. Thus, with this filter we avoid false positives
 * that would jeopardize low-latency guarantees.
 *
 * WARNING: queue merging may impair fairness among non-weight raised
 * queues, for at least two reasons: 1) the original weight of a
 * merged queue may change during the merged state, 2) even being the
 * weight the same, a merged queue may be bloated with many more
 * requests than the ones produced by its originally-associated
 * process.
 */
static struct bfq_queue *
bfq_setup_cooperator(struct bfq_data *bfqd, struct bfq_queue *bfqq,
		     void *io_struct, bool request)
{
	struct bfq_queue *in_service_bfqq, *new_bfqq;

	if (bfqq->new_bfqq)
		return bfqq->new_bfqq;

	if (io_struct && wr_from_too_long(bfqq) &&
	    likely(bfqq != &bfqd->oom_bfqq))
		bfq_log_bfqq(bfqd, bfqq,
			     "would have looked for coop, but bfq%d wr",
			bfqq->pid);

	if (!io_struct ||
	    wr_from_too_long(bfqq) ||
	    unlikely(bfqq == &bfqd->oom_bfqq))
		return NULL;

	/* If there is only one backlogged queue, don't search. */
	if (bfqd->busy_queues == 1)
		return NULL;

	in_service_bfqq = bfqd->in_service_queue;

	if (in_service_bfqq && in_service_bfqq != bfqq &&
	    bfqd->in_service_bic && wr_from_too_long(in_service_bfqq)
	    && likely(in_service_bfqq == &bfqd->oom_bfqq))
		bfq_log_bfqq(bfqd, bfqq,
		"would have tried merge with in-service-queue, but wr");

	if (!in_service_bfqq || in_service_bfqq == bfqq ||
	    !bfqd->in_service_bic || wr_from_too_long(in_service_bfqq) ||
	    unlikely(in_service_bfqq == &bfqd->oom_bfqq))
		goto check_scheduled;

	if (bfq_rq_close_to_sector(io_struct, request, bfqd->last_position) &&
	    bfqq->entity.parent == in_service_bfqq->entity.parent &&
	    bfq_may_be_close_cooperator(bfqq, in_service_bfqq)) {
		new_bfqq = bfq_setup_merge(bfqq, in_service_bfqq);
		if (new_bfqq)
			return new_bfqq;
	}
	/*
	 * Check whether there is a cooperator among currently scheduled
	 * queues. The only thing we need is that the bio/request is not
	 * NULL, as we need it to establish whether a cooperator exists.
	 */
check_scheduled:
	new_bfqq = bfq_find_close_cooperator(bfqd, bfqq,
			bfq_io_struct_pos(io_struct, request));

	BUG_ON(new_bfqq && bfqq->entity.parent != new_bfqq->entity.parent);

	if (new_bfqq && wr_from_too_long(new_bfqq) &&
	    likely(new_bfqq != &bfqd->oom_bfqq) &&
	    bfq_may_be_close_cooperator(bfqq, new_bfqq))
		bfq_log_bfqq(bfqd, bfqq,
			     "would have merged with bfq%d, but wr",
			     new_bfqq->pid);

	if (new_bfqq && !wr_from_too_long(new_bfqq) &&
	    likely(new_bfqq != &bfqd->oom_bfqq) &&
	    bfq_may_be_close_cooperator(bfqq, new_bfqq))
		return bfq_setup_merge(bfqq, new_bfqq);

	return NULL;
}

static void bfq_bfqq_save_state(struct bfq_queue *bfqq)
{
	/*
	 * If !bfqq->bic, the queue is already shared or its requests
	 * have already been redirected to a shared queue; both idle window
	 * and weight raising state have already been saved. Do nothing.
	 */
	if (!bfqq->bic)
		return;

	bfqq->bic->saved_idle_window = bfq_bfqq_idle_window(bfqq);
	bfqq->bic->saved_IO_bound = bfq_bfqq_IO_bound(bfqq);
	bfqq->bic->saved_in_large_burst = bfq_bfqq_in_large_burst(bfqq);
	bfqq->bic->was_in_burst_list = !hlist_unhashed(&bfqq->burst_list_node);
}

static void bfq_get_bic_reference(struct bfq_queue *bfqq)
{
	/*
	 * If bfqq->bic has a non-NULL value, the bic to which it belongs
	 * is about to begin using a shared bfq_queue.
	 */
	if (bfqq->bic)
		atomic_long_inc(&bfqq->bic->icq.ioc->refcount);
}

static void
bfq_merge_bfqqs(struct bfq_data *bfqd, struct bfq_io_cq *bic,
		struct bfq_queue *bfqq, struct bfq_queue *new_bfqq)
{
	bfq_log_bfqq(bfqd, bfqq, "merging with queue %lu",
		(long unsigned)new_bfqq->pid);
	/* Save weight raising and idle window of the merged queues */
	bfq_bfqq_save_state(bfqq);
	bfq_bfqq_save_state(new_bfqq);
	if (bfq_bfqq_IO_bound(bfqq))
		bfq_mark_bfqq_IO_bound(new_bfqq);
	bfq_clear_bfqq_IO_bound(bfqq);

	/*
	 * If bfqq is weight-raised, then let new_bfqq inherit
	 * weight-raising. To reduce false positives, neglect the case
	 * where bfqq has just been created, but has not yet made it
	 * to be weight-raised (which may happen because EQM may merge
	 * bfqq even before bfq_add_request is executed for the first
	 * time for bfqq). Handling this case would however be very
	 * easy, thanks to the flag just_created.
	 */
	if (new_bfqq->wr_coeff == 1 && bfqq->wr_coeff > 1) {
		new_bfqq->wr_coeff = bfqq->wr_coeff;
		new_bfqq->wr_cur_max_time = bfqq->wr_cur_max_time;
		new_bfqq->last_wr_start_finish = bfqq->last_wr_start_finish;
		if (bfq_bfqq_busy(new_bfqq))
			bfqd->wr_busy_queues++;
		new_bfqq->entity.prio_changed = 1;
		bfq_log_bfqq(bfqd, new_bfqq,
			     "wr start after merge with %d, rais_max_time %u",
			     bfqq->pid,
			     jiffies_to_msecs(bfqq->wr_cur_max_time));
	}

	if (bfqq->wr_coeff > 1) { /* bfqq has given its wr to new_bfqq */
		bfqq->wr_coeff = 1;
		bfqq->entity.prio_changed = 1;
		if (bfq_bfqq_busy(bfqq))
			bfqd->wr_busy_queues--;
	}

	bfq_log_bfqq(bfqd, new_bfqq, "merge_bfqqs: wr_busy %d",
		     bfqd->wr_busy_queues);

	/*
	 * Grab a reference to the bic, to prevent it from being destroyed
	 * before being possibly touched by a bfq_split_bfqq().
	 */
	bfq_get_bic_reference(bfqq);
	bfq_get_bic_reference(new_bfqq);
	/*
	 * Merge queues (that is, let bic redirect its requests to new_bfqq)
	 */
	bic_set_bfqq(bic, new_bfqq, 1);
	bfq_mark_bfqq_coop(new_bfqq);
	/*
	 * new_bfqq now belongs to at least two bics (it is a shared queue):
	 * set new_bfqq->bic to NULL. bfqq either:
	 * - does not belong to any bic any more, and hence bfqq->bic must
	 *   be set to NULL, or
	 * - is a queue whose owning bics have already been redirected to a
	 *   different queue, hence the queue is destined to not belong to
	 *   any bic soon and bfqq->bic is already NULL (therefore the next
	 *   assignment causes no harm).
	 */
	new_bfqq->bic = NULL;
	bfqq->bic = NULL;
	bfq_put_queue(bfqq);
}

static int bfq_allow_merge(struct request_queue *q, struct request *rq,
			   struct bio *bio)
{
	struct bfq_data *bfqd = q->elevator->elevator_data;
	struct bfq_io_cq *bic;
	struct bfq_queue *bfqq, *new_bfqq;

	/*
	 * Disallow merge of a sync bio into an async request.
	 */
	if (bfq_bio_sync(bio) && !rq_is_sync(rq))
		return 0;

	/*
	 * Lookup the bfqq that this bio will be queued with. Allow
	 * merge only if rq is queued there.
	 * Queue lock is held here.
	 */
	bic = bfq_bic_lookup(bfqd, current->io_context);
	if (!bic)
		return 0;

	bfqq = bic_to_bfqq(bic, bfq_bio_sync(bio));
	/*
	 * We take advantage of this function to perform an early merge
	 * of the queues of possible cooperating processes.
	 */
	if (bfqq) {
		new_bfqq = bfq_setup_cooperator(bfqd, bfqq, bio, false);
		if (new_bfqq) {
			bfq_merge_bfqqs(bfqd, bic, bfqq, new_bfqq);
			/*
			 * If we get here, the bio will be queued in the
			 * shared queue, i.e., new_bfqq, so use new_bfqq
			 * to decide whether bio and rq can be merged.
			 */
			bfqq = new_bfqq;
		}
	}

	return bfqq == RQ_BFQQ(rq);
}

/*
 * Set the maximum time for the in-service queue to consume its
 * budget. This prevents seeky processes from lowering the throughput.
 * In practice, a time-slice service scheme is used with seeky
 * processes.
 */
static void bfq_set_budget_timeout(struct bfq_data *bfqd,
				   struct bfq_queue *bfqq)
{
	unsigned int timeout_coeff;

	if (bfqq->wr_cur_max_time == bfqd->bfq_wr_rt_max_time)
		timeout_coeff = 1;
	else
		timeout_coeff = bfqq->entity.weight / bfqq->entity.orig_weight;

	bfqd->last_budget_start = ktime_get();

	bfqq->budget_timeout = jiffies +
		bfqd->bfq_timeout * timeout_coeff;

	bfq_log_bfqq(bfqd, bfqq, "set budget_timeout %u",
		jiffies_to_msecs(bfqd->bfq_timeout * timeout_coeff));
}

static void __bfq_set_in_service_queue(struct bfq_data *bfqd,
				       struct bfq_queue *bfqq)
{
	if (bfqq) {
		bfqg_stats_update_avg_queue_size(bfqq_group(bfqq));
		bfq_mark_bfqq_must_alloc(bfqq);
		bfq_clear_bfqq_fifo_expire(bfqq);

		bfqd->budgets_assigned = (bfqd->budgets_assigned*7 + 256) / 8;

		BUG_ON(bfqq == bfqd->in_service_queue);
		BUG_ON(RB_EMPTY_ROOT(&bfqq->sort_list));

		if (bfqq->wr_coeff > 1 &&
		    bfqq->wr_cur_max_time == bfqd->bfq_wr_rt_max_time &&
			time_is_before_jiffies(bfqq->budget_timeout)) {
			/*
			 * For soft real-time queues, move the start
			 * of the weight-raising period forward by the
			 * time the queue has not received any
			 * service. Otherwise, a relatively long
			 * service delay is likely to cause the
			 * weight-raising period of the queue to end,
			 * because of the short duration of the
			 * weight-raising period of a soft real-time
			 * queue.  It is worth noting that this move
			 * is not so dangerous for the other queues,
			 * because soft real-time queues are not
			 * greedy.
			 *
			 * To not add a further variable, we use the
			 * overloaded field budget_timeout to
			 * determine for how long the queue has not
			 * received service, i.e., how much time has
			 * elapsed since the queue expired. However,
			 * this is a little imprecise, because
			 * budget_timeout is set to jiffies if bfqq
			 * not only expires, but also remains with no
			 * request.
			 */
			bfqq->last_wr_start_finish += jiffies -
				bfqq->budget_timeout;
		}

		bfq_set_budget_timeout(bfqd, bfqq);
		bfq_log_bfqq(bfqd, bfqq,
			     "set_in_service_queue, cur-budget = %d",
			     bfqq->entity.budget);
	} else
		bfq_log(bfqd, "set_in_service_queue: NULL");

	bfqd->in_service_queue = bfqq;
}

/*
 * Get and set a new queue for service.
 */
static struct bfq_queue *bfq_set_in_service_queue(struct bfq_data *bfqd)
{
	struct bfq_queue *bfqq = bfq_get_next_queue(bfqd);

	__bfq_set_in_service_queue(bfqd, bfqq);
	return bfqq;
}

static void bfq_arm_slice_timer(struct bfq_data *bfqd)
{
	struct bfq_queue *bfqq = bfqd->in_service_queue;
	struct bfq_io_cq *bic;
	u32 sl;

	BUG_ON(!RB_EMPTY_ROOT(&bfqq->sort_list));

	/* Processes have exited, don't wait. */
	bic = bfqd->in_service_bic;
	if (!bic || atomic_read(&bic->icq.ioc->active_ref) == 0)
		return;

	bfq_mark_bfqq_wait_request(bfqq);

	/*
	 * We don't want to idle for seeks, but we do want to allow
	 * fair distribution of slice time for a process doing back-to-back
	 * seeks. So allow a little bit of time for him to submit a new rq.
	 *
	 * To prevent processes with (partly) seeky workloads from
	 * being too ill-treated, grant them a small fraction of the
	 * assigned budget before reducing the waiting time to
	 * BFQ_MIN_TT. This happened to help reduce latency.
	 */
	sl = bfqd->bfq_slice_idle;
	/*
	 * Unless the queue is being weight-raised or the scenario is
	 * asymmetric, grant only minimum idle time if the queue
	 * is seeky. A long idling is preserved for a weight-raised
	 * queue, or, more in general, in an asymemtric scenario,
	 * because a long idling is needed for guaranteeing to a queue
	 * its reserved share of the throughput (in particular, it is
	 * needed if the queue has a higher weight than some other
	 * queue).
	 */
	if (BFQQ_SEEKY(bfqq) && bfqq->wr_coeff == 1 &&
	    bfq_symmetric_scenario(bfqd))
		sl = min_t(u32, sl, BFQ_MIN_TT);

	bfqd->last_idling_start = ktime_get();
	hrtimer_start(&bfqd->idle_slice_timer, ns_to_ktime(sl),
		      HRTIMER_MODE_REL);
	bfqg_stats_set_start_idle_time(bfqq_group(bfqq));
	bfq_log(bfqd, "arm idle: %ld/%ld ms",
		sl / NSEC_PER_MSEC, bfqd->bfq_slice_idle / NSEC_PER_MSEC);
}

/*
 * In autotuning mode, max_budget is dynamically recomputed as the
 * amount of sectors transferred in timeout at the estimated peak
 * rate. This enables BFQ to utilize a full timeslice with a full
 * budget, even if the in-service queue is served at peak rate. And
 * this maximises throughput with sequential workloads.
 */
static unsigned long bfq_calc_max_budget(struct bfq_data *bfqd)
{
	return (u64)bfqd->peak_rate * USEC_PER_MSEC *
		jiffies_to_msecs(bfqd->bfq_timeout)>>BFQ_RATE_SHIFT;
}

/*
 * Update parameters related to throughput and responsiveness, as a
 * function of the estimated peak rate. See comments on
 * bfq_calc_max_budget(), and on T_slow and T_fast arrays.
 */
void update_thr_responsiveness_params(struct bfq_data *bfqd)
{
	int dev_type = blk_queue_nonrot(bfqd->queue);

	if (bfqd->bfq_user_max_budget == 0) {
		bfqd->bfq_max_budget =
			bfq_calc_max_budget(bfqd);
		BUG_ON(bfqd->bfq_max_budget < 0);
		bfq_log(bfqd, "new max_budget = %d",
			bfqd->bfq_max_budget);
	}

	if (bfqd->device_speed == BFQ_BFQD_FAST &&
	    bfqd->peak_rate < device_speed_thresh[dev_type]) {
		bfqd->device_speed = BFQ_BFQD_SLOW;
		bfqd->RT_prod = R_slow[dev_type] *
			T_slow[dev_type];
	} else if (bfqd->device_speed == BFQ_BFQD_SLOW &&
		   bfqd->peak_rate > device_speed_thresh[dev_type]) {
		bfqd->device_speed = BFQ_BFQD_FAST;
		bfqd->RT_prod = R_fast[dev_type] *
			T_fast[dev_type];
	}

	bfq_log(bfqd,
"dev_type %s dev_speed_class = %s (%llu sects/sec), thresh %llu setcs/sec",
		dev_type == 0 ? "ROT" : "NONROT",
		bfqd->device_speed == BFQ_BFQD_FAST ? "FAST" : "SLOW",
		bfqd->device_speed == BFQ_BFQD_FAST ?
		(USEC_PER_SEC*(u64)R_fast[dev_type])>>BFQ_RATE_SHIFT :
		(USEC_PER_SEC*(u64)R_slow[dev_type])>>BFQ_RATE_SHIFT,
		(USEC_PER_SEC*(u64)device_speed_thresh[dev_type])>>
		BFQ_RATE_SHIFT);
}

void bfq_reset_rate_computation(struct bfq_data *bfqd, struct request *rq)
{
	if (rq != NULL) { /* new rq dispatch now, reset accordingly */
		bfqd->last_dispatch = bfqd->first_dispatch = ktime_get_ns() ;
		bfqd->peak_rate_samples = 1;
		bfqd->sequential_samples = 0;
		bfqd->tot_sectors_dispatched = bfqd->last_rq_max_size =
			blk_rq_sectors(rq);
	} else /* no new rq dispatched, just reset the number of samples */
		bfqd->peak_rate_samples = 0; /* full re-init on next disp. */

	bfq_log(bfqd,
		"reset_rate_computation at end, sample %u/%u size %llu",
		bfqd->peak_rate_samples, bfqd->sequential_samples,
		bfqd->tot_sectors_dispatched);
}

void bfq_update_rate_reset(struct bfq_data *bfqd, struct request *rq)
{
	u32 bw, weight, divisor;

	/*
	 * For the convergence property to hold (see comments on
	 * bfq_update_peak_rate()) and for the assessment to be
	 * reliable, a minimum number of samples must be present, and
	 * a minimum amount of time must have elapsed. If not so, do
	 * not compute new rate. Just reset parameters, to get ready
	 * for a new evaluation attempt.
	 */
	if (bfqd->peak_rate_samples < BFQ_RATE_MIN_SAMPLES ||
		bfqd->delta_from_first_us < BFQ_RATE_MIN_INTERVAL) {
		bfq_log(bfqd,
	"update_rate_reset: only resetting, delta_first %uus samples %d",
			bfqd->delta_from_first_us, bfqd->peak_rate_samples);
		goto reset_computation;
	}

	/*
	 * If a new request completion has occurred after last
	 * dispatch, then, to approximate the rate at which requests
	 * have been served by the device, it is more precise to
	 * extend the observation interval to the last completion.
	 */
	bfqd->delta_from_first_us =
		max_t(u64, bfqd->delta_from_first_us,
			(bfqd->last_completion - bfqd->first_dispatch)/
			NSEC_PER_USEC);

	BUG_ON(bfqd->delta_from_first_us == 0);
	bw = div_u64(bfqd->tot_sectors_dispatched<<BFQ_RATE_SHIFT,
		     bfqd->delta_from_first_us);

	bfq_log(bfqd,
	"update_rate_reset: size %llu delta_first %uus bw %llu sects/s (%d)",
		bfqd->tot_sectors_dispatched, bfqd->delta_from_first_us,
		((USEC_PER_SEC*(u64)bw)>>BFQ_RATE_SHIFT),
		bw > 20<<BFQ_RATE_SHIFT);

	/*
	 * Peak rate not updated if:
	 * - the percentage of sequential dispatches is below 3/4 of the
	 *   total, and bw is below the current estimated peak rate
	 * - bw is unreasonably high (> 20M sectors/sec)
	 */
	if ((bfqd->peak_rate_samples > (3 * bfqd->sequential_samples)>>2 &&
	     bw <= bfqd->peak_rate) ||
		bw > 20<<BFQ_RATE_SHIFT) {
		bfq_log(bfqd,
"update_rate_reset: goto reset, samples %u/%u bw/peak %llu/%llu",
		bfqd->peak_rate_samples, bfqd->sequential_samples,
		((USEC_PER_SEC*(u64)bw)>>BFQ_RATE_SHIFT),
		((USEC_PER_SEC*(u64)bfqd->peak_rate)>>BFQ_RATE_SHIFT));
		goto reset_computation;
	} else {
		bfq_log(bfqd,
"update_rate_reset: do update, samples %u/%u bw/peak %llu/%llu",
		bfqd->peak_rate_samples, bfqd->sequential_samples,
		((USEC_PER_SEC*(u64)bw)>>BFQ_RATE_SHIFT),
		((USEC_PER_SEC*(u64)bfqd->peak_rate)>>BFQ_RATE_SHIFT));
	}

	/*
	 * We have to update the peak rate, at last! To this purpose,
	 * we use a low-pass filter. We compute the smoothing constant
	 * of the filter as a function of the 'weight' of the new
	 * measured rate.
	 *
	 * As can be seen in next formulas, we define this weight as a
	 * quantity proportional to how sequential the workload is,
	 * and to how long the observation time interval is.
	 *
	 * The weight runs from 0 to 8. The maximum value of the
	 * weight, 8, yields the minimum value for the smoothing
	 * constant. At this minimum value for the smoothing constant,
	 * the measured rate contributes for half of the next value of
	 * the estimated peak rate.
	 *
	 * So, the first step is to compute the weight as a function
	 * of how sequential the workload is. Note that the weight
	 * cannot reach 9, because bfqd->sequential_samples cannot
	 * become equal to bfqd->peak_rate_samples, which, in its
	 * turn, holds true because bfqd->sequential_samples is not
	 * incremented for the first sample.
	 */
	weight = (9 * bfqd->sequential_samples) / bfqd->peak_rate_samples;

	/*
	 * Second step: further refine the weight as a function of the
	 * duration of the observation interval.
	 */
	weight = min_t(u32, 8,
		       (weight * bfqd->delta_from_first_us) /
		       BFQ_RATE_REF_INTERVAL);

	/*
	 * Divisor ranging from 10, for minimum weight, to 2, for
	 * maximum weight.
	 */
	divisor = 10 - weight;
	BUG_ON(divisor == 0);

	/*
	 * Finally, update peak rate:
	 *
	 * peak_rate = peak_rate * (divisor-1) / divisor  +  bw / divisor
	 */
	bfqd->peak_rate *= divisor-1;
	bfqd->peak_rate /= divisor;
	bw /= divisor; /* smoothing constant alpha = 1/divisor */

	bfq_log(bfqd,
		"update_rate_reset: divisor %d tmp_peak_rate %llu tmp_bw %u",
		divisor,
		((USEC_PER_SEC*(u64)bfqd->peak_rate)>>BFQ_RATE_SHIFT),
		(u32)((USEC_PER_SEC*(u64)bw)>>BFQ_RATE_SHIFT));

	BUG_ON(bfqd->peak_rate == 0);
	BUG_ON(bfqd->peak_rate > 20<<BFQ_RATE_SHIFT);

	bfqd->peak_rate += bw;
	update_thr_responsiveness_params(bfqd);
	BUG_ON(bfqd->peak_rate > 20<<BFQ_RATE_SHIFT);

reset_computation:
	bfq_reset_rate_computation(bfqd, rq);
}

/*
 * Update the read/write peak rate (the main quantity used for
 * auto-tuning, see update_thr_responsiveness_params()).
 *
 * It is not trivial to estimate the peak rate (correctly): because of
 * the presence of sw and hw queues between the scheduler and the
 * device components that finally serve I/O requests, it is hard to
 * say exactly when a given dispatched request is served inside the
 * device, and for how long. As a consequence, it is hard to know
 * precisely at what rate a given set of requests is actually served
 * by the device.
 *
 * On the opposite end, the dispatch time of any request is trivially
 * available, and, from this piece of information, the "dispatch rate"
 * of requests can be immediately computed. So, the idea in the next
 * function is to use what is known, namely request dispatch times
 * (plus, when useful, request completion times), to estimate what is
 * unknown, namely in-device request service rate.
 *
 * The main issue is that, because of the above facts, the rate at
 * which a certain set of requests is dispatched over a certain time
 * interval can vary greatly with respect to the rate at which the
 * same requests are then served. But, since the size of any
 * intermediate queue is limited, and the service scheme is lossless
 * (no request is silently dropped), the following obvious convergence
 * property holds: the number of requests dispatched MUST become
 * closer and closer to the number of requests completed as the
 * observation interval grows. This is the key property used in
 * the next function to estimate the peak service rate as a function
 * of the observed dispatch rate. The function assumes to be invoked
 * on every request dispatch.
 */
void bfq_update_peak_rate(struct bfq_data *bfqd, struct request *rq)
{
	u64 now_ns = ktime_get_ns();

	if (bfqd->peak_rate_samples == 0) { /* first dispatch */
		bfq_log(bfqd,
		"update_peak_rate: goto reset, samples %d",
				bfqd->peak_rate_samples) ;
		bfq_reset_rate_computation(bfqd, rq);
		goto update_last_values; /* will add one sample */
	}

	/*
	 * Device idle for very long: the observation interval lasting
	 * up to this dispatch cannot be a valid observation interval
	 * for computing a new peak rate (similarly to the late-
	 * completion event in bfq_completed_request()). Go to
	 * update_rate_and_reset to have the following three steps
	 * taken:
	 * - close the observation interval at the last (previous)
	 *   request dispatch or completion
	 * - compute rate, if possible, for that observation interval
	 * - start a new observation interval with this dispatch
	 */
	if (now_ns - bfqd->last_dispatch > 100*NSEC_PER_MSEC &&
	    bfqd->rq_in_driver == 0) {
		bfq_log(bfqd,
"update_peak_rate: jumping to updating&resetting delta_last %lluus samples %d",
			(now_ns - bfqd->last_dispatch)/NSEC_PER_USEC,
			bfqd->peak_rate_samples) ;
		goto update_rate_and_reset;
	}

	/* Update sampling information */
	bfqd->peak_rate_samples++;

	if ((bfqd->rq_in_driver > 0 ||
		now_ns - bfqd->last_completion < BFQ_MIN_TT)
	     && get_sdist(bfqd->last_position, rq) < BFQQ_SEEK_THR)
		bfqd->sequential_samples++;

	bfqd->tot_sectors_dispatched += blk_rq_sectors(rq);

	/* Reset max observed rq size every 32 dispatches */
	if (likely(bfqd->peak_rate_samples % 32))
		bfqd->last_rq_max_size =
			max_t(u32, blk_rq_sectors(rq), bfqd->last_rq_max_size);
	else
		bfqd->last_rq_max_size = blk_rq_sectors(rq);

	bfqd->delta_from_first_us = (now_ns - bfqd->first_dispatch)/NSEC_PER_USEC;

	bfq_log(bfqd,
	"update_peak_rate: added samples %u/%u size %llu delta_first_us %u",
		bfqd->peak_rate_samples, bfqd->sequential_samples,
		bfqd->tot_sectors_dispatched,
		bfqd->delta_from_first_us);

	/* Target observation interval not yet reached, go on sampling */
	if (bfqd->delta_from_first_us < BFQ_RATE_REF_INTERVAL)
		goto update_last_values;

update_rate_and_reset:
	bfq_update_rate_reset(bfqd, rq);
update_last_values:
	bfqd->last_position = blk_rq_pos(rq) + blk_rq_sectors(rq);
	bfqd->last_dispatch = now_ns;

	bfq_log(bfqd,
	"update_peak_rate: delta_first %lluus last_pos %llu peak_rate %llu",
		(now_ns - bfqd->first_dispatch)/NSEC_PER_USEC,
		(unsigned long long) bfqd->last_position,
		((USEC_PER_SEC*(u64)bfqd->peak_rate)>>BFQ_RATE_SHIFT));
	bfq_log(bfqd,
	"update_peak_rate: samples at end %d", bfqd->peak_rate_samples);
}

/*
 * Move request from internal lists to the dispatch list of the request queue
 */
static void bfq_dispatch_insert(struct request_queue *q, struct request *rq)
{
	struct bfq_queue *bfqq = RQ_BFQQ(rq);

	/*
	 * For consistency, the next instruction should have been executed
	 * after removing the request from the queue and dispatching it.
	 * We execute instead this instruction before bfq_remove_request()
	 * (and hence introduce a temporary inconsistency), for efficiency.
	 * In fact, in a forced_dispatch, this prevents two counters related
	 * to bfqq->dispatched to risk to be uselessly decremented if bfqq
	 * is not in service, and then to be incremented again after
	 * incrementing bfqq->dispatched.
	 */
	bfqq->dispatched++;
	bfq_update_peak_rate(q->elevator->elevator_data, rq);

	bfq_remove_request(rq);
	elv_dispatch_sort(q, rq);
}

/*
 * Return expired entry, or NULL to just start from scratch in rbtree.
 */
static struct request *bfq_check_fifo(struct bfq_queue *bfqq)
{
	struct request *rq = NULL;

	if (bfq_bfqq_fifo_expire(bfqq))
		return NULL;

	bfq_mark_bfqq_fifo_expire(bfqq);

	if (list_empty(&bfqq->fifo))
		return NULL;

	rq = rq_entry_fifo(bfqq->fifo.next);

	if (time_is_after_jiffies(rq->fifo_time))
		return NULL;

	return rq;
}

static void __bfq_bfqq_expire(struct bfq_data *bfqd, struct bfq_queue *bfqq)
{
	BUG_ON(bfqq != bfqd->in_service_queue);

	__bfq_bfqd_reset_in_service(bfqd);

	/*
	 * If this bfqq is shared between multiple processes, check
	 * to make sure that those processes are still issuing I/Os
	 * within the mean seek distance. If not, it may be time to
	 * break the queues apart again.
	 */
	if (bfq_bfqq_coop(bfqq) && BFQQ_SEEKY(bfqq))
		bfq_mark_bfqq_split_coop(bfqq);

	if (RB_EMPTY_ROOT(&bfqq->sort_list)) {
		if (bfqq->dispatched == 0)
			/*
			 * Overloading budget_timeout field to store
			 * the time at which the queue remains with no
			 * backlog and no outstanding request; used by
			 * the weight-raising mechanism.
			 */
			bfqq->budget_timeout = jiffies;

		bfq_del_bfqq_busy(bfqd, bfqq, 1);
	} else {
		bfq_activate_bfqq(bfqd, bfqq);
		/*
		 * Resort priority tree of potential close cooperators.
		 */
		bfq_pos_tree_add_move(bfqd, bfqq);
	}
}

/**
 * __bfq_bfqq_recalc_budget - try to adapt the budget to the @bfqq behavior.
 * @bfqd: device data.
 * @bfqq: queue to update.
 * @reason: reason for expiration.
 *
 * Handle the feedback on @bfqq budget at queue expiration.
 * See the body for detailed comments.
 */
static void __bfq_bfqq_recalc_budget(struct bfq_data *bfqd,
				     struct bfq_queue *bfqq,
				     enum bfqq_expiration reason)
{
	struct request *next_rq;
	int budget, min_budget;

	BUG_ON(bfqq != bfqd->in_service_queue);

	min_budget = bfq_min_budget(bfqd);

	if (bfqq->wr_coeff == 1)
		budget = bfqq->max_budget;
	else /*
	      * Use a constant, low budget for weight-raised queues,
	      * to help achieve a low latency. Keep it slightly higher
	      * than the minimum possible budget, to cause a little
	      * bit fewer expirations.
	      */
		budget = 2 * min_budget;

	bfq_log_bfqq(bfqd, bfqq, "recalc_budg: last budg %d, budg left %d",
		bfqq->entity.budget, bfq_bfqq_budget_left(bfqq));
	bfq_log_bfqq(bfqd, bfqq, "recalc_budg: last max_budg %d, min budg %d",
		budget, bfq_min_budget(bfqd));
	bfq_log_bfqq(bfqd, bfqq, "recalc_budg: sync %d, seeky %d",
		bfq_bfqq_sync(bfqq), BFQQ_SEEKY(bfqd->in_service_queue));

	if (bfq_bfqq_sync(bfqq) && bfqq->wr_coeff == 1) {
		switch (reason) {
		/*
		 * Caveat: in all the following cases we trade latency
		 * for throughput.
		 */
		case BFQ_BFQQ_TOO_IDLE:
			/*
			 * This is the only case where we may reduce
			 * the budget: if there is no request of the
			 * process still waiting for completion, then
			 * we assume (tentatively) that the timer has
			 * expired because the batch of requests of
			 * the process could have been served with a
			 * smaller budget.  Hence, betting that
			 * process will behave in the same way when it
			 * becomes backlogged again, we reduce its
			 * next budget.  As long as we guess right,
			 * this budget cut reduces the latency
			 * experienced by the process.
			 *
			 * However, if there are still outstanding
			 * requests, then the process may have not yet
			 * issued its next request just because it is
			 * still waiting for the completion of some of
			 * the still outstanding ones.  So in this
			 * subcase we do not reduce its budget, on the
			 * contrary we increase it to possibly boost
			 * the throughput, as discussed in the
			 * comments to the BUDGET_TIMEOUT case.
			 */
			if (bfqq->dispatched > 0) /* still outstanding reqs */
				budget = min(budget * 2, bfqd->bfq_max_budget);
			else {
				if (budget > 5 * min_budget)
					budget -= 4 * min_budget;
				else
					budget = min_budget;
			}
			break;
		case BFQ_BFQQ_BUDGET_TIMEOUT:
			/*
			 * We double the budget here because it gives
			 * the chance to boost the throughput if this
			 * is not a seeky process (and has bumped into
			 * this timeout because of, e.g., ZBR).
			 */
			budget = min(budget * 2, bfqd->bfq_max_budget);
			break;
		case BFQ_BFQQ_BUDGET_EXHAUSTED:
			/*
			 * The process still has backlog, and did not
			 * let either the budget timeout or the disk
			 * idling timeout expire. Hence it is not
			 * seeky, has a short thinktime and may be
			 * happy with a higher budget too. So
			 * definitely increase the budget of this good
			 * candidate to boost the disk throughput.
			 */
			budget = min(budget * 4, bfqd->bfq_max_budget);
			break;
		case BFQ_BFQQ_NO_MORE_REQUESTS:
			/*
			 * For queues that expire for this reason, it
			 * is particularly important to keep the
			 * budget close to the actual service they
			 * need. Doing so reduces the timestamp
			 * misalignment problem described in the
			 * comments in the body of
			 * __bfq_activate_entity. In fact, suppose
			 * that a queue systematically expires for
			 * BFQ_BFQQ_NO_MORE_REQUESTS and presents a
			 * new request in time to enjoy timestamp
			 * back-shifting. The larger the budget of the
			 * queue is with respect to the service the
			 * queue actually requests in each service
			 * slot, the more times the queue can be
			 * reactivated with the same virtual finish
			 * time. It follows that, even if this finish
			 * time is pushed to the system virtual time
			 * to reduce the consequent timestamp
			 * misalignment, the queue unjustly enjoys for
			 * many re-activations a lower finish time
			 * than all newly activated queues.
			 *
			 * The service needed by bfqq is measured
			 * quite precisely by bfqq->entity.service.
			 * Since bfqq does not enjoy device idling,
			 * bfqq->entity.service is equal to the number
			 * of sectors that the process associated with
			 * bfqq requested to read/write before waiting
			 * for request completions, or blocking for
			 * other reasons.
			 */
			budget = max_t(int, bfqq->entity.service, min_budget);
			break;
		default:
			return;
		}
	} else if (!bfq_bfqq_sync(bfqq))
		/*
		 * Async queues get always the maximum possible
		 * budget, as for them we do not care about latency
		 * (in addition, their ability to dispatch is limited
		 * by the charging factor).
		 */
		budget = bfqd->bfq_max_budget;

	bfqq->max_budget = budget;

	if (bfqd->budgets_assigned >= bfq_stats_min_budgets &&
	    !bfqd->bfq_user_max_budget)
		bfqq->max_budget = min(bfqq->max_budget, bfqd->bfq_max_budget);

	/*
	 * If there is still backlog, then assign a new budget, making
	 * sure that it is large enough for the next request.  Since
	 * the finish time of bfqq must be kept in sync with the
	 * budget, be sure to call __bfq_bfqq_expire() *after* this
	 * update.
	 *
	 * If there is no backlog, then no need to update the budget;
	 * it will be updated on the arrival of a new request.
	 */
	next_rq = bfqq->next_rq;
	if (next_rq) {
		BUG_ON(reason == BFQ_BFQQ_TOO_IDLE ||
		       reason == BFQ_BFQQ_NO_MORE_REQUESTS);
		bfqq->entity.budget = max_t(unsigned long, bfqq->max_budget,
					    bfq_serv_to_charge(next_rq, bfqq));
		BUG_ON(!bfq_bfqq_busy(bfqq));
		BUG_ON(RB_EMPTY_ROOT(&bfqq->sort_list));
	}

	bfq_log_bfqq(bfqd, bfqq, "head sect: %u, new budget %d",
			next_rq ? blk_rq_sectors(next_rq) : 0,
			bfqq->entity.budget);
}

/*
 * Return true if the process associated with bfqq is "slow". The slow
 * flag is used, in addition to the budget timeout, to reduce the
 * amount of service provided to seeky processes, and thus reduce
 * their chances to lower the throughput. More details in the comments
 * on the function bfq_bfqq_expire().
 *
 * An important observation is in order: as discussed in the comments
 * on the function bfq_update_peak_rate(), with devices with internal
 * queues, it is hard if ever possible to know when and for how long
 * an I/O request is processed by the device (apart from the trivial
 * I/O pattern where a new request is dispatched only after the
 * previous one has been completed). This makes it hard to evaluate
 * the real rate at which the I/O requests of each bfq_queue are
 * served.  In fact, for an I/O scheduler like BFQ, serving a
 * bfq_queue means just dispatching its requests during its service
 * slot (i.e., until the budget of the queue is exhausted, or the
 * queue remains idle, or, finally, a timeout fires). But, during the
 * service slot of a bfq_queue, around 100 ms at most, the device may
 * be even still processing requests of bfq_queues served in previous
 * service slots. On the opposite end, the requests of the in-service
 * bfq_queue may be completed after the service slot of the queue
 * finishes.
 *
 * Anyway, unless more sophisticated solutions are used
 * (where possible), the sum of the sizes of the requests dispatched
 * during the service slot of a bfq_queue is probably the only
 * approximation available for the service received by the bfq_queue
 * during its service slot. And this sum is the quantity used in this
 * function to evaluate the I/O speed of a process.
 */
static bool bfq_bfqq_is_slow(struct bfq_data *bfqd, struct bfq_queue *bfqq,
				 bool compensate, enum bfqq_expiration reason,
				 unsigned long *delta_ms)
{
	ktime_t delta_ktime;
	u64 delta_usecs;
	bool slow = BFQQ_SEEKY(bfqq); /* if delta too short, use seekyness */

	if (!bfq_bfqq_sync(bfqq))
		return false;

	if (compensate)
		delta_ktime = bfqd->last_idling_start;
	else
		delta_ktime = ktime_get();
	delta_ktime = ktime_sub(delta_ktime, bfqd->last_budget_start);
	delta_usecs = ktime_to_us(delta_ktime);

	/* don't trust short/unrealistic values. */
	if (delta_usecs < 1000 || delta_usecs >= LONG_MAX) {
		if (blk_queue_nonrot(bfqd->queue))
			 /*
			  * give same worst-case guarantees as idling
			  * for seeky
			  */
			*delta_ms = BFQ_MIN_TT / NSEC_PER_MSEC;
		else /* charge at least one seek */
			*delta_ms = bfq_slice_idle / NSEC_PER_MSEC;

		bfq_log(bfqd, "bfq_bfqq_is_slow: unrealistic %llu", delta_usecs);

		return slow;
	}

	*delta_ms = delta_usecs / USEC_PER_MSEC;

	/*
	 * Use only long (> 20ms) intervals to filter out excessive
	 * spikes in service rate estimation.
	 */
	if (delta_usecs > 20000) {
		/*
		 * Caveat for rotational devices: processes doing I/O
		 * in the slower disk zones tend to be slow(er) even
		 * if not seeky. In this respect, the estimated peak
		 * rate is likely to be an average over the disk
		 * surface. Accordingly, to not be too harsh with
		 * unlucky processes, a process is deemed slow only if
		 * its bw has been lower than half of the estimated
		 * peak rate.
		 */
		slow = bfqq->entity.service < bfqd->bfq_max_budget / 2;
		bfq_log(bfqd, "bfq_bfqq_is_slow: relative bw %d/%d",
			bfqq->entity.service, bfqd->bfq_max_budget);
	}

	bfq_log_bfqq(bfqd, bfqq, "bfq_bfqq_is_slow: slow %d", slow);

	return slow;
}

/*
 * To be deemed as soft real-time, an application must meet two
 * requirements. First, the application must not require an average
 * bandwidth higher than the approximate bandwidth required to playback or
 * record a compressed high-definition video.
 * The next function is invoked on the completion of the last request of a
 * batch, to compute the next-start time instant, soft_rt_next_start, such
 * that, if the next request of the application does not arrive before
 * soft_rt_next_start, then the above requirement on the bandwidth is met.
 *
 * The second requirement is that the request pattern of the application is
 * isochronous, i.e., that, after issuing a request or a batch of requests,
 * the application stops issuing new requests until all its pending requests
 * have been completed. After that, the application may issue a new batch,
 * and so on.
 * For this reason the next function is invoked to compute
 * soft_rt_next_start only for applications that meet this requirement,
 * whereas soft_rt_next_start is set to infinity for applications that do
 * not.
 *
 * Unfortunately, even a greedy application may happen to behave in an
 * isochronous way if the CPU load is high. In fact, the application may
 * stop issuing requests while the CPUs are busy serving other processes,
 * then restart, then stop again for a while, and so on. In addition, if
 * the disk achieves a low enough throughput with the request pattern
 * issued by the application (e.g., because the request pattern is random
 * and/or the device is slow), then the application may meet the above
 * bandwidth requirement too. To prevent such a greedy application to be
 * deemed as soft real-time, a further rule is used in the computation of
 * soft_rt_next_start: soft_rt_next_start must be higher than the current
 * time plus the maximum time for which the arrival of a request is waited
 * for when a sync queue becomes idle, namely bfqd->bfq_slice_idle.
 * This filters out greedy applications, as the latter issue instead their
 * next request as soon as possible after the last one has been completed
 * (in contrast, when a batch of requests is completed, a soft real-time
 * application spends some time processing data).
 *
 * Unfortunately, the last filter may easily generate false positives if
 * only bfqd->bfq_slice_idle is used as a reference time interval and one
 * or both the following cases occur:
 * 1) HZ is so low that the duration of a jiffy is comparable to or higher
 *    than bfqd->bfq_slice_idle. This happens, e.g., on slow devices with
 *    HZ=100.
 * 2) jiffies, instead of increasing at a constant rate, may stop increasing
 *    for a while, then suddenly 'jump' by several units to recover the lost
 *    increments. This seems to happen, e.g., inside virtual machines.
 * To address this issue, we do not use as a reference time interval just
 * bfqd->bfq_slice_idle, but bfqd->bfq_slice_idle plus a few jiffies. In
 * particular we add the minimum number of jiffies for which the filter
 * seems to be quite precise also in embedded systems and KVM/QEMU virtual
 * machines.
 */
static unsigned long bfq_bfqq_softrt_next_start(struct bfq_data *bfqd,
						struct bfq_queue *bfqq)
{
	bfq_log_bfqq(bfqd, bfqq,
"softrt_next_start: service_blkg %lu soft_rate %u sects/sec interval %u",
		     bfqq->service_from_backlogged,
		     bfqd->bfq_wr_max_softrt_rate,
		     jiffies_to_msecs(HZ * bfqq->service_from_backlogged /
				      bfqd->bfq_wr_max_softrt_rate));

	return max(bfqq->last_idle_bklogged +
		   HZ * bfqq->service_from_backlogged /
		   bfqd->bfq_wr_max_softrt_rate,
		   jiffies + nsecs_to_jiffies(bfqq->bfqd->bfq_slice_idle) + 4);
}

/*
 * Return the farthest future time instant according to jiffies
 * macros.
 */
static unsigned long bfq_greatest_from_now(void)
{
	return jiffies + MAX_JIFFY_OFFSET;
}

/*
 * Return the farthest past time instant according to jiffies
 * macros.
 */
static unsigned long bfq_smallest_from_now(void)
{
	return jiffies - MAX_JIFFY_OFFSET;
}

/**
 * bfq_bfqq_expire - expire a queue.
 * @bfqd: device owning the queue.
 * @bfqq: the queue to expire.
 * @compensate: if true, compensate for the time spent idling.
 * @reason: the reason causing the expiration.
 *
 * If the process associated with bfqq does slow I/O (e.g., because it
 * issues random requests), we charge bfqq with the time it has been
 * in service instead of the service it has received (see
 * bfq_bfqq_charge_time for details on how this goal is achieved). As
 * a consequence, bfqq will typically get higher timestamps upon
 * reactivation, and hence it will be rescheduled as if it had
 * received more service than what it has actually received. In the
 * end, bfqq receives less service in proportion to how slowly its
 * associated process consumes its budgets (and hence how seriously it
 * tends to lower the throughput). In addition, this time-charging
 * strategy guarantees time fairness among slow processes. In
 * contrast, if the process associated with bfqq is not slow, we
 * charge bfqq exactly with the service it has received.
 *
 * Charging time to the first type of queues and the exact service to
 * the other has the effect of using the WF2Q+ policy to schedule the
 * former on a timeslice basis, without violating service domain
 * guarantees among the latter.
 */
static void bfq_bfqq_expire(struct bfq_data *bfqd,
			    struct bfq_queue *bfqq,
			    bool compensate,
			    enum bfqq_expiration reason)
{
	bool slow;
	unsigned long delta = 0;
	struct bfq_entity *entity = &bfqq->entity;

	BUG_ON(bfqq != bfqd->in_service_queue);

	/*
	 * Check whether the process is slow (see bfq_bfqq_is_slow).
	 */
	slow = bfq_bfqq_is_slow(bfqd, bfqq, compensate, reason, &delta);

	/*
	 * Increase service_from_backlogged before next statement,
	 * because the possible next invocation of
	 * bfq_bfqq_charge_time would likely inflate
	 * entity->service. In contrast, service_from_backlogged must
	 * contain real service, to enable the soft real-time
	 * heuristic to correctly compute the bandwidth consumed by
	 * bfqq.
	 */
	bfqq->service_from_backlogged += entity->service;

	/*
	 * As above explained, charge slow (typically seeky) and
	 * timed-out queues with the time and not the service
	 * received, to favor sequential workloads.
	 *
	 * Processes doing I/O in the slower disk zones will tend to
	 * be slow(er) even if not seeky. Therefore, since the
	 * estimated peak rate is actually an average over the disk
	 * surface, these processes may timeout just for bad luck. To
	 * avoid punishing them, do not charge time to processes that
	 * succeeded in consuming at least 2/3 of their budget. This
	 * allows BFQ to preserve enough elasticity to still perform
	 * bandwidth, and not time, distribution with little unlucky
	 * or quasi-sequential processes.
	 */
	if (bfqq->wr_coeff == 1 &&
	    (slow ||
	     (reason == BFQ_BFQQ_BUDGET_TIMEOUT &&
	      bfq_bfqq_budget_left(bfqq) >=  entity->budget / 3)))
		bfq_bfqq_charge_time(bfqd, bfqq, delta);

	BUG_ON(bfqq->entity.budget < bfqq->entity.service);

	if (reason == BFQ_BFQQ_TOO_IDLE &&
	    entity->service <= 2 * entity->budget / 10 )
		bfq_clear_bfqq_IO_bound(bfqq);

	if (bfqd->low_latency && bfqq->wr_coeff == 1)
		bfqq->last_wr_start_finish = jiffies;

	if (bfqd->low_latency && bfqd->bfq_wr_max_softrt_rate > 0 &&
	    RB_EMPTY_ROOT(&bfqq->sort_list)) {
		/*
		 * If we get here, and there are no outstanding
		 * requests, then the request pattern is isochronous
		 * (see the comments on the function
		 * bfq_bfqq_softrt_next_start()). Thus we can compute
		 * soft_rt_next_start. If, instead, the queue still
		 * has outstanding requests, then we have to wait for
		 * the completion of all the outstanding requests to
		 * discover whether the request pattern is actually
		 * isochronous.
		 */
		BUG_ON(bfqd->busy_queues < 1);
		if (bfqq->dispatched == 0) {
			bfqq->soft_rt_next_start =
				bfq_bfqq_softrt_next_start(bfqd, bfqq);
			bfq_log_bfqq(bfqd, bfqq, "new soft_rt_next %lu",
				     bfqq->soft_rt_next_start);
		} else {
			/*
			 * The application is still waiting for the
			 * completion of one or more requests:
			 * prevent it from possibly being incorrectly
			 * deemed as soft real-time by setting its
			 * soft_rt_next_start to infinity. In fact,
			 * without this assignment, the application
			 * would be incorrectly deemed as soft
			 * real-time if:
			 * 1) it issued a new request before the
			 *    completion of all its in-flight
			 *    requests, and
			 * 2) at that time, its soft_rt_next_start
			 *    happened to be in the past.
			 */
			bfqq->soft_rt_next_start =
				bfq_greatest_from_now();
			/*
			 * Schedule an update of soft_rt_next_start to when
			 * the task may be discovered to be isochronous.
			 */
			bfq_mark_bfqq_softrt_update(bfqq);
		}
	}

	bfq_log_bfqq(bfqd, bfqq,
		"expire (%d, slow %d, num_disp %d, idle_win %d, weight %d)",
		     reason, slow, bfqq->dispatched,
		     bfq_bfqq_idle_window(bfqq), entity->weight);

	/*
	 * Increase, decrease or leave budget unchanged according to
	 * reason.
	 */
	BUG_ON(bfqq->entity.budget < bfqq->entity.service);
	__bfq_bfqq_recalc_budget(bfqd, bfqq, reason);
	BUG_ON(bfqq->next_rq == NULL &&
	       bfqq->entity.budget < bfqq->entity.service);
	__bfq_bfqq_expire(bfqd, bfqq);

	BUG_ON(!bfq_bfqq_busy(bfqq) && reason == BFQ_BFQQ_BUDGET_EXHAUSTED &&
		!bfq_class_idle(bfqq));

	if (!bfq_bfqq_busy(bfqq) &&
	    reason != BFQ_BFQQ_BUDGET_TIMEOUT &&
	    reason != BFQ_BFQQ_BUDGET_EXHAUSTED)
		bfq_mark_bfqq_non_blocking_wait_rq(bfqq);
}

/*
 * Budget timeout is not implemented through a dedicated timer, but
 * just checked on request arrivals and completions, as well as on
 * idle timer expirations.
 */
static bool bfq_bfqq_budget_timeout(struct bfq_queue *bfqq)
{
	return time_is_before_eq_jiffies(bfqq->budget_timeout);
}

/*
 * If we expire a queue that is actively waiting (i.e., with the
 * device idled) for the arrival of a new request, then we may incur
 * the timestamp misalignment problem described in the body of the
 * function __bfq_activate_entity. Hence we return true only if this
 * condition does not hold, or if the queue is slow enough to deserve
 * only to be kicked off for preserving a high throughput.
 */
static bool bfq_may_expire_for_budg_timeout(struct bfq_queue *bfqq)
{
	bfq_log_bfqq(bfqq->bfqd, bfqq,
		"may_budget_timeout: wait_request %d left %d timeout %d",
		bfq_bfqq_wait_request(bfqq),
			bfq_bfqq_budget_left(bfqq) >=  bfqq->entity.budget / 3,
		bfq_bfqq_budget_timeout(bfqq));

	return (!bfq_bfqq_wait_request(bfqq) ||
		bfq_bfqq_budget_left(bfqq) >=  bfqq->entity.budget / 3)
		&&
		bfq_bfqq_budget_timeout(bfqq);
}

/*
 * For a queue that becomes empty, device idling is allowed only if
 * this function returns true for that queue. As a consequence, since
 * device idling plays a critical role for both throughput boosting
 * and service guarantees, the return value of this function plays a
 * critical role as well.
 *
 * In a nutshell, this function returns true only if idling is
 * beneficial for throughput or, even if detrimental for throughput,
 * idling is however necessary to preserve service guarantees (low
 * latency, desired throughput distribution, ...). In particular, on
 * NCQ-capable devices, this function tries to return false, so as to
 * help keep the drives' internal queues full, whenever this helps the
 * device boost the throughput without causing any service-guarantee
 * issue.
 *
 * In more detail, the return value of this function is obtained by,
 * first, computing a number of boolean variables that take into
 * account throughput and service-guarantee issues, and, then,
 * combining these variables in a logical expression. Most of the
 * issues taken into account are not trivial. We discuss these issues
 * while introducing the variables.
 */
static bool bfq_bfqq_may_idle(struct bfq_queue *bfqq)
{
	struct bfq_data *bfqd = bfqq->bfqd;
	bool idling_boosts_thr, idling_boosts_thr_without_issues,
		idling_needed_for_service_guarantees,
		asymmetric_scenario;

	if (bfqd->strict_guarantees)
		return true;

	/*
	 * The next variable takes into account the cases where idling
	 * boosts the throughput.
	 *
	 * The value of the variable is computed considering, first, that
	 * idling is virtually always beneficial for the throughput if:
	 * (a) the device is not NCQ-capable, or
	 * (b) regardless of the presence of NCQ, the device is rotational
	 *     and the request pattern for bfqq is I/O-bound and sequential.
	 *
	 * Secondly, and in contrast to the above item (b), idling an
	 * NCQ-capable flash-based device would not boost the
	 * throughput even with sequential I/O; rather it would lower
	 * the throughput in proportion to how fast the device
	 * is. Accordingly, the next variable is true if any of the
	 * above conditions (a) and (b) is true, and, in particular,
	 * happens to be false if bfqd is an NCQ-capable flash-based
	 * device.
	 */
	idling_boosts_thr = !bfqd->hw_tag ||
		(!blk_queue_nonrot(bfqd->queue) && bfq_bfqq_IO_bound(bfqq) &&
		 bfq_bfqq_idle_window(bfqq)) ;

	/*
	 * The value of the next variable,
	 * idling_boosts_thr_without_issues, is equal to that of
	 * idling_boosts_thr, unless a special case holds. In this
	 * special case, described below, idling may cause problems to
	 * weight-raised queues.
	 *
	 * When the request pool is saturated (e.g., in the presence
	 * of write hogs), if the processes associated with
	 * non-weight-raised queues ask for requests at a lower rate,
	 * then processes associated with weight-raised queues have a
	 * higher probability to get a request from the pool
	 * immediately (or at least soon) when they need one. Thus
	 * they have a higher probability to actually get a fraction
	 * of the device throughput proportional to their high
	 * weight. This is especially true with NCQ-capable drives,
	 * which enqueue several requests in advance, and further
	 * reorder internally-queued requests.
	 *
	 * For this reason, we force to false the value of
	 * idling_boosts_thr_without_issues if there are weight-raised
	 * busy queues. In this case, and if bfqq is not weight-raised,
	 * this guarantees that the device is not idled for bfqq (if,
	 * instead, bfqq is weight-raised, then idling will be
	 * guaranteed by another variable, see below). Combined with
	 * the timestamping rules of BFQ (see [1] for details), this
	 * behavior causes bfqq, and hence any sync non-weight-raised
	 * queue, to get a lower number of requests served, and thus
	 * to ask for a lower number of requests from the request
	 * pool, before the busy weight-raised queues get served
	 * again. This often mitigates starvation problems in the
	 * presence of heavy write workloads and NCQ, thereby
	 * guaranteeing a higher application and system responsiveness
	 * in these hostile scenarios.
	 */
	idling_boosts_thr_without_issues = idling_boosts_thr &&
		bfqd->wr_busy_queues == 0;

	/*
	 * There is then a case where idling must be performed not
	 * for throughput concerns, but to preserve service
	 * guarantees.
	 *
	 * To introduce this case, we can note that allowing the drive
	 * to enqueue more than one request at a time, and hence
	 * delegating de facto final scheduling decisions to the
	 * drive's internal scheduler, entails loss of control on the
	 * actual request service order. In particular, the critical
	 * situation is when requests from different processes happen
	 * to be present, at the same time, in the internal queue(s)
	 * of the drive. In such a situation, the drive, by deciding
	 * the service order of the internally-queued requests, does
	 * determine also the actual throughput distribution among
	 * these processes. But the drive typically has no notion or
	 * concern about per-process throughput distribution, and
	 * makes its decisions only on a per-request basis. Therefore,
	 * the service distribution enforced by the drive's internal
	 * scheduler is likely to coincide with the desired
	 * device-throughput distribution only in a completely
	 * symmetric scenario where:
	 * (i)  each of these processes must get the same throughput as
	 *      the others;
	 * (ii) all these processes have the same I/O pattern
	        (either sequential or random).
	 * In fact, in such a scenario, the drive will tend to treat
	 * the requests of each of these processes in about the same
	 * way as the requests of the others, and thus to provide
	 * each of these processes with about the same throughput
	 * (which is exactly the desired throughput distribution). In
	 * contrast, in any asymmetric scenario, device idling is
	 * certainly needed to guarantee that bfqq receives its
	 * assigned fraction of the device throughput (see [1] for
	 * details).
	 *
	 * We address this issue by controlling, actually, only the
	 * symmetry sub-condition (i), i.e., provided that
	 * sub-condition (i) holds, idling is not performed,
	 * regardless of whether sub-condition (ii) holds. In other
	 * words, only if sub-condition (i) holds, then idling is
	 * allowed, and the device tends to be prevented from queueing
	 * many requests, possibly of several processes. The reason
	 * for not controlling also sub-condition (ii) is that we
	 * exploit preemption to preserve guarantees in case of
	 * symmetric scenarios, even if (ii) does not hold, as
	 * explained in the next two paragraphs.
	 *
	 * Even if a queue, say Q, is expired when it remains idle, Q
	 * can still preempt the new in-service queue if the next
	 * request of Q arrives soon (see the comments on
	 * bfq_bfqq_update_budg_for_activation). If all queues and
	 * groups have the same weight, this form of preemption,
	 * combined with the hole-recovery heuristic described in the
	 * comments on function bfq_bfqq_update_budg_for_activation,
	 * are enough to preserve a correct bandwidth distribution in
	 * the mid term, even without idling. In fact, even if not
	 * idling allows the internal queues of the device to contain
	 * many requests, and thus to reorder requests, we can rather
	 * safely assume that the internal scheduler still preserves a
	 * minimum of mid-term fairness. The motivation for using
	 * preemption instead of idling is that, by not idling,
	 * service guarantees are preserved without minimally
	 * sacrificing throughput. In other words, both a high
	 * throughput and its desired distribution are obtained.
	 *
	 * More precisely, this preemption-based, idleless approach
	 * provides fairness in terms of IOPS, and not sectors per
	 * second. This can be seen with a simple example. Suppose
	 * that there are two queues with the same weight, but that
	 * the first queue receives requests of 8 sectors, while the
	 * second queue receives requests of 1024 sectors. In
	 * addition, suppose that each of the two queues contains at
	 * most one request at a time, which implies that each queue
	 * always remains idle after it is served. Finally, after
	 * remaining idle, each queue receives very quickly a new
	 * request. It follows that the two queues are served
	 * alternatively, preempting each other if needed. This
	 * implies that, although both queues have the same weight,
	 * the queue with large requests receives a service that is
	 * 1024/8 times as high as the service received by the other
	 * queue.
	 *
	 * On the other hand, device idling is performed, and thus
	 * pure sector-domain guarantees are provided, for the
	 * following queues, which are likely to need stronger
	 * throughput guarantees: weight-raised queues, and queues
	 * with a higher weight than other queues. When such queues
	 * are active, sub-condition (i) is false, which triggers
	 * device idling.
	 *
	 * According to the above considerations, the next variable is
	 * true (only) if sub-condition (i) holds. To compute the
	 * value of this variable, we not only use the return value of
	 * the function bfq_symmetric_scenario(), but also check
	 * whether bfqq is being weight-raised, because
	 * bfq_symmetric_scenario() does not take into account also
	 * weight-raised queues (see comments on
	 * bfq_weights_tree_add()).
	 *
	 * As a side note, it is worth considering that the above
	 * device-idling countermeasures may however fail in the
	 * following unlucky scenario: if idling is (correctly)
	 * disabled in a time period during which all symmetry
	 * sub-conditions hold, and hence the device is allowed to
	 * enqueue many requests, but at some later point in time some
	 * sub-condition stops to hold, then it may become impossible
	 * to let requests be served in the desired order until all
	 * the requests already queued in the device have been served.
	 */
	asymmetric_scenario = bfqq->wr_coeff > 1 ||
		!bfq_symmetric_scenario(bfqd);

	/*
	 * Finally, there is a case where maximizing throughput is the
	 * best choice even if it may cause unfairness toward
	 * bfqq. Such a case is when bfqq became active in a burst of
	 * queue activations. Queues that became active during a large
	 * burst benefit only from throughput, as discussed in the
	 * comments on bfq_handle_burst. Thus, if bfqq became active
	 * in a burst and not idling the device maximizes throughput,
	 * then the device must no be idled, because not idling the
	 * device provides bfqq and all other queues in the burst with
	 * maximum benefit. Combining this and the above case, we can
	 * now establish when idling is actually needed to preserve
	 * service guarantees.
	 */
	idling_needed_for_service_guarantees =
		asymmetric_scenario && !bfq_bfqq_in_large_burst(bfqq);

	/*
	 * We have now all the components we need to compute the return
	 * value of the function, which is true only if both the following
	 * conditions hold:
	 * 1) bfqq is sync, because idling make sense only for sync queues;
	 * 2) idling either boosts the throughput (without issues), or
	 *    is necessary to preserve service guarantees.
	 */
	bfq_log_bfqq(bfqd, bfqq, "may_idle: sync %d idling_boosts_thr %d",
		     bfq_bfqq_sync(bfqq), idling_boosts_thr);

	bfq_log_bfqq(bfqd, bfqq,
		     "may_idle: wr_busy %d boosts %d IO-bound %d guar %d",
		     bfqd->wr_busy_queues,
		     idling_boosts_thr_without_issues,
		     bfq_bfqq_IO_bound(bfqq),
		     idling_needed_for_service_guarantees);

	return bfq_bfqq_sync(bfqq) &&
		(idling_boosts_thr_without_issues ||
		 idling_needed_for_service_guarantees);
}

/*
 * If the in-service queue is empty but the function bfq_bfqq_may_idle
 * returns true, then:
 * 1) the queue must remain in service and cannot be expired, and
 * 2) the device must be idled to wait for the possible arrival of a new
 *    request for the queue.
 * See the comments on the function bfq_bfqq_may_idle for the reasons
 * why performing device idling is the best choice to boost the throughput
 * and preserve service guarantees when bfq_bfqq_may_idle itself
 * returns true.
 */
static bool bfq_bfqq_must_idle(struct bfq_queue *bfqq)
{
	struct bfq_data *bfqd = bfqq->bfqd;

	return RB_EMPTY_ROOT(&bfqq->sort_list) && bfqd->bfq_slice_idle != 0 &&
	       bfq_bfqq_may_idle(bfqq);
}

/*
 * Select a queue for service.  If we have a current queue in service,
 * check whether to continue servicing it, or retrieve and set a new one.
 */
static struct bfq_queue *bfq_select_queue(struct bfq_data *bfqd)
{
	struct bfq_queue *bfqq;
	struct request *next_rq;
	enum bfqq_expiration reason = BFQ_BFQQ_BUDGET_TIMEOUT;

	bfqq = bfqd->in_service_queue;
	if (!bfqq)
		goto new_queue;

	bfq_log_bfqq(bfqd, bfqq, "select_queue: already in-service queue");

	if (bfq_may_expire_for_budg_timeout(bfqq) &&
	    !hrtimer_active(&bfqd->idle_slice_timer) &&
	    !bfq_bfqq_must_idle(bfqq))
		goto expire;

	next_rq = bfqq->next_rq;
	/*
	 * If bfqq has requests queued and it has enough budget left to
	 * serve them, keep the queue, otherwise expire it.
	 */
	if (next_rq) {
		if (bfq_serv_to_charge(next_rq, bfqq) >
			bfq_bfqq_budget_left(bfqq)) {
			reason = BFQ_BFQQ_BUDGET_EXHAUSTED;
			goto expire;
		} else {
			/*
			 * The idle timer may be pending because we may
			 * not disable disk idling even when a new request
			 * arrives.
			 */
			if (bfq_bfqq_wait_request(bfqq)) {
				BUG_ON(!hrtimer_active(&bfqd->idle_slice_timer));
				/*
				 * If we get here: 1) at least a new request
				 * has arrived but we have not disabled the
				 * timer because the request was too small,
				 * 2) then the block layer has unplugged
				 * the device, causing the dispatch to be
				 * invoked.
				 *
				 * Since the device is unplugged, now the
				 * requests are probably large enough to
				 * provide a reasonable throughput.
				 * So we disable idling.
				 */
				bfq_clear_bfqq_wait_request(bfqq);
				hrtimer_try_to_cancel(&bfqd->idle_slice_timer);
				bfqg_stats_update_idle_time(bfqq_group(bfqq));
			}
			goto keep_queue;
		}
	}

	/*
	 * No requests pending. However, if the in-service queue is idling
	 * for a new request, or has requests waiting for a completion and
	 * may idle after their completion, then keep it anyway.
	 */
	if (hrtimer_active(&bfqd->idle_slice_timer) ||
	    (bfqq->dispatched != 0 && bfq_bfqq_may_idle(bfqq))) {
		bfqq = NULL;
		goto keep_queue;
	}

	reason = BFQ_BFQQ_NO_MORE_REQUESTS;
expire:
	bfq_bfqq_expire(bfqd, bfqq, false, reason);
new_queue:
	bfqq = bfq_set_in_service_queue(bfqd);
	bfq_log(bfqd, "select_queue: new queue %d returned",
		bfqq ? bfqq->pid : 0);
keep_queue:
	return bfqq;
}

static void bfq_update_wr_data(struct bfq_data *bfqd, struct bfq_queue *bfqq)
{
	struct bfq_entity *entity = &bfqq->entity;
	if (bfqq->wr_coeff > 1) { /* queue is being weight-raised */
		bfq_log_bfqq(bfqd, bfqq,
			"raising period dur %u/%u msec, old coeff %u, w %d(%d)",
			jiffies_to_msecs(jiffies - bfqq->last_wr_start_finish),
			jiffies_to_msecs(bfqq->wr_cur_max_time),
			bfqq->wr_coeff,
			bfqq->entity.weight, bfqq->entity.orig_weight);

		BUG_ON(bfqq != bfqd->in_service_queue && entity->weight !=
		       entity->orig_weight * bfqq->wr_coeff);
		if (entity->prio_changed)
			bfq_log_bfqq(bfqd, bfqq, "WARN: pending prio change");

		/*
		 * If the queue was activated in a burst, or too much
		 * time has elapsed from the beginning of this
		 * weight-raising period, then end weight raising.
		 */
		if (bfq_bfqq_in_large_burst(bfqq) ||
		    time_is_before_jiffies(bfqq->last_wr_start_finish +
					   bfqq->wr_cur_max_time)) {
			bfqq->last_wr_start_finish = jiffies;
			bfq_log_bfqq(bfqd, bfqq,
				     "wrais ending at %lu, rais_max_time %u",
				     bfqq->last_wr_start_finish,
				     jiffies_to_msecs(bfqq->wr_cur_max_time));
			bfq_bfqq_end_wr(bfqq);
		}
	}
	/* Update weight both if it must be raised and if it must be lowered */
	if ((entity->weight > entity->orig_weight) != (bfqq->wr_coeff > 1))
		__bfq_entity_update_weight_prio(
			bfq_entity_service_tree(entity),
			entity);
}

/*
 * Dispatch one request from bfqq, moving it to the request queue
 * dispatch list.
 */
static int bfq_dispatch_request(struct bfq_data *bfqd,
				struct bfq_queue *bfqq)
{
	int dispatched = 0;
	struct request *rq;
	unsigned long service_to_charge;

	BUG_ON(RB_EMPTY_ROOT(&bfqq->sort_list));

	/* Follow expired path, else get first next available. */
	rq = bfq_check_fifo(bfqq);
	if (!rq)
		rq = bfqq->next_rq;
	service_to_charge = bfq_serv_to_charge(rq, bfqq);

	if (service_to_charge > bfq_bfqq_budget_left(bfqq)) {
		/*
		 * This may happen if the next rq is chosen in fifo order
		 * instead of sector order. The budget is properly
		 * dimensioned to be always sufficient to serve the next
		 * request only if it is chosen in sector order. The reason
		 * is that it would be quite inefficient and little useful
		 * to always make sure that the budget is large enough to
		 * serve even the possible next rq in fifo order.
		 * In fact, requests are seldom served in fifo order.
		 *
		 * Expire the queue for budget exhaustion, and make sure
		 * that the next act_budget is enough to serve the next
		 * request, even if it comes from the fifo expired path.
		 */
		bfqq->next_rq = rq;
		/*
		 * Since this dispatch is failed, make sure that
		 * a new one will be performed
		 */
		if (!bfqd->rq_in_driver)
			bfq_schedule_dispatch(bfqd);
		BUG_ON(bfqq->entity.budget < bfqq->entity.service);
		goto expire;
	}

	BUG_ON(bfqq->entity.budget < bfqq->entity.service);
	/* Finally, insert request into driver dispatch list. */
	bfq_bfqq_served(bfqq, service_to_charge);

	BUG_ON(bfqq->entity.budget < bfqq->entity.service);

	bfq_dispatch_insert(bfqd->queue, rq);

	/*
	 * If weight raising has to terminate for bfqq, then next
	 * function causes an immediate update of bfqq's weight,
	 * without waiting for next activation. As a consequence, on
	 * expiration, bfqq will be timestamped as if has never been
	 * weight-raised during this service slot, even if it has
	 * received part or even most of the service as a
	 * weight-raised queue. This inflates bfqq's timestamps, which
	 * is beneficial, as bfqq is then more willing to leave the
	 * device immediately to possible other weight-raised queues.
	 */
	bfq_update_wr_data(bfqd, bfqq);

	bfq_log_bfqq(bfqd, bfqq,
			"dispatched %u sec req (%llu), budg left %d",
			blk_rq_sectors(rq),
			(long long unsigned)blk_rq_pos(rq),
			bfq_bfqq_budget_left(bfqq));

	dispatched++;

	if (!bfqd->in_service_bic) {
		atomic_long_inc(&RQ_BIC(rq)->icq.ioc->refcount);
		bfqd->in_service_bic = RQ_BIC(rq);
	}

	if (bfqd->busy_queues > 1 && bfq_class_idle(bfqq))
		goto expire;

	return dispatched;

expire:
	bfq_bfqq_expire(bfqd, bfqq, false, BFQ_BFQQ_BUDGET_EXHAUSTED);
	return dispatched;
}

static int __bfq_forced_dispatch_bfqq(struct bfq_queue *bfqq)
{
	int dispatched = 0;

	while (bfqq->next_rq) {
		bfq_dispatch_insert(bfqq->bfqd->queue, bfqq->next_rq);
		dispatched++;
	}

	BUG_ON(!list_empty(&bfqq->fifo));
	return dispatched;
}

/*
 * Drain our current requests.
 * Used for barriers and when switching io schedulers on-the-fly.
 */
static int bfq_forced_dispatch(struct bfq_data *bfqd)
{
	struct bfq_queue *bfqq, *n;
	struct bfq_service_tree *st;
	int dispatched = 0;

	bfqq = bfqd->in_service_queue;
	if (bfqq)
		__bfq_bfqq_expire(bfqd, bfqq);

	/*
	 * Loop through classes, and be careful to leave the scheduler
	 * in a consistent state, as feedback mechanisms and vtime
	 * updates cannot be disabled during the process.
	 */
	list_for_each_entry_safe(bfqq, n, &bfqd->active_list, bfqq_list) {
		st = bfq_entity_service_tree(&bfqq->entity);

		dispatched += __bfq_forced_dispatch_bfqq(bfqq);

		bfqq->max_budget = bfq_max_budget(bfqd);
		bfq_forget_idle(st);
	}

	BUG_ON(bfqd->busy_queues != 0);

	return dispatched;
}

static int bfq_dispatch_requests(struct request_queue *q, int force)
{
	struct bfq_data *bfqd = q->elevator->elevator_data;
	struct bfq_queue *bfqq;

	bfq_log(bfqd, "dispatch requests: %d busy queues", bfqd->busy_queues);

	if (bfqd->busy_queues == 0)
		return 0;

	if (unlikely(force))
		return bfq_forced_dispatch(bfqd);

	/*
	 * Force device to serve one request at a time if
	 * strict_guarantees is true. Forcing this service scheme is
	 * currently the ONLY way to guarantee that the request
	 * service order enforced by the scheduler is respected by a
	 * queueing device. Otherwise the device is free even to make
	 * some unlucky request wait for as long as the device
	 * wishes.
	 *
	 * Of course, serving one request at at time may cause loss of
	 * throughput.
	 */
	if (bfqd->strict_guarantees && bfqd->rq_in_driver > 0)
		return 0;

	bfqq = bfq_select_queue(bfqd);
	if (!bfqq)
		return 0;

	BUG_ON(bfqq->entity.budget < bfqq->entity.service);

	BUG_ON(bfq_bfqq_wait_request(bfqq));

	if (!bfq_dispatch_request(bfqd, bfqq))
		return 0;

	bfq_log_bfqq(bfqd, bfqq, "dispatched %s request",
			bfq_bfqq_sync(bfqq) ? "sync" : "async");

	BUG_ON(bfqq->next_rq == NULL &&
	       bfqq->entity.budget < bfqq->entity.service);
	return 1;
}

/*
 * Task holds one reference to the queue, dropped when task exits.  Each rq
 * in-flight on this queue also holds a reference, dropped when rq is freed.
 *
 * Queue lock must be held here.
 */
static void bfq_put_queue(struct bfq_queue *bfqq)
{
#ifdef CONFIG_BFQ_GROUP_IOSCHED
	struct bfq_group *bfqg = bfqq_group(bfqq);
#endif

	BUG_ON(bfqq->ref <= 0);

	bfq_log_bfqq(bfqq->bfqd, bfqq, "put_queue: %p %d", bfqq, bfqq->ref);
	bfqq->ref--;
	if (bfqq->ref)
		return;

	BUG_ON(rb_first(&bfqq->sort_list));
	BUG_ON(bfqq->allocated[READ] + bfqq->allocated[WRITE] != 0);
	BUG_ON(bfqq->entity.tree);
	BUG_ON(bfq_bfqq_busy(bfqq));
	BUG_ON(bfqq->bfqd->in_service_queue == bfqq);

	if (bfq_bfqq_sync(bfqq))
		/*
		 * The fact that this queue is being destroyed does not
		 * invalidate the fact that this queue may have been
		 * activated during the current burst. As a consequence,
		 * although the queue does not exist anymore, and hence
		 * needs to be removed from the burst list if there,
		 * the burst size has not to be decremented.
		 */
		hlist_del_init(&bfqq->burst_list_node);

	bfq_log_bfqq(bfqq->bfqd, bfqq, "put_queue: %p freed", bfqq);

	kmem_cache_free(bfq_pool, bfqq);
#ifdef CONFIG_BFQ_GROUP_IOSCHED
	bfqg_put(bfqg);
#endif
}

static void bfq_put_cooperator(struct bfq_queue *bfqq)
{
	struct bfq_queue *__bfqq, *next;

	/*
	 * If this queue was scheduled to merge with another queue, be
	 * sure to drop the reference taken on that queue (and others in
	 * the merge chain). See bfq_setup_merge and bfq_merge_bfqqs.
	 */
	__bfqq = bfqq->new_bfqq;
	while (__bfqq) {
		if (__bfqq == bfqq)
			break;
		next = __bfqq->new_bfqq;
		bfq_put_queue(__bfqq);
		__bfqq = next;
	}
}

static void bfq_exit_bfqq(struct bfq_data *bfqd, struct bfq_queue *bfqq)
{
	if (bfqq == bfqd->in_service_queue) {
		__bfq_bfqq_expire(bfqd, bfqq);
		bfq_schedule_dispatch(bfqd);
	}

	bfq_log_bfqq(bfqd, bfqq, "exit_bfqq: %p, %d", bfqq, bfqq->ref);

	bfq_put_cooperator(bfqq);

	bfq_put_queue(bfqq);
}

static void bfq_init_icq(struct io_cq *icq)
{
	icq_to_bic(icq)->ttime.last_end_request = ktime_get_ns() - (1ULL<<32);
}

static void bfq_exit_icq(struct io_cq *icq)
{
	struct bfq_io_cq *bic = icq_to_bic(icq);
	struct bfq_data *bfqd = bic_to_bfqd(bic);

	if (bic_to_bfqq(bic, false)) {
		bfq_exit_bfqq(bfqd, bic_to_bfqq(bic, false));
		bic_set_bfqq(bic, NULL, false);
	}

	if (bic_to_bfqq(bic, true)) {
		/*
		 * If the bic is using a shared queue, put the reference
		 * taken on the io_context when the bic started using a
		 * shared bfq_queue.
		 */
		if (bfq_bfqq_coop(bic_to_bfqq(bic, true)))
			put_io_context(icq->ioc);
		bfq_exit_bfqq(bfqd, bic_to_bfqq(bic, true));
		bic_set_bfqq(bic, NULL, true);
	}
}

/*
 * Update the entity prio values; note that the new values will not
 * be used until the next (re)activation.
 */
static void bfq_set_next_ioprio_data(struct bfq_queue *bfqq,
				     struct bfq_io_cq *bic)
{
	struct task_struct *tsk = current;
	int ioprio_class;

	ioprio_class = IOPRIO_PRIO_CLASS(bic->ioprio);
	switch (ioprio_class) {
	default:
		dev_err(bfqq->bfqd->queue->backing_dev_info.dev,
			"bfq: bad prio class %d\n", ioprio_class);
	case IOPRIO_CLASS_NONE:
		/*
		 * No prio set, inherit CPU scheduling settings.
		 */
		bfqq->new_ioprio = task_nice_ioprio(tsk);
		bfqq->new_ioprio_class = task_nice_ioclass(tsk);
		break;
	case IOPRIO_CLASS_RT:
		bfqq->new_ioprio = IOPRIO_PRIO_DATA(bic->ioprio);
		bfqq->new_ioprio_class = IOPRIO_CLASS_RT;
		break;
	case IOPRIO_CLASS_BE:
		bfqq->new_ioprio = IOPRIO_PRIO_DATA(bic->ioprio);
		bfqq->new_ioprio_class = IOPRIO_CLASS_BE;
		break;
	case IOPRIO_CLASS_IDLE:
		bfqq->new_ioprio_class = IOPRIO_CLASS_IDLE;
		bfqq->new_ioprio = 7;
		bfq_clear_bfqq_idle_window(bfqq);
		break;
	}

	if (bfqq->new_ioprio >= IOPRIO_BE_NR) {
		pr_crit("bfq_set_next_ioprio_data: new_ioprio %d\n",
			bfqq->new_ioprio);
		BUG();
	}

	bfqq->entity.new_weight = bfq_ioprio_to_weight(bfqq->new_ioprio);
	bfqq->entity.prio_changed = 1;
	bfq_log_bfqq(bfqq->bfqd, bfqq,
		     "set_next_ioprio_data: bic_class %d prio %d class %d",
		     ioprio_class, bfqq->new_ioprio, bfqq->new_ioprio_class);
}

static void bfq_check_ioprio_change(struct bfq_io_cq *bic, struct bio *bio)
{
	struct bfq_data *bfqd = bic_to_bfqd(bic);
	struct bfq_queue *bfqq;
	unsigned long uninitialized_var(flags);
	int ioprio = bic->icq.ioc->ioprio;

	/*
	 * This condition may trigger on a newly created bic, be sure to
	 * drop the lock before returning.
	 */
	if (unlikely(!bfqd) || likely(bic->ioprio == ioprio))
		return;

	bic->ioprio = ioprio;

	bfqq = bic_to_bfqq(bic, false);
	if (bfqq) {
		bfq_put_queue(bfqq);
		bfqq = bfq_get_queue(bfqd, bio, BLK_RW_ASYNC, bic);
		bic_set_bfqq(bic, bfqq, false);
		bfq_log_bfqq(bfqd, bfqq,
			     "check_ioprio_change: bfqq %p %d",
			     bfqq, bfqq->ref);
	}

	bfqq = bic_to_bfqq(bic, true);
	if (bfqq)
		bfq_set_next_ioprio_data(bfqq, bic);
}

static void bfq_init_bfqq(struct bfq_data *bfqd, struct bfq_queue *bfqq,
			  struct bfq_io_cq *bic, pid_t pid, int is_sync)
{
	RB_CLEAR_NODE(&bfqq->entity.rb_node);
	INIT_LIST_HEAD(&bfqq->fifo);
	INIT_HLIST_NODE(&bfqq->burst_list_node);
	BUG_ON(!hlist_unhashed(&bfqq->burst_list_node));

	bfqq->ref = 0;
	bfqq->bfqd = bfqd;

	if (bic)
		bfq_set_next_ioprio_data(bfqq, bic);

	if (is_sync) {
		if (!bfq_class_idle(bfqq))
			bfq_mark_bfqq_idle_window(bfqq);
		bfq_mark_bfqq_sync(bfqq);
		bfq_mark_bfqq_just_created(bfqq);
	} else
		bfq_clear_bfqq_sync(bfqq);
	bfq_mark_bfqq_IO_bound(bfqq);

	/* Tentative initial value to trade off between thr and lat */
	bfqq->max_budget = (2 * bfq_max_budget(bfqd)) / 3;
	bfqq->pid = pid;

	bfqq->wr_coeff = 1;
	bfqq->last_wr_start_finish = bfq_smallest_from_now();
	bfqq->budget_timeout = bfq_smallest_from_now();
	bfqq->split_time = bfq_smallest_from_now();
	/*
	 * Set to the value for which bfqq will not be deemed as
	 * soft rt when it becomes backlogged.
	 */
	bfqq->soft_rt_next_start = bfq_greatest_from_now();

	/* first request is almost certainly seeky */
	bfqq->seek_history = 1;
}

static struct bfq_queue **bfq_async_queue_prio(struct bfq_data *bfqd,
					       struct bfq_group *bfqg,
					       int ioprio_class, int ioprio)
{
	switch (ioprio_class) {
	case IOPRIO_CLASS_RT:
		return &bfqg->async_bfqq[0][ioprio];
	case IOPRIO_CLASS_NONE:
		ioprio = IOPRIO_NORM;
		/* fall through */
	case IOPRIO_CLASS_BE:
		return &bfqg->async_bfqq[1][ioprio];
	case IOPRIO_CLASS_IDLE:
		return &bfqg->async_idle_bfqq;
	default:
		BUG();
	}
}

static struct bfq_queue *bfq_get_queue(struct bfq_data *bfqd,
				       struct bio *bio, bool is_sync,
				       struct bfq_io_cq *bic)
{
	const int ioprio = IOPRIO_PRIO_DATA(bic->ioprio);
	const int ioprio_class = IOPRIO_PRIO_CLASS(bic->ioprio);
	struct bfq_queue **async_bfqq = NULL;
	struct bfq_queue *bfqq;
	struct bfq_group *bfqg;

	rcu_read_lock();

	bfqg = bfq_find_set_group(bfqd, bio_blkcg(bio));
	if (!bfqg) {
		bfqq = &bfqd->oom_bfqq;
		goto out;
	}

	if (!is_sync) {
		async_bfqq = bfq_async_queue_prio(bfqd, bfqg, ioprio_class,
						  ioprio);
		bfqq = *async_bfqq;
		if (bfqq)
			goto out;
	}

	bfqq = kmem_cache_alloc_node(bfq_pool, GFP_NOWAIT | __GFP_ZERO,
				     bfqd->queue->node);

	if (bfqq) {
		bfq_init_bfqq(bfqd, bfqq, bic, current->pid,
			      is_sync);
		bfq_init_entity(&bfqq->entity, bfqg);
		bfq_log_bfqq(bfqd, bfqq, "allocated");
	} else {
		bfqq = &bfqd->oom_bfqq;
		bfq_log_bfqq(bfqd, bfqq, "using oom bfqq");
		goto out;
	}

	/*
	 * Pin the queue now that it's allocated, scheduler exit will
	 * prune it.
	 */
	if (async_bfqq) {
		bfqq->ref++;
		bfq_log_bfqq(bfqd, bfqq, "get_queue, bfqq not in async: %p, %d",
			     bfqq, bfqq->ref);
		*async_bfqq = bfqq;
	}

out:
	bfqq->ref++;
	bfq_log_bfqq(bfqd, bfqq, "get_queue, at end: %p, %d", bfqq, bfqq->ref);
	rcu_read_unlock();
	return bfqq;
}

static void bfq_update_io_thinktime(struct bfq_data *bfqd,
				    struct bfq_io_cq *bic)
{
	struct bfq_ttime *ttime = &bic->ttime;
	u64 elapsed = ktime_get_ns() - bic->ttime.last_end_request;

	elapsed = min_t(u64, elapsed, 2 * bfqd->bfq_slice_idle);

	ttime->ttime_samples = (7*bic->ttime.ttime_samples + 256) / 8;
	ttime->ttime_total = div_u64(7*ttime->ttime_total + 256*elapsed,  8);
	ttime->ttime_mean = div64_ul(ttime->ttime_total + 128,
				     ttime->ttime_samples);
}

static void
bfq_update_io_seektime(struct bfq_data *bfqd, struct bfq_queue *bfqq,
		       struct request *rq)
{
	bfqq->seek_history <<= 1;
	bfqq->seek_history |=
		get_sdist(bfqq->last_request_pos, rq) > BFQQ_SEEK_THR;
}

/*
 * Disable idle window if the process thinks too long or seeks so much that
 * it doesn't matter.
 */
static void bfq_update_idle_window(struct bfq_data *bfqd,
				   struct bfq_queue *bfqq,
				   struct bfq_io_cq *bic)
{
	int enable_idle;

	/* Don't idle for async or idle io prio class. */
	if (!bfq_bfqq_sync(bfqq) || bfq_class_idle(bfqq))
		return;

	/* Idle window just restored, statistics are meaningless. */
	if (time_is_after_eq_jiffies(bfqq->split_time +
				     bfqd->bfq_wr_min_idle_time))
		return;

	enable_idle = bfq_bfqq_idle_window(bfqq);

	if (atomic_read(&bic->icq.ioc->active_ref) == 0 ||
	    bfqd->bfq_slice_idle == 0 ||
		(bfqd->hw_tag && BFQQ_SEEKY(bfqq) &&
			bfqq->wr_coeff == 1))
		enable_idle = 0;
	else if (bfq_sample_valid(bic->ttime.ttime_samples)) {
		if (bic->ttime.ttime_mean > bfqd->bfq_slice_idle &&
			bfqq->wr_coeff == 1)
			enable_idle = 0;
		else
			enable_idle = 1;
	}
	bfq_log_bfqq(bfqd, bfqq, "update_idle_window: enable_idle %d",
		enable_idle);

	if (enable_idle)
		bfq_mark_bfqq_idle_window(bfqq);
	else
		bfq_clear_bfqq_idle_window(bfqq);
}

/*
 * Called when a new fs request (rq) is added to bfqq.  Check if there's
 * something we should do about it.
 */
static void bfq_rq_enqueued(struct bfq_data *bfqd, struct bfq_queue *bfqq,
			    struct request *rq)
{
	struct bfq_io_cq *bic = RQ_BIC(rq);

	if (rq->cmd_flags & REQ_META)
		bfqq->meta_pending++;

	bfq_update_io_thinktime(bfqd, bic);
	bfq_update_io_seektime(bfqd, bfqq, rq);
	if (bfqq->entity.service > bfq_max_budget(bfqd) / 8 ||
	    !BFQQ_SEEKY(bfqq))
		bfq_update_idle_window(bfqd, bfqq, bic);

	bfq_log_bfqq(bfqd, bfqq,
		     "rq_enqueued: idle_window=%d (seeky %d)",
		     bfq_bfqq_idle_window(bfqq), BFQQ_SEEKY(bfqq));

	bfqq->last_request_pos = blk_rq_pos(rq) + blk_rq_sectors(rq);

	if (bfqq == bfqd->in_service_queue && bfq_bfqq_wait_request(bfqq)) {
		bool small_req = bfqq->queued[rq_is_sync(rq)] == 1 &&
				 blk_rq_sectors(rq) < 32;
		bool budget_timeout = bfq_bfqq_budget_timeout(bfqq);

		/*
		 * There is just this request queued: if the request
		 * is small and the queue is not to be expired, then
		 * just exit.
		 *
		 * In this way, if the device is being idled to wait
		 * for a new request from the in-service queue, we
		 * avoid unplugging the device and committing the
		 * device to serve just a small request. On the
		 * contrary, we wait for the block layer to decide
		 * when to unplug the device: hopefully, new requests
		 * will be merged to this one quickly, then the device
		 * will be unplugged and larger requests will be
		 * dispatched.
		 */
		if (small_req && !budget_timeout)
			return;

		/*
		 * A large enough request arrived, or the queue is to
		 * be expired: in both cases disk idling is to be
		 * stopped, so clear wait_request flag and reset
		 * timer.
		 */
		bfq_clear_bfqq_wait_request(bfqq);
		hrtimer_try_to_cancel(&bfqd->idle_slice_timer);
		bfqg_stats_update_idle_time(bfqq_group(bfqq));

		/*
		 * The queue is not empty, because a new request just
		 * arrived. Hence we can safely expire the queue, in
		 * case of budget timeout, without risking that the
		 * timestamps of the queue are not updated correctly.
		 * See [1] for more details.
		 */
		if (budget_timeout)
			bfq_bfqq_expire(bfqd, bfqq, false,
					BFQ_BFQQ_BUDGET_TIMEOUT);

		/*
		 * Let the request rip immediately, or let a new queue be
		 * selected if bfqq has just been expired.
		 */
		__blk_run_queue(bfqd->queue);
	}
}

static void bfq_insert_request(struct request_queue *q, struct request *rq)
{
	struct bfq_data *bfqd = q->elevator->elevator_data;
	struct bfq_queue *bfqq = RQ_BFQQ(rq), *new_bfqq;

	assert_spin_locked(bfqd->queue->queue_lock);

	/*
	 * An unplug may trigger a requeue of a request from the device
	 * driver: make sure we are in process context while trying to
	 * merge two bfq_queues.
	 */
	if (!in_interrupt()) {
		new_bfqq = bfq_setup_cooperator(bfqd, bfqq, rq, true);
		if (new_bfqq) {
			if (bic_to_bfqq(RQ_BIC(rq), 1) != bfqq)
				new_bfqq = bic_to_bfqq(RQ_BIC(rq), 1);
			/*
			 * Release the request's reference to the old bfqq
			 * and make sure one is taken to the shared queue.
			 */
			new_bfqq->allocated[rq_data_dir(rq)]++;
			bfqq->allocated[rq_data_dir(rq)]--;
			new_bfqq->ref++;
			bfq_clear_bfqq_just_created(bfqq);
			bfq_put_queue(bfqq);
			if (bic_to_bfqq(RQ_BIC(rq), 1) == bfqq)
				bfq_merge_bfqqs(bfqd, RQ_BIC(rq),
						bfqq, new_bfqq);
			rq->elv.priv[1] = new_bfqq;
			bfqq = new_bfqq;
		}
	}

	bfq_add_request(rq);

	rq->fifo_time = jiffies + bfqd->bfq_fifo_expire[rq_is_sync(rq)];
	list_add_tail(&rq->queuelist, &bfqq->fifo);

	bfq_rq_enqueued(bfqd, bfqq, rq);
}

static void bfq_update_hw_tag(struct bfq_data *bfqd)
{
	bfqd->max_rq_in_driver = max_t(int, bfqd->max_rq_in_driver,
				       bfqd->rq_in_driver);

	if (bfqd->hw_tag == 1)
		return;

	/*
	 * This sample is valid if the number of outstanding requests
	 * is large enough to allow a queueing behavior.  Note that the
	 * sum is not exact, as it's not taking into account deactivated
	 * requests.
	 */
	if (bfqd->rq_in_driver + bfqd->queued < BFQ_HW_QUEUE_THRESHOLD)
		return;

	if (bfqd->hw_tag_samples++ < BFQ_HW_QUEUE_SAMPLES)
		return;

	bfqd->hw_tag = bfqd->max_rq_in_driver > BFQ_HW_QUEUE_THRESHOLD;
	bfqd->max_rq_in_driver = 0;
	bfqd->hw_tag_samples = 0;
}

static void bfq_completed_request(struct request_queue *q, struct request *rq)
{
	struct bfq_queue *bfqq = RQ_BFQQ(rq);
	struct bfq_data *bfqd = bfqq->bfqd;
	u64 now_ns;
	u32 delta_us;

	bfq_log_bfqq(bfqd, bfqq, "completed one req with %u sects left",
		     blk_rq_sectors(rq));

	assert_spin_locked(bfqd->queue->queue_lock);
	bfq_update_hw_tag(bfqd);

	BUG_ON(!bfqd->rq_in_driver);
	BUG_ON(!bfqq->dispatched);
	bfqd->rq_in_driver--;
	bfqq->dispatched--;
	bfqg_stats_update_completion(bfqq_group(bfqq),
				     rq_start_time_ns(rq),
				     rq_io_start_time_ns(rq), rq->cmd_flags);

	if (!bfqq->dispatched && !bfq_bfqq_busy(bfqq)) {
		BUG_ON(!RB_EMPTY_ROOT(&bfqq->sort_list));
		/*
		 * Set budget_timeout (which we overload to store the
		 * time at which the queue remains with no backlog and
		 * no outstanding request; used by the weight-raising
		 * mechanism).
		 */
		bfqq->budget_timeout = jiffies;

		bfq_weights_tree_remove(bfqd, &bfqq->entity,
					&bfqd->queue_weights_tree);
	}

	now_ns = ktime_get_ns();

	RQ_BIC(rq)->ttime.last_end_request = now_ns;

	delta_us = (now_ns - bfqd->last_completion)/NSEC_PER_USEC;

	bfq_log(bfqd, "rq_completed: delta %uus/%luus max_size %u bw %llu/%llu",
		delta_us, BFQ_MIN_TT/NSEC_PER_USEC, bfqd->last_rq_max_size,
		(USEC_PER_SEC*
		(u64)((bfqd->last_rq_max_size<<BFQ_RATE_SHIFT)/delta_us))
			>>BFQ_RATE_SHIFT,
		(USEC_PER_SEC*(u64)(1UL<<(BFQ_RATE_SHIFT-10)))>>BFQ_RATE_SHIFT);

	/*
	 * If the request took rather long to complete, and, according
	 * to the maximum request size recorded, this completion latency
	 * implies that the request was certainly served at a very low
	 * rate (less than 1M sectors/sec), then the whole observation
	 * interval that lasts up to this time instant cannot be a
	 * valid time interval for computing a new peak rate.  Invoke
	 * bfq_update_rate_reset to have the following three steps
	 * taken:
	 * - close the observation interval at the last (previous)
	 *   request dispatch or completion
	 * - compute rate, if possible, for that observation interval
	 * - reset to zero samples, which will trigger a proper
	 *   re-initialization of the observation interval on next
	 *   dispatch
	 */
	if (delta_us > BFQ_MIN_TT/NSEC_PER_USEC &&
	   (bfqd->last_rq_max_size<<BFQ_RATE_SHIFT)/delta_us <
			1UL<<(BFQ_RATE_SHIFT - 10))
		bfq_update_rate_reset(bfqd, NULL);
	bfqd->last_completion = now_ns;

	/*
	 * If we are waiting to discover whether the request pattern
	 * of the task associated with the queue is actually
	 * isochronous, and both requisites for this condition to hold
	 * are now satisfied, then compute soft_rt_next_start (see the
	 * comments on the function bfq_bfqq_softrt_next_start()). We
	 * schedule this delayed check when bfqq expires, if it still
	 * has in-flight requests.
	 */
	if (bfq_bfqq_softrt_update(bfqq) && bfqq->dispatched == 0 &&
	    RB_EMPTY_ROOT(&bfqq->sort_list))
		bfqq->soft_rt_next_start =
			bfq_bfqq_softrt_next_start(bfqd, bfqq);

	/*
	 * If this is the in-service queue, check if it needs to be expired,
	 * or if we want to idle in case it has no pending requests.
	 */
	if (bfqd->in_service_queue == bfqq) {
		if (bfqq->dispatched == 0 && bfq_bfqq_must_idle(bfqq)) {
			bfq_arm_slice_timer(bfqd);
			goto out;
		} else if (bfq_may_expire_for_budg_timeout(bfqq))
			bfq_bfqq_expire(bfqd, bfqq, false,
					BFQ_BFQQ_BUDGET_TIMEOUT);
		else if (RB_EMPTY_ROOT(&bfqq->sort_list) &&
			 (bfqq->dispatched == 0 ||
			  !bfq_bfqq_may_idle(bfqq)))
			bfq_bfqq_expire(bfqd, bfqq, false,
					BFQ_BFQQ_NO_MORE_REQUESTS);
	}

	if (!bfqd->rq_in_driver)
		bfq_schedule_dispatch(bfqd);

out:
	return;
}

static int __bfq_may_queue(struct bfq_queue *bfqq)
{
	if (bfq_bfqq_wait_request(bfqq) && bfq_bfqq_must_alloc(bfqq)) {
		bfq_clear_bfqq_must_alloc(bfqq);
		return ELV_MQUEUE_MUST;
	}

	return ELV_MQUEUE_MAY;
}

static int bfq_may_queue(struct request_queue *q, int rw)
{
	struct bfq_data *bfqd = q->elevator->elevator_data;
	struct task_struct *tsk = current;
	struct bfq_io_cq *bic;
	struct bfq_queue *bfqq;

	/*
	 * Don't force setup of a queue from here, as a call to may_queue
	 * does not necessarily imply that a request actually will be
	 * queued. So just lookup a possibly existing queue, or return
	 * 'may queue' if that fails.
	 */
	bic = bfq_bic_lookup(bfqd, tsk->io_context);
	if (!bic)
		return ELV_MQUEUE_MAY;

	bfqq = bic_to_bfqq(bic, rw_is_sync(rw));
	if (bfqq)
		return __bfq_may_queue(bfqq);

	return ELV_MQUEUE_MAY;
}

/*
 * Queue lock held here.
 */
static void bfq_put_request(struct request *rq)
{
	struct bfq_queue *bfqq = RQ_BFQQ(rq);

	if (bfqq) {
		const int rw = rq_data_dir(rq);

		BUG_ON(!bfqq->allocated[rw]);
		bfqq->allocated[rw]--;

		rq->elv.priv[0] = NULL;
		rq->elv.priv[1] = NULL;

		bfq_log_bfqq(bfqq->bfqd, bfqq, "put_request %p, %d",
			     bfqq, bfqq->ref);
		bfq_put_queue(bfqq);
	}
}

/*
 * Returns NULL if a new bfqq should be allocated, or the old bfqq if this
 * was the last process referring to that bfqq.
 */
static struct bfq_queue *
bfq_split_bfqq(struct bfq_io_cq *bic, struct bfq_queue *bfqq)
{
	bfq_log_bfqq(bfqq->bfqd, bfqq, "splitting queue");

	put_io_context(bic->icq.ioc);

	if (bfqq_process_refs(bfqq) == 1) {
		bfqq->pid = current->pid;
		bfq_clear_bfqq_coop(bfqq);
		bfq_clear_bfqq_split_coop(bfqq);
		return bfqq;
	}

	bic_set_bfqq(bic, NULL, 1);

	bfq_put_cooperator(bfqq);

	bfq_put_queue(bfqq);
	return NULL;
}

/*
 * Allocate bfq data structures associated with this request.
 */
static int bfq_set_request(struct request_queue *q, struct request *rq,
			   struct bio *bio, gfp_t gfp_mask)
{
	struct bfq_data *bfqd = q->elevator->elevator_data;
	struct bfq_io_cq *bic = icq_to_bic(rq->elv.icq);
	const int rw = rq_data_dir(rq);
	const int is_sync = rq_is_sync(rq);
	struct bfq_queue *bfqq;
	unsigned long flags;
	bool split = false;

	spin_lock_irqsave(q->queue_lock, flags);
	bfq_check_ioprio_change(bic, bio);

	if (!bic)
		goto queue_fail;

	bfq_bic_update_cgroup(bic, bio);

new_queue:
	bfqq = bic_to_bfqq(bic, is_sync);
	if (!bfqq || bfqq == &bfqd->oom_bfqq) {
		if (bfqq)
			bfq_put_queue(bfqq);
		bfqq = bfq_get_queue(bfqd, bio, is_sync, bic);
		BUG_ON(!hlist_unhashed(&bfqq->burst_list_node));

		bic_set_bfqq(bic, bfqq, is_sync);
		if (split && is_sync) {
			bfq_log_bfqq(bfqd, bfqq,
				     "set_request: was_in_list %d "
				     "was_in_large_burst %d "
				     "large burst in progress %d",
				     bic->was_in_burst_list,
				     bic->saved_in_large_burst,
				     bfqd->large_burst);
			if ((bic->was_in_burst_list && bfqd->large_burst) ||
			    bic->saved_in_large_burst) {
				bfq_log_bfqq(bfqd, bfqq,
					     "set_request: marking in "
					     "large burst");
				bfq_mark_bfqq_in_large_burst(bfqq);
			} else {
				bfq_log_bfqq(bfqd, bfqq,
					     "set_request: clearing in "
					     "large burst");
			    bfq_clear_bfqq_in_large_burst(bfqq);
			    if (bic->was_in_burst_list)
			       hlist_add_head(&bfqq->burst_list_node,
				              &bfqd->burst_list);
			}
			bfqq->split_time = jiffies;
		}
	} else {
		/* If the queue was seeky for too long, break it apart. */
		if (bfq_bfqq_coop(bfqq) && bfq_bfqq_split_coop(bfqq)) {
			bfq_log_bfqq(bfqd, bfqq, "breaking apart bfqq");

			/* Update bic before losing reference to bfqq */
			if (bfq_bfqq_in_large_burst(bfqq))
				bic->saved_in_large_burst = true;

			bfqq = bfq_split_bfqq(bic, bfqq);
			split = true;
			if (!bfqq)
				goto new_queue;
		}
	}

	bfqq->allocated[rw]++;
	bfqq->ref++;
	bfq_log_bfqq(bfqd, bfqq, "set_request: bfqq %p, %d", bfqq, bfqq->ref);

	rq->elv.priv[0] = bic;
	rq->elv.priv[1] = bfqq;

	/*
	 * If a bfq_queue has only one process reference, it is owned
	 * by only one bfq_io_cq: we can set the bic field of the
	 * bfq_queue to the address of that structure. Also, if the
	 * queue has just been split, mark a flag so that the
	 * information is available to the other scheduler hooks.
	 */
	if (likely(bfqq != &bfqd->oom_bfqq) && bfqq_process_refs(bfqq) == 1) {
		bfqq->bic = bic;
		if (split) {
			/*
			 * If the queue has just been split from a shared
			 * queue, restore the idle window and the possible
			 * weight raising period.
			 */
			bfq_bfqq_resume_state(bfqq, bic);
		}
	}

	if (unlikely(bfq_bfqq_just_created(bfqq)))
		bfq_handle_burst(bfqd, bfqq);

	spin_unlock_irqrestore(q->queue_lock, flags);

	return 0;

queue_fail:
	bfq_schedule_dispatch(bfqd);
	spin_unlock_irqrestore(q->queue_lock, flags);

	return 1;
}

static void bfq_kick_queue(struct work_struct *work)
{
	struct bfq_data *bfqd =
		container_of(work, struct bfq_data, unplug_work);
	struct request_queue *q = bfqd->queue;

	spin_lock_irq(q->queue_lock);
	__blk_run_queue(q);
	spin_unlock_irq(q->queue_lock);
}

/*
 * Handler of the expiration of the timer running if the in-service queue
 * is idling inside its time slice.
 */
static enum hrtimer_restart bfq_idle_slice_timer(struct hrtimer *timer)
{
	struct bfq_data *bfqd = container_of(timer, struct bfq_data,
					     idle_slice_timer);
	struct bfq_queue *bfqq;
	unsigned long flags;
	enum bfqq_expiration reason;

	spin_lock_irqsave(bfqd->queue->queue_lock, flags);

	bfqq = bfqd->in_service_queue;
	/*
	 * Theoretical race here: the in-service queue can be NULL or
	 * different from the queue that was idling if the timer handler
	 * spins on the queue_lock and a new request arrives for the
	 * current queue and there is a full dispatch cycle that changes
	 * the in-service queue.  This can hardly happen, but in the worst
	 * case we just expire a queue too early.
	 */
	if (bfqq) {
		bfq_log_bfqq(bfqd, bfqq, "slice_timer expired");
		bfq_clear_bfqq_wait_request(bfqq);

		if (bfq_bfqq_budget_timeout(bfqq))
			/*
			 * Also here the queue can be safely expired
			 * for budget timeout without wasting
			 * guarantees
			 */
			reason = BFQ_BFQQ_BUDGET_TIMEOUT;
		else if (bfqq->queued[0] == 0 && bfqq->queued[1] == 0)
			/*
			 * The queue may not be empty upon timer expiration,
			 * because we may not disable the timer when the
			 * first request of the in-service queue arrives
			 * during disk idling.
			 */
			reason = BFQ_BFQQ_TOO_IDLE;
		else
			goto schedule_dispatch;

		bfq_bfqq_expire(bfqd, bfqq, true, reason);
	}

schedule_dispatch:
	bfq_schedule_dispatch(bfqd);

	spin_unlock_irqrestore(bfqd->queue->queue_lock, flags);
	return HRTIMER_NORESTART;
}

static void bfq_shutdown_timer_wq(struct bfq_data *bfqd)
{
	hrtimer_cancel(&bfqd->idle_slice_timer);
	cancel_work_sync(&bfqd->unplug_work);
}

#ifdef CONFIG_BFQ_GROUP_IOSCHED
static void __bfq_put_async_bfqq(struct bfq_data *bfqd,
					struct bfq_queue **bfqq_ptr)
{
	struct bfq_group *root_group = bfqd->root_group;
	struct bfq_queue *bfqq = *bfqq_ptr;

	bfq_log(bfqd, "put_async_bfqq: %p", bfqq);
	if (bfqq) {
		bfq_bfqq_move(bfqd, bfqq, root_group);
		bfq_log_bfqq(bfqd, bfqq, "put_async_bfqq: putting %p, %d",
			     bfqq, bfqq->ref);
		bfq_put_queue(bfqq);
		*bfqq_ptr = NULL;
	}
}

/*
 * Release all the bfqg references to its async queues.  If we are
 * deallocating the group these queues may still contain requests, so
 * we reparent them to the root cgroup (i.e., the only one that will
 * exist for sure until all the requests on a device are gone).
 */
static void bfq_put_async_queues(struct bfq_data *bfqd, struct bfq_group *bfqg)
{
	int i, j;

	for (i = 0; i < 2; i++)
		for (j = 0; j < IOPRIO_BE_NR; j++)
			__bfq_put_async_bfqq(bfqd, &bfqg->async_bfqq[i][j]);

	__bfq_put_async_bfqq(bfqd, &bfqg->async_idle_bfqq);
}
#endif

static void bfq_exit_queue(struct elevator_queue *e)
{
	struct bfq_data *bfqd = e->elevator_data;
	struct request_queue *q = bfqd->queue;
	struct bfq_queue *bfqq, *n;

	bfq_shutdown_timer_wq(bfqd);

	spin_lock_irq(q->queue_lock);

	BUG_ON(bfqd->in_service_queue);
	list_for_each_entry_safe(bfqq, n, &bfqd->idle_list, bfqq_list)
		bfq_deactivate_bfqq(bfqd, bfqq, 0);

	spin_unlock_irq(q->queue_lock);

	bfq_shutdown_timer_wq(bfqd);

	BUG_ON(hrtimer_active(&bfqd->idle_slice_timer));

#ifdef CONFIG_BFQ_GROUP_IOSCHED
	blkcg_deactivate_policy(q, &blkcg_policy_bfq);
#else
	kfree(bfqd->root_group);
#endif

	kfree(bfqd);
}

static void bfq_init_root_group(struct bfq_group *root_group,
				struct bfq_data *bfqd)
{
	int i;

#ifdef CONFIG_BFQ_GROUP_IOSCHED
	root_group->entity.parent = NULL;
	root_group->my_entity = NULL;
	root_group->bfqd = bfqd;
#endif
	root_group->rq_pos_tree = RB_ROOT;
	for (i = 0; i < BFQ_IOPRIO_CLASSES; i++)
		root_group->sched_data.service_tree[i] = BFQ_SERVICE_TREE_INIT;
}

static int bfq_init_queue(struct request_queue *q, struct elevator_type *e)
{
	struct bfq_data *bfqd;
	struct elevator_queue *eq;

	eq = elevator_alloc(q, e);
	if (!eq)
		return -ENOMEM;

	bfqd = kzalloc_node(sizeof(*bfqd), GFP_KERNEL, q->node);
	if (!bfqd) {
		kobject_put(&eq->kobj);
		return -ENOMEM;
	}
	eq->elevator_data = bfqd;

	/*
	 * Our fallback bfqq if bfq_find_alloc_queue() runs into OOM issues.
	 * Grab a permanent reference to it, so that the normal code flow
	 * will not attempt to free it.
	 */
	bfq_init_bfqq(bfqd, &bfqd->oom_bfqq, NULL, 1, 0);
	bfqd->oom_bfqq.ref++;
	bfqd->oom_bfqq.new_ioprio = BFQ_DEFAULT_QUEUE_IOPRIO;
	bfqd->oom_bfqq.new_ioprio_class = IOPRIO_CLASS_BE;
	bfqd->oom_bfqq.entity.new_weight =
		bfq_ioprio_to_weight(bfqd->oom_bfqq.new_ioprio);

	/* oom_bfqq does not participate to bursts */
	bfq_clear_bfqq_just_created(&bfqd->oom_bfqq);
	/*
	 * Trigger weight initialization, according to ioprio, at the
	 * oom_bfqq's first activation. The oom_bfqq's ioprio and ioprio
	 * class won't be changed any more.
	 */
	bfqd->oom_bfqq.entity.prio_changed = 1;

	bfqd->queue = q;

	spin_lock_irq(q->queue_lock);
	q->elevator = eq;
	spin_unlock_irq(q->queue_lock);

	bfqd->root_group = bfq_create_group_hierarchy(bfqd, q->node);
	if (!bfqd->root_group)
		goto out_free;
	bfq_init_root_group(bfqd->root_group, bfqd);
	bfq_init_entity(&bfqd->oom_bfqq.entity, bfqd->root_group);

	hrtimer_init(&bfqd->idle_slice_timer, CLOCK_MONOTONIC,
		     HRTIMER_MODE_REL);
	bfqd->idle_slice_timer.function = bfq_idle_slice_timer;

	bfqd->queue_weights_tree = RB_ROOT;
	bfqd->group_weights_tree = RB_ROOT;

	INIT_WORK(&bfqd->unplug_work, bfq_kick_queue);

	INIT_LIST_HEAD(&bfqd->active_list);
	INIT_LIST_HEAD(&bfqd->idle_list);
	INIT_HLIST_HEAD(&bfqd->burst_list);

	bfqd->hw_tag = -1;

	bfqd->bfq_max_budget = bfq_default_max_budget;

	bfqd->bfq_fifo_expire[0] = bfq_fifo_expire[0];
	bfqd->bfq_fifo_expire[1] = bfq_fifo_expire[1];
	bfqd->bfq_back_max = bfq_back_max;
	bfqd->bfq_back_penalty = bfq_back_penalty;
	bfqd->bfq_slice_idle = bfq_slice_idle;
	bfqd->bfq_class_idle_last_service = 0;
	bfqd->bfq_timeout = bfq_timeout;

	bfqd->bfq_requests_within_timer = 120;

	bfqd->bfq_large_burst_thresh = 8;
	bfqd->bfq_burst_interval = msecs_to_jiffies(180);

	bfqd->low_latency = true;

	/*
	 * Trade-off between responsiveness and fairness.
	 */
	bfqd->bfq_wr_coeff = 30;
	bfqd->bfq_wr_rt_max_time = msecs_to_jiffies(300);
	bfqd->bfq_wr_max_time = 0;
	bfqd->bfq_wr_min_idle_time = msecs_to_jiffies(2000);
	bfqd->bfq_wr_min_inter_arr_async = msecs_to_jiffies(500);
	bfqd->bfq_wr_max_softrt_rate = 7000; /*
					      * Approximate rate required
					      * to playback or record a
					      * high-definition compressed
					      * video.
					      */
	bfqd->wr_busy_queues = 0;

	/*
	 * Begin by assuming, optimistically, that the device is a
	 * high-speed one, and that its peak rate is equal to 2/3 of
	 * the highest reference rate.
	 */
	bfqd->RT_prod = R_fast[blk_queue_nonrot(bfqd->queue)] *
			T_fast[blk_queue_nonrot(bfqd->queue)];
	bfqd->peak_rate = R_fast[blk_queue_nonrot(bfqd->queue)] * 2 / 3;
	bfqd->device_speed = BFQ_BFQD_FAST;

	return 0;

out_free:
	kfree(bfqd);
	kobject_put(&eq->kobj);
	return -ENOMEM;
}

static void bfq_slab_kill(void)
{
	if (bfq_pool)
		kmem_cache_destroy(bfq_pool);
}

static int __init bfq_slab_setup(void)
{
	bfq_pool = KMEM_CACHE(bfq_queue, 0);
	if (!bfq_pool)
		return -ENOMEM;
	return 0;
}

static ssize_t bfq_var_show(unsigned int var, char *page)
{
	return sprintf(page, "%d\n", var);
}

static ssize_t bfq_var_store(unsigned long *var, const char *page,
			     size_t count)
{
	unsigned long new_val;
	int ret = kstrtoul(page, 10, &new_val);

	if (ret == 0)
		*var = new_val;

	return count;
}

static ssize_t bfq_wr_max_time_show(struct elevator_queue *e, char *page)
{
	struct bfq_data *bfqd = e->elevator_data;
	return sprintf(page, "%d\n", bfqd->bfq_wr_max_time > 0 ?
		       jiffies_to_msecs(bfqd->bfq_wr_max_time) :
		       jiffies_to_msecs(bfq_wr_duration(bfqd)));
}

static ssize_t bfq_weights_show(struct elevator_queue *e, char *page)
{
	struct bfq_queue *bfqq;
	struct bfq_data *bfqd = e->elevator_data;
	ssize_t num_char = 0;

	num_char += sprintf(page + num_char, "Tot reqs queued %d\n\n",
			    bfqd->queued);

	spin_lock_irq(bfqd->queue->queue_lock);

	num_char += sprintf(page + num_char, "Active:\n");
	list_for_each_entry(bfqq, &bfqd->active_list, bfqq_list) {
	  num_char += sprintf(page + num_char,
			      "pid%d: weight %hu, nr_queued %d %d, dur %d/%u\n",
			      bfqq->pid,
			      bfqq->entity.weight,
			      bfqq->queued[0],
			      bfqq->queued[1],
			jiffies_to_msecs(jiffies - bfqq->last_wr_start_finish),
			jiffies_to_msecs(bfqq->wr_cur_max_time));
	}

	num_char += sprintf(page + num_char, "Idle:\n");
	list_for_each_entry(bfqq, &bfqd->idle_list, bfqq_list) {
			num_char += sprintf(page + num_char,
				"pid%d: weight %hu, dur %d/%u\n",
				bfqq->pid,
				bfqq->entity.weight,
				jiffies_to_msecs(jiffies -
					bfqq->last_wr_start_finish),
				jiffies_to_msecs(bfqq->wr_cur_max_time));
	}

	spin_unlock_irq(bfqd->queue->queue_lock);

	return num_char;
}

#define SHOW_FUNCTION(__FUNC, __VAR, __CONV)				\
static ssize_t __FUNC(struct elevator_queue *e, char *page)		\
{									\
	struct bfq_data *bfqd = e->elevator_data;			\
	u64 __data = __VAR;						\
	if (__CONV == 1)						\
		__data = jiffies_to_msecs(__data);			\
	else if (__CONV == 2)						\
		__data = div_u64(__data, NSEC_PER_MSEC);		\
	return bfq_var_show(__data, (page));				\
}
SHOW_FUNCTION(bfq_fifo_expire_sync_show, bfqd->bfq_fifo_expire[1], 2);
SHOW_FUNCTION(bfq_fifo_expire_async_show, bfqd->bfq_fifo_expire[0], 2);
SHOW_FUNCTION(bfq_back_seek_max_show, bfqd->bfq_back_max, 0);
SHOW_FUNCTION(bfq_back_seek_penalty_show, bfqd->bfq_back_penalty, 0);
SHOW_FUNCTION(bfq_slice_idle_show, bfqd->bfq_slice_idle, 2);
SHOW_FUNCTION(bfq_max_budget_show, bfqd->bfq_user_max_budget, 0);
SHOW_FUNCTION(bfq_timeout_sync_show, bfqd->bfq_timeout, 1);
SHOW_FUNCTION(bfq_strict_guarantees_show, bfqd->strict_guarantees, 0);
SHOW_FUNCTION(bfq_low_latency_show, bfqd->low_latency, 0);
SHOW_FUNCTION(bfq_wr_coeff_show, bfqd->bfq_wr_coeff, 0);
SHOW_FUNCTION(bfq_wr_rt_max_time_show, bfqd->bfq_wr_rt_max_time, 1);
SHOW_FUNCTION(bfq_wr_min_idle_time_show, bfqd->bfq_wr_min_idle_time, 1);
SHOW_FUNCTION(bfq_wr_min_inter_arr_async_show, bfqd->bfq_wr_min_inter_arr_async,
	1);
SHOW_FUNCTION(bfq_wr_max_softrt_rate_show, bfqd->bfq_wr_max_softrt_rate, 0);
#undef SHOW_FUNCTION

#define USEC_SHOW_FUNCTION(__FUNC, __VAR)				\
static ssize_t __FUNC(struct elevator_queue *e, char *page)		\
{									\
	struct bfq_data *bfqd = e->elevator_data;			\
	u64 __data = __VAR;						\
	__data = div_u64(__data, NSEC_PER_USEC);			\
	return bfq_var_show(__data, (page));				\
}
USEC_SHOW_FUNCTION(bfq_slice_idle_us_show, bfqd->bfq_slice_idle);
#undef USEC_SHOW_FUNCTION

#define STORE_FUNCTION(__FUNC, __PTR, MIN, MAX, __CONV)			\
static ssize_t								\
__FUNC(struct elevator_queue *e, const char *page, size_t count)	\
{									\
	struct bfq_data *bfqd = e->elevator_data;			\
	unsigned long uninitialized_var(__data);			\
	int ret = bfq_var_store(&__data, (page), count);		\
	if (__data < (MIN))						\
		__data = (MIN);						\
	else if (__data > (MAX))					\
		__data = (MAX);						\
	if (__CONV == 1)						\
		*(__PTR) = msecs_to_jiffies(__data);			\
	else if (__CONV == 2)						\
		*(__PTR) = (u64)__data * NSEC_PER_MSEC;			\
	else								\
		*(__PTR) = __data;					\
	return ret;							\
}
STORE_FUNCTION(bfq_fifo_expire_sync_store, &bfqd->bfq_fifo_expire[1], 1,
		INT_MAX, 2);
STORE_FUNCTION(bfq_fifo_expire_async_store, &bfqd->bfq_fifo_expire[0], 1,
		INT_MAX, 2);
STORE_FUNCTION(bfq_back_seek_max_store, &bfqd->bfq_back_max, 0, INT_MAX, 0);
STORE_FUNCTION(bfq_back_seek_penalty_store, &bfqd->bfq_back_penalty, 1,
		INT_MAX, 0);
STORE_FUNCTION(bfq_slice_idle_store, &bfqd->bfq_slice_idle, 0, INT_MAX, 2);
STORE_FUNCTION(bfq_wr_coeff_store, &bfqd->bfq_wr_coeff, 1, INT_MAX, 0);
STORE_FUNCTION(bfq_wr_max_time_store, &bfqd->bfq_wr_max_time, 0, INT_MAX, 1);
STORE_FUNCTION(bfq_wr_rt_max_time_store, &bfqd->bfq_wr_rt_max_time, 0, INT_MAX,
		1);
STORE_FUNCTION(bfq_wr_min_idle_time_store, &bfqd->bfq_wr_min_idle_time, 0,
		INT_MAX, 1);
STORE_FUNCTION(bfq_wr_min_inter_arr_async_store,
		&bfqd->bfq_wr_min_inter_arr_async, 0, INT_MAX, 1);
STORE_FUNCTION(bfq_wr_max_softrt_rate_store, &bfqd->bfq_wr_max_softrt_rate, 0,
		INT_MAX, 0);
#undef STORE_FUNCTION

#define USEC_STORE_FUNCTION(__FUNC, __PTR, MIN, MAX)			\
static ssize_t __FUNC(struct elevator_queue *e, const char *page, size_t count)\
{									\
	struct bfq_data *bfqd = e->elevator_data;			\
	unsigned long __data;						\
	int ret = bfq_var_store(&__data, (page), count);		\
	if (__data < (MIN))						\
		__data = (MIN);						\
	else if (__data > (MAX))					\
		__data = (MAX);						\
	*(__PTR) = (u64)__data * NSEC_PER_USEC;				\
	return ret;							\
}
USEC_STORE_FUNCTION(bfq_slice_idle_us_store, &bfqd->bfq_slice_idle, 0,
		    UINT_MAX);
#undef USEC_STORE_FUNCTION

/* do nothing for the moment */
static ssize_t bfq_weights_store(struct elevator_queue *e,
				    const char *page, size_t count)
{
	return count;
}

static ssize_t bfq_max_budget_store(struct elevator_queue *e,
				    const char *page, size_t count)
{
	struct bfq_data *bfqd = e->elevator_data;
	unsigned long uninitialized_var(__data);
	int ret = bfq_var_store(&__data, (page), count);

	if (__data == 0)
		bfqd->bfq_max_budget = bfq_calc_max_budget(bfqd);
	else {
		if (__data > INT_MAX)
			__data = INT_MAX;
		bfqd->bfq_max_budget = __data;
	}

	bfqd->bfq_user_max_budget = __data;

	return ret;
}

/*
 * Leaving this name to preserve name compatibility with cfq
 * parameters, but this timeout is used for both sync and async.
 */
static ssize_t bfq_timeout_sync_store(struct elevator_queue *e,
				      const char *page, size_t count)
{
	struct bfq_data *bfqd = e->elevator_data;
	unsigned long uninitialized_var(__data);
	int ret = bfq_var_store(&__data, (page), count);

	if (__data < 1)
		__data = 1;
	else if (__data > INT_MAX)
		__data = INT_MAX;

	bfqd->bfq_timeout = msecs_to_jiffies(__data);
	if (bfqd->bfq_user_max_budget == 0)
		bfqd->bfq_max_budget = bfq_calc_max_budget(bfqd);

	return ret;
}

static ssize_t bfq_strict_guarantees_store(struct elevator_queue *e,
				     const char *page, size_t count)
{
	struct bfq_data *bfqd = e->elevator_data;
	unsigned long uninitialized_var(__data);
	int ret = bfq_var_store(&__data, (page), count);

	if (__data > 1)
		__data = 1;
	if (!bfqd->strict_guarantees && __data == 1
	    && bfqd->bfq_slice_idle < 8 * NSEC_PER_MSEC)
		bfqd->bfq_slice_idle = 8 * NSEC_PER_MSEC;

	bfqd->strict_guarantees = __data;

	return ret;
}

static ssize_t bfq_low_latency_store(struct elevator_queue *e,
				     const char *page, size_t count)
{
	struct bfq_data *bfqd = e->elevator_data;
	unsigned long uninitialized_var(__data);
	int ret = bfq_var_store(&__data, (page), count);

	if (__data > 1)
		__data = 1;
	if (__data == 0 && bfqd->low_latency != 0)
		bfq_end_wr(bfqd);
	bfqd->low_latency = __data;

	return ret;
}

#define BFQ_ATTR(name) \
	__ATTR(name, S_IRUGO|S_IWUSR, bfq_##name##_show, bfq_##name##_store)

static struct elv_fs_entry bfq_attrs[] = {
	BFQ_ATTR(fifo_expire_sync),
	BFQ_ATTR(fifo_expire_async),
	BFQ_ATTR(back_seek_max),
	BFQ_ATTR(back_seek_penalty),
	BFQ_ATTR(slice_idle),
	BFQ_ATTR(slice_idle_us),
	BFQ_ATTR(max_budget),
	BFQ_ATTR(timeout_sync),
	BFQ_ATTR(strict_guarantees),
	BFQ_ATTR(low_latency),
	BFQ_ATTR(wr_coeff),
	BFQ_ATTR(wr_max_time),
	BFQ_ATTR(wr_rt_max_time),
	BFQ_ATTR(wr_min_idle_time),
	BFQ_ATTR(wr_min_inter_arr_async),
	BFQ_ATTR(wr_max_softrt_rate),
	BFQ_ATTR(weights),
	__ATTR_NULL
};

static struct elevator_type iosched_bfq = {
	.ops = {
		.elevator_merge_fn =		bfq_merge,
		.elevator_merged_fn =		bfq_merged_request,
		.elevator_merge_req_fn =	bfq_merged_requests,
#ifdef CONFIG_BFQ_GROUP_IOSCHED
		.elevator_bio_merged_fn =	bfq_bio_merged,
#endif
		.elevator_allow_merge_fn =	bfq_allow_merge,
		.elevator_dispatch_fn =		bfq_dispatch_requests,
		.elevator_add_req_fn =		bfq_insert_request,
		.elevator_activate_req_fn =	bfq_activate_request,
		.elevator_deactivate_req_fn =	bfq_deactivate_request,
		.elevator_completed_req_fn =	bfq_completed_request,
		.elevator_former_req_fn =	elv_rb_former_request,
		.elevator_latter_req_fn =	elv_rb_latter_request,
		.elevator_init_icq_fn =		bfq_init_icq,
		.elevator_exit_icq_fn =		bfq_exit_icq,
		.elevator_set_req_fn =		bfq_set_request,
		.elevator_put_req_fn =		bfq_put_request,
		.elevator_may_queue_fn =	bfq_may_queue,
		.elevator_init_fn =		bfq_init_queue,
		.elevator_exit_fn =		bfq_exit_queue,
	},
	.icq_size =		sizeof(struct bfq_io_cq),
	.icq_align =		__alignof__(struct bfq_io_cq),
	.elevator_attrs =	bfq_attrs,
	.elevator_name =	"bfq",
	.elevator_owner =	THIS_MODULE,
};

#ifdef CONFIG_BFQ_GROUP_IOSCHED
static struct blkcg_policy blkcg_policy_bfq = {
	.dfl_cftypes		= bfq_blkg_files,
	.legacy_cftypes		= bfq_blkcg_legacy_files,

	.cpd_alloc_fn		= bfq_cpd_alloc,
	.cpd_init_fn		= bfq_cpd_init,
	.cpd_bind_fn	        = bfq_cpd_init,
	.cpd_free_fn		= bfq_cpd_free,

	.pd_alloc_fn		= bfq_pd_alloc,
	.pd_init_fn		= bfq_pd_init,
	.pd_offline_fn		= bfq_pd_offline,
	.pd_free_fn		= bfq_pd_free,
	.pd_reset_stats_fn	= bfq_pd_reset_stats,
};
#endif

static int __init bfq_init(void)
{
	int ret;
	char msg[50] = "BFQ I/O-scheduler: v8r3";

#ifdef CONFIG_BFQ_GROUP_IOSCHED
	ret = blkcg_policy_register(&blkcg_policy_bfq);
	if (ret)
		return ret;
#endif

	ret = -ENOMEM;
	if (bfq_slab_setup())
		goto err_pol_unreg;

	/*
	 * Times to load large popular applications for the typical
	 * systems installed on the reference devices (see the
	 * comments before the definitions of the next two
	 * arrays). Actually, we use slightly slower values, as the
	 * estimated peak rate tends to be smaller than the actual
	 * peak rate.  The reason for this last fact is that estimates
	 * are computed over much shorter time intervals than the long
	 * intervals typically used for benchmarking. Why? First, to
	 * adapt more quickly to variations. Second, because an I/O
	 * scheduler cannot rely on a peak-rate-evaluation workload to
	 * be run for a long time.
	 */
	T_slow[0] = msecs_to_jiffies(3500); /* actually 4 sec */
	T_slow[1] = msecs_to_jiffies(1000); /* actually 1.5 sec */
	T_fast[0] = msecs_to_jiffies(7000); /* actually 8 sec */
	T_fast[1] = msecs_to_jiffies(2500); /* actually 3 sec */

	/*
	 * Thresholds that determine the switch between speed classes
	 * (see the comments before the definition of the array
	 * device_speed_thresh). These thresholds are biased towards
	 * transitions to the fast class. This is safer than the
	 * opposite bias. In fact, a wrong transition to the slow
	 * class results in short weight-raising periods, because the
	 * speed of the device then tends to be higher that the
	 * reference peak rate. On the opposite end, a wrong
	 * transition to the fast class tends to increase
	 * weight-raising periods, because of the opposite reason.
	 */
	device_speed_thresh[0] = (4 * R_slow[0]) / 3;
	device_speed_thresh[1] = (4 * R_slow[1]) / 3;

	ret = elv_register(&iosched_bfq);
	if (ret)
		goto err_pol_unreg;

#ifdef CONFIG_BFQ_GROUP_IOSCHED
	strcat(msg, " (with cgroups support)");
#endif
	pr_info("%s", msg);

	return 0;

err_pol_unreg:
#ifdef CONFIG_BFQ_GROUP_IOSCHED
	blkcg_policy_unregister(&blkcg_policy_bfq);
#endif
	return ret;
}

static void __exit bfq_exit(void)
{
	elv_unregister(&iosched_bfq);
#ifdef CONFIG_BFQ_GROUP_IOSCHED
	blkcg_policy_unregister(&blkcg_policy_bfq);
#endif
	bfq_slab_kill();
}

module_init(bfq_init);
module_exit(bfq_exit);

MODULE_AUTHOR("Arianna Avanzini, Fabio Checconi, Paolo Valente");
MODULE_LICENSE("GPL");
