/*
 * Marrow I/O Scheduler
 * Based on Maple.
 *
 * Copyright (C) 2018 Draco <tylernij@gmail.com> and Khronodragon <dev@khronodragon.com>
 * 	     (C) 2016 Joe Marrows <joe@frap129.org>
 *
 * Marrow is a dual FIFO scheduler which is contextually biased towards read or write requests.
 * Synchronous requests are executed before asynchronous ones and expire faster because
 * they block the requesting thread, while asynchronous ones can wait.
 *
 * When the screen is on, read requests are preferred for faster app loading and reduced latency.
 * When the screen is off, write requests are preferred because latency doesn't matter, and processes may commit their data to disk. App and system update speeds are also improved.
 *
 * Benchmark scores are also naturally boosted because they are generally synchronous.
 */
#include <linux/blkdev.h>
#include <linux/elevator.h>
#include <linux/bio.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/display_state.h>

#define MARROW_IOSCHED_PATCHLEVEL	(1)

enum { ASYNC, SYNC };

/* Tunables */
static const int sync_read_expire = 25;		/* max time before a read sync is submitted. */
static const int sync_write_expire = 500;	/* max time before a write sync is submitted. */
static const int async_read_expire = 100;	/* ditto for read async, these limits are SOFT! */
static const int async_write_expire = 600;	/* ditto for write async, these limits are SOFT! */
static const int fifo_batch = 2;		/* # of sequential requests treated as one by the above parameters. */
static const int fifo_batch_screen_off = 20;	/* ditto for the fifo_batch for screen off */
static const int sleep_latency_multiple = 50;	/* multple for expire time when device is asleep */

struct marrow_data *mdata;
static int sync;
static int io_type;

/* Elevator data */
struct marrow_data {
	/* Request queues */
	struct list_head fifo_list[2][2];

	/* Attributes */
	unsigned int batched;

	/* Settings */
	int fifo_expire[2][2];
	int fifo_batch;
	int sleep_latency_multiple;
};

static inline struct marrow_data *
marrow_get_data(struct request_queue *q) {
	return q->elevator->elevator_data;
}

static inline void
marrow_merged_requests(struct request_queue *q, struct request *rq,
		    struct request *next)
{
	/*
	 * If next expires before rq, assign its expire time to rq
	 * and move into next position (next will be deleted) in fifo.
	 */
	if (!list_empty(&rq->queuelist) && !list_empty(&next->queuelist) && time_before(next->fifo_time, rq->fifo_time)) {
		list_move(&rq->queuelist, &next->queuelist);
		rq->fifo_time = next->fifo_time;
	}

	/* Delete next request */
	rq_fifo_clear(next);
}

static void
marrow_add_request(struct request_queue *q, struct request *rq)
{
	mdata = marrow_get_data(q);
	sync = rq_is_sync(rq);
	const int dir = rq_data_dir(rq);

	/*
	 * Add request to the proper fifo list and set its
	 * expire time.
	 */

   	/* increase expiration when device is asleep */
   	unsigned int fifo_expire_suspended = mdata->fifo_expire[sync][dir] * sleep_latency_multiple;
   	if (likely(is_display_on() && mdata->fifo_expire[sync][dir])) {
   		rq->fifo_time = jiffies + mdata->fifo_expire[sync][dir];
   		list_add_tail(&rq->queuelist, &mdata->fifo_list[sync][dir]);
   	} else if (!is_display_on() && fifo_expire_suspended) {
		rq->fifo_time = jiffies + mdata->fifo_expire[sync][dir];
   		list_add_tail(&rq->queuelist, &mdata->fifo_list[sync][dir]);
   	}
}

static struct request *
marrow_expired_request(struct marrow_data *mdata, int sync, int io_type)
{
	struct list_head *list = &mdata->fifo_list[sync][io_type];
	struct request *rq;

	if (unlikely(list_empty(list)))
		return NULL;

	/* Retrieve request */
	rq = rq_entry_fifo(list->next);

	/* Request has expired */
    	if (unlikely(time_after(jiffies, rq->fifo_time)))
		return rq;

	return NULL;
}

static struct request *
marrow_choose_expired_request(struct marrow_data *mdata)
{
	struct request *rq_sync_read = marrow_expired_request(mdata, SYNC, READ);
	struct request *rq_sync_write = marrow_expired_request(mdata, SYNC, WRITE);
	struct request *rq_async_read = marrow_expired_request(mdata, ASYNC, READ);
	struct request *rq_async_write = marrow_expired_request(mdata, ASYNC, WRITE);

	/* Reset (non-expired-)batch-counter */
	mdata->batched = 0;

	/*
	 * Check expired requests.
	 * Synchronous requests have priority over asynchronous.
	 * Read requests have priority over write.
	 * Do not moderate write speeds because we only care about UI latency.
	 */

	if (likely(is_display_on())) {
		if (rq_async_read && rq_sync_read) {
			return rq_sync_read;
		} else if (rq_sync_read) {
			return rq_sync_read;
		} else if (rq_async_read) {
			return rq_async_read;
		}

		if (rq_async_write && rq_sync_write) {
			return rq_sync_write;
		} else if (rq_sync_write) {
			return rq_sync_write;
		} else if (rq_async_write) {
			return rq_async_write;
		}
	} else {
		if (rq_async_write && rq_sync_write) {
			return rq_async_write;
		} else if (rq_async_write) {
			return rq_async_write;
		} else if (rq_sync_write) {
			return rq_sync_write;
		}

		if (rq_async_read && rq_sync_read) {
			return rq_async_read;
		} else if (rq_async_read) {
			return rq_async_read;
		} else if (rq_sync_read) {
			return rq_sync_read;
		}
	}

	return NULL;
}

static struct request *
marrow_choose_request(struct marrow_data *mdata, int io_type)
{
	struct list_head *sync = mdata->fifo_list[SYNC];
	struct list_head *async = mdata->fifo_list[ASYNC];

	/* Increase (non-expired-)batch-counter */
	mdata->batched++;

	/*
	 * Retrieve request from available fifo list.
	 * Synchronous requests have priority over asynchronous.
	 * Read requests have priority over write.
	 */
	if (likely(is_display_on())) {
		if (!list_empty(&sync[io_type]))
			return rq_entry_fifo(sync[io_type].next);
		if (!list_empty(&async[io_type]))
			return rq_entry_fifo(async[io_type].next);

		if (!list_empty(&sync[!io_type]))
			return rq_entry_fifo(sync[!io_type].next);
		if (!list_empty(&async[!io_type]))
			return rq_entry_fifo(async[!io_type].next);
	} else {
		if (!list_empty(&async[!io_type]))
			return rq_entry_fifo(async[!io_type].next);
		if (!list_empty(&sync[!io_type]))
			return rq_entry_fifo(sync[!io_type].next);

		if (!list_empty(&async[io_type]))
			return rq_entry_fifo(async[io_type].next);
		if (!list_empty(&sync[io_type]))
			return rq_entry_fifo(sync[io_type].next);
	}


	return NULL;
}

static inline void
marrow_dispatch_request(struct marrow_data *mdata, struct request *rq)
{
	/*
	 * Remove the request from the fifo list
	 * and dispatch it.
	 */
	rq_fifo_clear(rq);
	elv_dispatch_add_tail(rq->q, rq);
}

static inline int
marrow_dispatch_requests(struct request_queue *q, int force)
{
	mdata = marrow_get_data(q);
	struct request *rq = NULL;
	int io_type = READ;

	/*
	 * Retrieve any expired request after a batch of
	 * sequential requests.
	 * Assume unlikely to accelerate current read requests (less latency).
	 */
	if (unlikely(is_display_on() && mdata->batched >= mdata->fifo_batch))
		rq = marrow_choose_expired_request(mdata);

	/* Retrieve request */
	if (likely(!rq)) {
		/* Treat writes fairly while suspended, otherwise pick when read feed is empty */
		if (unlikely(!is_display_on() || (list_empty(&mdata->fifo_list[SYNC][READ]) && list_empty(&mdata->fifo_list[ASYNC][READ]))))
			io_type = WRITE;

		rq = marrow_choose_request(mdata, io_type);
		if (unlikely(!rq)) return 0;
	}

	/* Dispatch request */
	marrow_dispatch_request(mdata, rq);

	return 1;
}

static struct request *
marrow_former_request(struct request_queue *q, struct request *rq)
{
	mdata = marrow_get_data(q);
	sync = rq_is_sync(rq);
	io_type = rq_data_dir(rq);

	if (unlikely(rq->queuelist.prev == &mdata->fifo_list[sync][io_type]))
		return NULL;

	/* Return former request */
	return list_entry(rq->queuelist.prev, struct request, queuelist);
}

static struct request *
marrow_latter_request(struct request_queue *q, struct request *rq)
{
	mdata = marrow_get_data(q);
	sync = rq_is_sync(rq);
	io_type = rq_data_dir(rq);

	if (unlikely(rq->queuelist.next == &mdata->fifo_list[sync][io_type]))
		return NULL;

	/* Return latter request */
	return list_entry(rq->queuelist.next, struct request, queuelist);
}

static inline int marrow_init_queue(struct request_queue *q, struct elevator_type *e)
{
	struct elevator_queue *eq = elevator_alloc(q, e);
	if (unlikely(!eq))
		return -ENOMEM;

	/* Allocate structure */
	mdata = kmalloc_node(sizeof(*mdata), GFP_KERNEL, q->node);
	if (unlikely(!mdata)) {
		kobject_put(&eq->kobj);
		return -ENOMEM;
	}
	eq->elevator_data = mdata;

	/* Initialize fifo lists */
	INIT_LIST_HEAD(&mdata->fifo_list[SYNC][READ]);
	INIT_LIST_HEAD(&mdata->fifo_list[SYNC][WRITE]);
	INIT_LIST_HEAD(&mdata->fifo_list[ASYNC][READ]);
	INIT_LIST_HEAD(&mdata->fifo_list[ASYNC][WRITE]);

	/* Initialize data */
	mdata->batched = 0;
	mdata->fifo_expire[SYNC][READ] = sync_read_expire;
	mdata->fifo_expire[SYNC][WRITE] = sync_write_expire;
	mdata->fifo_expire[ASYNC][READ] = async_read_expire;
	mdata->fifo_expire[ASYNC][WRITE] = async_write_expire;
	mdata->fifo_batch = (is_display_on() ? fifo_batch : fifo_batch_screen_off);
	mdata->sleep_latency_multiple = sleep_latency_multiple;

	spin_lock_irq(q->queue_lock);
	q->elevator = eq;
	spin_unlock_irq(q->queue_lock);
	return 0;
}

static inline void
marrow_exit_queue(struct elevator_queue *e)
{
	mdata = e->elevator_data;
	kfree(mdata);
}

/*
 * sysfs code
 */

#define SHOW_FUNCTION(__FUNC, __VAR, __CONV)				\
static ssize_t __FUNC(struct elevator_queue *e, char *page)		\
{									\
	mdata = e->elevator_data;			\
	int __data = __VAR;						\
	if (__CONV)							\
		__data = jiffies_to_msecs(__data);			\
	return sprintf((page), "%d\n", __data);			\
}
SHOW_FUNCTION(marrow_sync_read_expire_show, mdata->fifo_expire[SYNC][READ], 1);
SHOW_FUNCTION(marrow_sync_write_expire_show, mdata->fifo_expire[SYNC][WRITE], 1);
SHOW_FUNCTION(marrow_async_read_expire_show, mdata->fifo_expire[ASYNC][READ], 1);
SHOW_FUNCTION(marrow_async_write_expire_show, mdata->fifo_expire[ASYNC][WRITE], 1);
SHOW_FUNCTION(marrow_fifo_batch_show, mdata->fifo_batch, 0);
SHOW_FUNCTION(marrow_sleep_latency_multiple_show, mdata->sleep_latency_multiple, 0);
#undef SHOW_FUNCTION

#define STORE_FUNCTION(__FUNC, __PTR, MIN, MAX, __CONV)			\
static ssize_t __FUNC(struct elevator_queue *e, const char *page, size_t count)	\
{									\
	mdata = e->elevator_data;			\
	int __data;					\
	char *p = (char *) page;			\
	__data = simple_strtol(p, &p, 10);			\
	int ret = count;					\
	if (__data < (MIN))						\
		__data = (MIN);						\
	else if (__data > (MAX))					\
		__data = (MAX);						\
	if (__CONV)							\
		*(__PTR) = msecs_to_jiffies(__data);			\
	else								\
		*(__PTR) = __data;					\
	return ret;							\
}
STORE_FUNCTION(marrow_sync_read_expire_store, &mdata->fifo_expire[SYNC][READ], 0, INT_MAX, 1);
STORE_FUNCTION(marrow_sync_write_expire_store, &mdata->fifo_expire[SYNC][WRITE], 0, INT_MAX, 1);
STORE_FUNCTION(marrow_async_read_expire_store, &mdata->fifo_expire[ASYNC][READ], 0, INT_MAX, 1);
STORE_FUNCTION(marrow_async_write_expire_store, &mdata->fifo_expire[ASYNC][WRITE], 0, INT_MAX, 1);
STORE_FUNCTION(marrow_fifo_batch_store, &mdata->fifo_batch, 1, INT_MAX, 0);
STORE_FUNCTION(marrow_sleep_latency_multiple_store, &mdata->sleep_latency_multiple, 1, INT_MAX, 0);
#undef STORE_FUNCTION

#define DD_ATTR(name) \
	__ATTR(name, S_IRUGO|S_IWUSR, marrow_##name##_show, \
				      marrow_##name##_store)

static struct elv_fs_entry marrow_attrs[] = {
	DD_ATTR(sync_read_expire),
	DD_ATTR(sync_write_expire),
	DD_ATTR(async_read_expire),
	DD_ATTR(async_write_expire),
	DD_ATTR(fifo_batch),
  	DD_ATTR(sleep_latency_multiple),
	__ATTR_NULL
};

static struct elevator_type iosched_marrow = {
	.ops = {
		.elevator_merge_req_fn		= marrow_merged_requests,
		.elevator_dispatch_fn		= marrow_dispatch_requests,
		.elevator_add_req_fn		= marrow_add_request,
		.elevator_former_req_fn		= marrow_former_request,
		.elevator_latter_req_fn		= marrow_latter_request,
		.elevator_init_fn		= marrow_init_queue,
		.elevator_exit_fn		= marrow_exit_queue,
	},

	.elevator_attrs = marrow_attrs,
	.elevator_name = "marrow",
	.elevator_owner = THIS_MODULE,
};

static inline int __init marrow_init(void)
{
	elv_register(&iosched_marrow);
	return 0;
}

static inline void __exit marrow_exit(void)
{
	elv_unregister(&iosched_marrow);
}

module_init(marrow_init);
module_exit(marrow_exit);

MODULE_AUTHOR("Draco & kdragon");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Marrow I/O Scheduler");
MODULE_VERSION("1.0");
