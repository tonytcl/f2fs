/**
 * fs/f2fs/gc.c
 *
 * Copyright (c) 2012 Samsung Electronics Co., Ltd.
 * http://www.samsung.com/
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/backing-dev.h>
#include <linux/proc_fs.h>
#include <linux/init.h>
#include <linux/f2fs_fs.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/freezer.h>
#include <linux/blkdev.h>

#include "f2fs.h"
#include "node.h"
#include "segment.h"
#include "gc.h"

static LIST_HEAD(f2fs_stat_list);
static struct kmem_cache *winode_slab;

static int gc_thread_func(void *data)
{
	struct f2fs_sb_info *sbi = data;
	wait_queue_head_t *wq = &sbi->gc_thread->gc_wait_queue_head;
	long wait_ms;

	wait_ms = GC_THREAD_MIN_SLEEP_TIME;

	do {
	 if (try_to_freeze())
	 continue;
	 else
	 wait_event_interruptible_timeout(*wq,
	 kthread_should_stop(),
	 msecs_to_jiffies(wait_ms));
	 if (kthread_should_stop())
	 break;

	 f2fs_balance_fs(sbi);

	 if (!test_opt(sbi, BG_GC))
	 continue;

	 /*
	 * [GC triggering condition]
	 * 0. GC is not conducted currently.
	 * 1. There are enough dirty segments.
	 * 2. IO subsystem is idle by checking the # of writeback pages.
	 * 3. IO subsystem is idle by checking the # of requests in
	 * bdev's request list.
	 *
	 * Note) We have to avoid triggering GCs too much frequently.
	 * Because it is possible that some segments can be
	 * invalidated soon after by user update or deletion.
	 * So, I'd like to wait some time to collect dirty segments.
	 */
	 if (!mutex_trylock(&sbi->gc_mutex))
	 continue;

	 if (!is_idle(sbi)) {
	 wait_ms = increase_sleep_time(wait_ms);
	 mutex_unlock(&sbi->gc_mutex);
	 continue;
	 }

	 if (has_enough_invalid_blocks(sbi))
	 wait_ms = decrease_sleep_time(wait_ms);
	 else
	 wait_ms = increase_sleep_time(wait_ms);

	 sbi->bg_gc++;

	 if (f2fs_gc(sbi, 1) == GC_NONE)
	 wait_ms = GC_THREAD_NOGC_SLEEP_TIME;
	 else if (wait_ms == GC_THREAD_NOGC_SLEEP_TIME)
	 wait_ms = GC_THREAD_MAX_SLEEP_TIME;

	} while (!kthread_should_stop());
	return 0;
}

int start_gc_thread(struct f2fs_sb_info *sbi)
{
	struct f2fs_gc_kthread *gc_th = NULL;

	gc_th = kmalloc(sizeof(struct f2fs_gc_kthread), GFP_KERNEL);
	if (!gc_th)
	 return -ENOMEM;

	sbi->gc_thread = gc_th;
	init_waitqueue_head(&sbi->gc_thread->gc_wait_queue_head);
	sbi->gc_thread->f2fs_gc_task = kthread_run(gc_thread_func, sbi,
	 GC_THREAD_NAME);
	if (IS_ERR(gc_th->f2fs_gc_task)) {
	 kfree(gc_th);
	 return -ENOMEM;
	}
	return 0;
}

void stop_gc_thread(struct f2fs_sb_info *sbi)
{
	struct f2fs_gc_kthread *gc_th = sbi->gc_thread;
	if (!gc_th)
	 return;
	kthread_stop(gc_th->f2fs_gc_task);
	kfree(gc_th);
	sbi->gc_thread = NULL;
}

static int select_gc_type(int gc_type)
{
	return (gc_type == BG_GC) ? GC_CB : GC_GREEDY;
}

static void select_policy(struct f2fs_sb_info *sbi, int gc_type,
	 int type, struct victim_sel_policy *p)
{
	struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);

	if (IS_SSR_TYPE(type)) {
	 p->alloc_mode = SSR;
	 p->gc_mode = GC_GREEDY;
	 p->type = GET_SSR_TYPE(type);
	 p->dirty_segmap = dirty_i->dirty_segmap[p->type];
	 p->log_ofs_unit = 0;
	} else {
	 p->alloc_mode = LFS;
	 p->gc_mode = select_gc_type(gc_type);
	 p->type = 0;
	 p->dirty_segmap = dirty_i->dirty_segmap[DIRTY];
	 p->log_ofs_unit = sbi->log_segs_per_sec;
	}
	p->offset = sbi->last_victim[p->gc_mode];
}

static unsigned int get_max_cost(struct f2fs_sb_info *sbi,
	 struct victim_sel_policy *p)
{
	if (p->gc_mode == GC_GREEDY)
	 return 1 << (sbi->log_blocks_per_seg + p->log_ofs_unit);
	else if (p->gc_mode == GC_CB)
	 return UINT_MAX;
	else /* No other gc_mode */
	 return 0;
}

static unsigned int check_bg_victims(struct f2fs_sb_info *sbi)
{
	struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);
	unsigned int segno;

	/*
	 * If the gc_type is FG_GC, we can select victim segments
	 * selected by background GC before.
	 * Those segments guarantee they have small valid blocks.
	 */
	segno = find_next_bit(dirty_i->victim_segmap[BG_GC],
	 TOTAL_SEGS(sbi), 0);
	if (segno < TOTAL_SEGS(sbi)) {
	 clear_bit(segno, dirty_i->victim_segmap[BG_GC]);
	 return segno;
	}
	return NULL_SEGNO;
}

static unsigned int get_cb_cost(struct f2fs_sb_info *sbi, unsigned int segno)
{
	struct sit_info *sit_i = SIT_I(sbi);
	unsigned int secno = GET_SECNO(sbi, segno);
	unsigned int start = secno << sbi->log_segs_per_sec;
	unsigned long long mtime = 0;
	unsigned int vblocks;
	unsigned char age = 0;
	unsigned char u;
	unsigned int i;

	for (i = 0; i < sbi->segs_per_sec; i++)
	 mtime += get_seg_entry(sbi, start + i)->mtime;
	vblocks = get_valid_blocks(sbi, segno, sbi->log_segs_per_sec);

	mtime >>= sbi->log_segs_per_sec;
	vblocks >>= sbi->log_segs_per_sec;

	u = (vblocks * 100) >> sbi->log_blocks_per_seg;

	/* Handle if the system time is changed by user */
	if (mtime < sit_i->min_mtime)
	 sit_i->min_mtime = mtime;
	if (mtime > sit_i->max_mtime)
	 sit_i->max_mtime = mtime;
	if (sit_i->max_mtime != sit_i->min_mtime)
	 age = 100 - div64_64(100 * (mtime - sit_i->min_mtime),
	 sit_i->max_mtime - sit_i->min_mtime);

	return UINT_MAX - ((100 * (100 - u) * age) / (100 + u));
}

static unsigned int get_gc_cost(struct f2fs_sb_info *sbi, unsigned int segno,
	 struct victim_sel_policy *p)
{
	if (p->alloc_mode == SSR)
	 return get_seg_entry(sbi, segno)->ckpt_valid_blocks;

	/* alloc_mode == LFS */
	if (p->gc_mode == GC_GREEDY)
	 return get_valid_blocks(sbi, segno, sbi->log_segs_per_sec);
	else
	 return get_cb_cost(sbi, segno);
}

/**
 * This function is called from two pathes.
 * One is garbage collection and the other is SSR segment selection.
 * When it is called during GC, it just gets a victim segment
 * and it does not remove it from dirty seglist.
 * When it is called from SSR segment selection, it finds a segment
 * which has minimum valid blocks and removes it from dirty seglist.
 */
static int get_victim_by_default(struct f2fs_sb_info *sbi,
	 unsigned int *result, int gc_type, int type)
{
	struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);
	struct victim_sel_policy p;
	unsigned int segno;
	int nsearched = 0;

	select_policy(sbi, gc_type, type, &p);

	p.min_segno = NULL_SEGNO;
	p.min_cost = get_max_cost(sbi, &p);

	mutex_lock(&dirty_i->seglist_lock);

	if (p.alloc_mode == LFS && gc_type == FG_GC) {
	 p.min_segno = check_bg_victims(sbi);
	 if (p.min_segno != NULL_SEGNO)
	 goto got_it;
	}

	while (1) {
	 unsigned long cost;

	 segno = find_next_bit(p.dirty_segmap,
	 TOTAL_SEGS(sbi), p.offset);
	 if (segno >= TOTAL_SEGS(sbi)) {
	 if (sbi->last_victim[p.gc_mode]) {
	 sbi->last_victim[p.gc_mode] = 0;
	 p.offset = 0;
	 continue;
	 }
	 break;
	 }
	 p.offset = ((segno >> p.log_ofs_unit) << p.log_ofs_unit)
	 + (1 << p.log_ofs_unit);

	 if (test_bit(segno, dirty_i->victim_segmap[FG_GC]))
	 continue;
	 if (gc_type == BG_GC &&
	 test_bit(segno, dirty_i->victim_segmap[BG_GC]))
	 continue;
	 if (IS_CURSEC(sbi, GET_SECNO(sbi, segno)))
	 continue;

	 cost = get_gc_cost(sbi, segno, &p);

	 if (p.min_cost > cost) {
	 p.min_segno = segno;
	 p.min_cost = cost;
	 }

	 if (cost == get_max_cost(sbi, &p))
	 continue;

	 if (nsearched++ >= MAX_VICTIM_SEARCH) {
	 sbi->last_victim[p.gc_mode] = segno;
	 break;
	 }
	}
got_it:
	if (p.min_segno != NULL_SEGNO) {
	 *result = (p.min_segno >> p.log_ofs_unit) << p.log_ofs_unit;
	 if (p.alloc_mode == LFS) {
	 int i;
	 for (i = 0; i < (1 << p.log_ofs_unit); i++)
	 set_bit(*result + i,
	 dirty_i->victim_segmap[gc_type]);
	 }
	}
	mutex_unlock(&dirty_i->seglist_lock);

	return (p.min_segno == NULL_SEGNO) ? 0 : 1;
}

static const struct victim_selection default_v_ops = {
	.get_victim = get_victim_by_default,
};

static struct inode *find_gc_inode(nid_t ino, struct list_head *ilist)
{
	struct list_head *this;
	struct inode_entry *ie;

	list_for_each(this, ilist) {
	 ie = list_entry(this, struct inode_entry, list);
	 if (ie->inode->i_ino == ino)
	 return ie->inode;
	}
	return NULL;
}

static void add_gc_inode(struct inode *inode, struct list_head *ilist)
{
	struct list_head *this;
	struct inode_entry *new_ie, *ie;

	list_for_each(this, ilist) {
	 ie = list_entry(this, struct inode_entry, list);
	 if (ie->inode == inode) {
	 iput(inode);
	 return;
	 }
	}
repeat:
	new_ie = kmem_cache_alloc(winode_slab, GFP_NOFS);
	if (!new_ie) {
	 cond_resched();
	 goto repeat;
	}
	new_ie->inode = inode;
	list_add_tail(&new_ie->list, ilist);
}

static void put_gc_inode(struct list_head *ilist)
{
	struct inode_entry *ie, *next_ie;
	list_for_each_entry_safe(ie, next_ie, ilist, list) {
	 iput(ie->inode);
	 list_del(&ie->list);
	 kmem_cache_free(winode_slab, ie);
	}
}

static int check_valid_map(struct f2fs_sb_info *sbi,
	 unsigned int segno, int offset)
{
	struct sit_info *sit_i = SIT_I(sbi);
	struct seg_entry *sentry;
	int ret;

	mutex_lock(&sit_i->sentry_lock);
	sentry = get_seg_entry(sbi, segno);
	ret = f2fs_test_bit(offset, sentry->cur_valid_map);
	mutex_unlock(&sit_i->sentry_lock);
	return ret ? GC_OK : GC_NEXT;
}

/**
 * This function compares node address got in summary with that in NAT.
 * On validity, copy that node with cold status, otherwise (invalid node)
 * ignore that.
 */
static int gc_node_segment(struct f2fs_sb_info *sbi,
	 struct f2fs_summary *sum, unsigned int segno, int gc_type)
{
	bool initial = true;
	struct f2fs_summary *entry;
	int off;

next_step:
	entry = sum;
	for (off = 0; off < sbi->blocks_per_seg; off++, entry++) {
	 nid_t nid = le32_to_cpu(entry->nid);
	 struct page *node_page;
	 int err;

	 /*
	 * It makes sure that free segments are able to write
	 * all the dirty node pages before CP after this CP.
	 * So let's check the space of dirty node pages.
	 */
	 if (should_do_checkpoint(sbi)) {
	 mutex_lock(&sbi->cp_mutex);
	 block_operations(sbi);
	 return GC_BLOCKED;
	 }

	 err = check_valid_map(sbi, segno, off);
	 if (err == GC_ERROR)
	 return err;
	 else if (err == GC_NEXT)
	 continue;

	 if (initial) {
	 ra_node_page(sbi, nid);
	 continue;
	 }
	 node_page = get_node_page(sbi, nid);
	 if (IS_ERR(node_page))
	 continue;

	 /* set page dirty and write it */
	 if (!PageWriteback(node_page))
	 set_page_dirty(node_page);
	 f2fs_put_page(node_page, 1);
	 gc_stat_inc_node_blk_count(sbi, 1);
	}
	if (initial) {
	 initial = false;
	 goto next_step;
	}

	if (gc_type == FG_GC) {
	 struct writeback_control wbc = {
	 .sync_mode = WB_SYNC_ALL,
	 .nr_to_write = LONG_MAX,
	 .for_reclaim = 0,
	 };
	 sync_node_pages(sbi, 0, &wbc);
	}
	return GC_DONE;
}

/**
 * Calculate start block index that this node page contains
 */
block_t start_bidx_of_node(unsigned int node_ofs)
{
	block_t start_bidx;
	unsigned int bidx, indirect_blks;
	int dec;

	indirect_blks = 2 * NIDS_PER_BLOCK + 4;

	start_bidx = 1;
	if (node_ofs == 0) {
	 start_bidx = 0;
	} else if (node_ofs <= 2) {
	 bidx = node_ofs - 1;
	} else if (node_ofs <= indirect_blks) {
	 dec = (node_ofs - 4) / (NIDS_PER_BLOCK + 1);
	 bidx = node_ofs - 2 - dec;
	} else {
	 dec = (node_ofs - indirect_blks - 3) / (NIDS_PER_BLOCK + 1);
	 bidx = node_ofs - 5 - dec;
	}

	if (start_bidx)
	 start_bidx = bidx * ADDRS_PER_BLOCK + ADDRS_PER_INODE;
	return start_bidx;
}

static int check_dnode(struct f2fs_sb_info *sbi, struct f2fs_summary *sum,
	 struct node_info *dni, block_t blkaddr, unsigned int *nofs)
{
	struct page *node_page;
	nid_t nid;
	unsigned int ofs_in_node;
	block_t source_blkaddr;

	nid = le32_to_cpu(sum->nid);
	ofs_in_node = le16_to_cpu(sum->ofs_in_node);

	node_page = get_node_page(sbi, nid);
	if (IS_ERR(node_page))
	 return GC_NEXT;

	get_node_info(sbi, nid, dni);

	if (sum->version != dni->version) {
	 f2fs_put_page(node_page, 1);
	 return GC_NEXT;
	}

	*nofs = ofs_of_node(node_page);
	source_blkaddr = datablock_addr(node_page, ofs_in_node);
	f2fs_put_page(node_page, 1);

	if (source_blkaddr != blkaddr)
	 return GC_NEXT;
	return GC_OK;
}

static void move_data_page(struct inode *inode, struct page *page, int gc_type)
{
	if (page->mapping != inode->i_mapping)
	 goto out;

	if (inode != page->mapping->host)
	 goto out;

	if (PageWriteback(page))
	 goto out;

	if (gc_type == BG_GC) {
	 set_page_dirty(page);
	 set_cold_data(page);
	} else {
	 struct f2fs_sb_info *sbi = F2FS_SB(inode->i_sb);
	 mutex_lock_op(sbi, DATA_WRITE);
	 if (clear_page_dirty_for_io(page) &&
	 S_ISDIR(inode->i_mode)) {
	 dec_page_count(sbi, F2FS_DIRTY_DENTS);
	 inode_dec_dirty_dents(inode);
	 }
	 set_cold_data(page);
	 do_write_data_page(page);
	 mutex_unlock_op(sbi, DATA_WRITE);
	 clear_cold_data(page);
	}
out:
	f2fs_put_page(page, 1);
}

/**
 * This function tries to get parent node of victim data block, and identifies
 * data block validity. If the block is valid, copy that with cold status and
 * modify parent node.
 * If the parent node is not valid or the data block address is different,
 * the victim data block is ignored.
 */
static int gc_data_segment(struct f2fs_sb_info *sbi, struct f2fs_summary *sum,
	 struct list_head *ilist, unsigned int segno, int gc_type)
{
	struct super_block *sb = sbi->sb;
	struct f2fs_summary *entry;
	block_t start_addr;
	int err, off;
	int phase = 0;

	start_addr = START_BLOCK(sbi, segno);

next_step:
	entry = sum;
	for (off = 0; off < sbi->blocks_per_seg; off++, entry++) {
	 struct page *data_page;
	 struct inode *inode;
	 struct node_info dni; /* dnode info for the data */
	 unsigned int ofs_in_node, nofs;
	 block_t start_bidx;

	 /*
	 * It makes sure that free segments are able to write
	 * all the dirty node pages before CP after this CP.
	 * So let's check the space of dirty node pages.
	 */
	 if (should_do_checkpoint(sbi)) {
	 mutex_lock(&sbi->cp_mutex);
	 block_operations(sbi);
	 err = GC_BLOCKED;
	 goto stop;
	 }

	 err = check_valid_map(sbi, segno, off);
	 if (err == GC_ERROR)
	 goto stop;
	 else if (err == GC_NEXT)
	 continue;

	 if (phase == 0) {
	 ra_node_page(sbi, le32_to_cpu(entry->nid));
	 continue;
	 }

	 /* Get an inode by ino with checking validity */
	 err = check_dnode(sbi, entry, &dni, start_addr + off, &nofs);
	 if (err == GC_ERROR)
	 goto stop;
	 else if (err == GC_NEXT)
	 continue;

	 if (phase == 1) {
	 ra_node_page(sbi, dni.ino);
	 continue;
	 }

	 start_bidx = start_bidx_of_node(nofs);
	 ofs_in_node = le16_to_cpu(entry->ofs_in_node);

	 if (phase == 2) {
	 inode = f2fs_iget_nowait(sb, dni.ino);
	 if (IS_ERR(inode))
	 continue;

	 data_page = find_data_page(inode,
	 start_bidx + ofs_in_node);
	 if (IS_ERR(data_page))
	 goto next_iput;

	 f2fs_put_page(data_page, 0);
	 add_gc_inode(inode, ilist);
	 } else {
	 inode = find_gc_inode(dni.ino, ilist);
	 if (inode) {
	 data_page = get_lock_data_page(inode,
	 start_bidx + ofs_in_node);
	 if (IS_ERR(data_page))
	 continue;
	 move_data_page(inode, data_page, gc_type);
	 gc_stat_inc_data_blk_count(sbi, 1);
	 }
	 }
	 continue;
next_iput:
	 iput(inode);
	}
	if (++phase < 4)
	 goto next_step;
	err = GC_DONE;
stop:
	if (gc_type == FG_GC)
	 f2fs_submit_bio(sbi, DATA, true);
	return err;
}

static int __get_victim(struct f2fs_sb_info *sbi, unsigned int *result,
	 int gc_type, int type)
{
	struct sit_info *sit_i = SIT_I(sbi);
	int ret;
	mutex_lock(&sit_i->sentry_lock);
	ret = DIRTY_I(sbi)->v_ops->get_victim(sbi, result, gc_type, type);
	mutex_unlock(&sit_i->sentry_lock);
	return ret;
}

static int do_garbage_collect(struct f2fs_sb_info *sbi, unsigned int segno,
	 struct list_head *ilist, int gc_type)
{
	struct page *sum_page;
	struct f2fs_summary_block *sum;
	int ret = GC_DONE;

	/* read segment summary of victim */
	sum_page = get_sum_page(sbi, segno);
	if (IS_ERR(sum_page))
	 return GC_ERROR;

	/*
	 * CP needs to lock sum_page. In this time, we don't need
	 * to lock this page, because this summary page is not gone anywhere.
	 * Also, this page is not gonna be updated before GC is done.
	 */
	unlock_page(sum_page);
	sum = page_address(sum_page);

	switch (GET_SUM_TYPE((&sum->footer))) {
	case SUM_TYPE_NODE:
	 ret = gc_node_segment(sbi, sum->entries, segno, gc_type);
	 break;
	case SUM_TYPE_DATA:
	 ret = gc_data_segment(sbi, sum->entries, ilist, segno, gc_type);
	 break;
	}
	gc_stat_inc_seg_count(sbi, GET_SUM_TYPE((&sum->footer)));
	gc_stat_inc_call_count(sbi->gc_info);

	f2fs_put_page(sum_page, 0);
	return ret;
}

int f2fs_gc(struct f2fs_sb_info *sbi, int nGC)
{
	unsigned int segno;
	int old_free_secs, cur_free_secs;
	int gc_status, nfree;
	struct list_head ilist;
	int gc_type = BG_GC;

	INIT_LIST_HEAD(&ilist);
gc_more:
	nfree = 0;
	gc_status = GC_NONE;

	if (has_not_enough_free_secs(sbi))
	 old_free_secs = reserved_sections(sbi);
	else
	 old_free_secs = free_sections(sbi);

	while (sbi->sb->s_flags & MS_ACTIVE) {
	 int i;
	 if (has_not_enough_free_secs(sbi))
	 gc_type = FG_GC;

	 cur_free_secs = free_sections(sbi) + nfree;

	 /* We got free space successfully. */
	 if (nGC < cur_free_secs - old_free_secs)
	 break;

	 if (!__get_victim(sbi, &segno, gc_type, NO_CHECK_TYPE))
	 break;

	 for (i = 0; i < sbi->segs_per_sec; i++) {
	 /*
	 * do_garbage_collect will give us three gc_status:
	 * GC_ERROR, GC_DONE, and GC_BLOCKED.
	 * If GC is finished uncleanly, we have to return
	 * the victim to dirty segment list.
	 */
	 gc_status = do_garbage_collect(sbi, segno + i,
	 &ilist, gc_type);
	 if (gc_status != GC_DONE)
	 goto stop;
	 nfree++;
	 }
	}
stop:
	if (has_not_enough_free_secs(sbi) || gc_status == GC_BLOCKED) {
	 write_checkpoint(sbi, (gc_status == GC_BLOCKED), false);
	 if (nfree)
	 goto gc_more;
	}
	sbi->last_gc_status = gc_status;
	mutex_unlock(&sbi->gc_mutex);

	put_gc_inode(&ilist);
	BUG_ON(!list_empty(&ilist));
	return gc_status;
}

#ifdef CONFIG_F2FS_STAT_FS
void f2fs_update_stat(struct f2fs_sb_info *sbi)
{
	struct f2fs_gc_info *gc_i = sbi->gc_info;
	struct f2fs_stat_info *si = gc_i->stat_info;
	int i;

	/* valid check of the segment numbers */
	si->hit_ext = sbi->read_hit_ext;
	si->total_ext = sbi->total_hit_ext;
	si->ndirty_node = get_pages(sbi, F2FS_DIRTY_NODES);
	si->ndirty_dent = get_pages(sbi, F2FS_DIRTY_DENTS);
	si->ndirty_dirs = sbi->n_dirty_dirs;
	si->ndirty_meta = get_pages(sbi, F2FS_DIRTY_META);
	si->total_count = (int)sbi->user_block_count / sbi->blocks_per_seg;
	si->rsvd_segs = reserved_segments(sbi);
	si->overp_segs = overprovision_segments(sbi);
	si->valid_count = valid_user_blocks(sbi);
	si->valid_node_count = valid_node_count(sbi);
	si->valid_inode_count = valid_inode_count(sbi);
	si->utilization = utilization(sbi);

	si->free_segs = free_segments(sbi);
	si->free_secs = free_sections(sbi);
	si->prefree_count = prefree_segments(sbi);
	si->dirty_count = dirty_segments(sbi);
	si->node_pages = sbi->node_inode->i_mapping->nrpages;
	si->meta_pages = sbi->meta_inode->i_mapping->nrpages;
	si->nats = NM_I(sbi)->nat_cnt;
	si->sits = SIT_I(sbi)->dirty_sentries;
	si->fnids = NM_I(sbi)->fcnt;
	si->bg_gc = sbi->bg_gc;
	si->util_free = (int)(free_user_blocks(sbi) >> sbi->log_blocks_per_seg)
	 * 100 / (int)(sbi->user_block_count >> sbi->log_blocks_per_seg)
	 / 2;
	si->util_valid = (int)(written_block_count(sbi) >>
	 sbi->log_blocks_per_seg)
	 * 100 / (int)(sbi->user_block_count >> sbi->log_blocks_per_seg)
	 / 2;
	si->util_invalid = 50 - si->util_free - si->util_valid;
	for (i = CURSEG_HOT_DATA; i <= CURSEG_COLD_NODE; i++) {
	 struct curseg_info *curseg = CURSEG_I(sbi, i);
	 si->curseg[i] = curseg->segno;
	 si->cursec[i] = curseg->segno >> sbi->log_segs_per_sec;
	 si->curzone[i] = si->cursec[i] / sbi->secs_per_zone;
	}

	for (i = 0; i < 2; i++) {
	 si->segment_count[i] = sbi->segment_count[i];
	 si->block_count[i] = sbi->block_count[i];
	}
}

/**
 * This function calculates BDF of every segments
 */
void f2fs_update_gc_metric(struct f2fs_sb_info *sbi)
{
	struct f2fs_gc_info *gc_i = sbi->gc_info;
	struct f2fs_stat_info *si = gc_i->stat_info;
	unsigned int blks_per_sec, hblks_per_sec, total_vblocks, bimodal, dist;
	struct sit_info *sit_i = SIT_I(sbi);
	unsigned int segno, vblocks;
	int ndirty = 0;

	bimodal = 0;
	total_vblocks = 0;
	blks_per_sec = 1 << (sbi->log_segs_per_sec + sbi->log_blocks_per_seg);
	hblks_per_sec = blks_per_sec / 2;
	mutex_lock(&sit_i->sentry_lock);
	for (segno = 0; segno < TOTAL_SEGS(sbi); segno += sbi->segs_per_sec) {
	 vblocks = get_valid_blocks(sbi, segno, sbi->log_segs_per_sec);
	 dist = abs(vblocks - hblks_per_sec);
	 bimodal += dist * dist;

	 if (vblocks > 0 && vblocks < blks_per_sec) {
	 total_vblocks += vblocks;
	 ndirty++;
	 }
	}
	mutex_unlock(&sit_i->sentry_lock);
	dist = sbi->total_sections * hblks_per_sec * hblks_per_sec / 100;
	si->bimodal = bimodal / dist;
	if (si->dirty_count)
	 si->avg_vblocks = total_vblocks / ndirty;
	else
	 si->avg_vblocks = 0;
}

static int f2fs_read_gc(char *page, char **start, off_t off,
	 int count, int *eof, void *data)
{
	struct f2fs_gc_info *gc_i, *next;
	struct f2fs_stat_info *si;
	char *buf = page;
	int i = 0;

	list_for_each_entry_safe(gc_i, next, &f2fs_stat_list, stat_list) {
	 int j;
	 si = gc_i->stat_info;

	 mutex_lock(&si->stat_list);
	 if (!si->sbi) {
	 mutex_unlock(&si->stat_list);
	 continue;
	 }
	 f2fs_update_stat(si->sbi);

	 buf += sprintf(buf, "=====[ partition info. #%d ]=====\n", i++);
	 buf += sprintf(buf, "[SB: 1] [CP: 2] [NAT: %d] [SIT: %d] ",
	 si->nat_area_segs, si->sit_area_segs);
	 buf += sprintf(buf, "[SSA: %d] [MAIN: %d",
	 si->ssa_area_segs, si->main_area_segs);
	 buf += sprintf(buf, "(OverProv:%d Resv:%d)]\n\n",
	 si->overp_segs, si->rsvd_segs);
	 buf += sprintf(buf, "Utilization: %d%% (%d valid blocks)\n",
	 si->utilization, si->valid_count);
	 buf += sprintf(buf, " - Node: %u (Inode: %u, ",
	 si->valid_node_count, si->valid_inode_count);
	 buf += sprintf(buf, "Other: %u)\n - Data: %u\n",
	 si->valid_node_count - si->valid_inode_count,
	 si->valid_count - si->valid_node_count);
	 buf += sprintf(buf, "\nMain area: %d segs, %d secs %d zones\n",
	 si->main_area_segs, si->main_area_sections,
	 si->main_area_zones);
	 buf += sprintf(buf, " - COLD data: %d, %d, %d\n",
	 si->curseg[CURSEG_COLD_DATA],
	 si->cursec[CURSEG_COLD_DATA],
	 si->curzone[CURSEG_COLD_DATA]);
	 buf += sprintf(buf, " - WARM data: %d, %d, %d\n",
	 si->curseg[CURSEG_WARM_DATA],
	 si->cursec[CURSEG_WARM_DATA],
	 si->curzone[CURSEG_WARM_DATA]);
	 buf += sprintf(buf, " - HOT data: %d, %d, %d\n",
	 si->curseg[CURSEG_HOT_DATA],
	 si->cursec[CURSEG_HOT_DATA],
	 si->curzone[CURSEG_HOT_DATA]);
	 buf += sprintf(buf, " - Dir dnode: %d, %d, %d\n",
	 si->curseg[CURSEG_HOT_NODE],
	 si->cursec[CURSEG_HOT_NODE],
	 si->curzone[CURSEG_HOT_NODE]);
	 buf += sprintf(buf, " - File dnode: %d, %d, %d\n",
	 si->curseg[CURSEG_WARM_NODE],
	 si->cursec[CURSEG_WARM_NODE],
	 si->curzone[CURSEG_WARM_NODE]);
	 buf += sprintf(buf, " - Indir nodes: %d, %d, %d\n",
	 si->curseg[CURSEG_COLD_NODE],
	 si->cursec[CURSEG_COLD_NODE],
	 si->curzone[CURSEG_COLD_NODE]);
	 buf += sprintf(buf, "\n - Valid: %d\n - Dirty: %d\n",
	 si->main_area_segs - si->dirty_count -
	 si->prefree_count - si->free_segs,
	 si->dirty_count);
	 buf += sprintf(buf, " - Prefree: %d\n - Free: %d (%d)\n\n",
	 si->prefree_count,
	 si->free_segs,
	 si->free_secs);
	 buf += sprintf(buf, "GC calls: %d (BG: %d)\n",
	 si->call_count, si->bg_gc);
	 buf += sprintf(buf, " - data segments : %d\n", si->data_segs);
	 buf += sprintf(buf, " - node segments : %d\n", si->node_segs);
	 buf += sprintf(buf, "Try to move %d blocks\n", si->tot_blks);
	 buf += sprintf(buf, " - data blocks : %d\n", si->data_blks);
	 buf += sprintf(buf, " - node blocks : %d\n", si->node_blks);
	 buf += sprintf(buf, "\nExtent Hit Ratio: %d / %d\n",
	 si->hit_ext, si->total_ext);
	 buf += sprintf(buf, "\nBalancing F2FS Async:\n");
	 buf += sprintf(buf, " - nodes %4d in %4d\n",
	 si->ndirty_node, si->node_pages);
	 buf += sprintf(buf, " - dents %4d in dirs:%4d\n",
	 si->ndirty_dent, si->ndirty_dirs);
	 buf += sprintf(buf, " - meta %4d in %4d\n",
	 si->ndirty_meta, si->meta_pages);
	 buf += sprintf(buf, " - NATs %5d > %lu\n",
	 si->nats, NM_WOUT_THRESHOLD);
	 buf += sprintf(buf, " - SITs: %5d\n - free_nids: %5d\n",
	 si->sits, si->fnids);
	 buf += sprintf(buf, "\nDistribution of User Blocks:");
	 buf += sprintf(buf, " [ valid | invalid | free ]\n");
	 buf += sprintf(buf, " [");
	 for (j = 0; j < si->util_valid; j++)
	 buf += sprintf(buf, "-");
	 buf += sprintf(buf, "|");
	 for (j = 0; j < si->util_invalid; j++)
	 buf += sprintf(buf, "-");
	 buf += sprintf(buf, "|");
	 for (j = 0; j < si->util_free; j++)
	 buf += sprintf(buf, "-");
	 buf += sprintf(buf, "]\n\n");
	 buf += sprintf(buf, "SSR: %u blocks in %u segments\n",
	 si->block_count[SSR], si->segment_count[SSR]);
	 buf += sprintf(buf, "LFS: %u blocks in %u segments\n",
	 si->block_count[LFS], si->segment_count[LFS]);
	 mutex_unlock(&si->stat_list);
	}
	return buf - page;
}

static int f2fs_read_sit(char *page, char **start, off_t off,
	 int count, int *eof, void *data)
{
	struct f2fs_gc_info *gc_i, *next;
	struct f2fs_stat_info *si;
	char *buf = page;

	list_for_each_entry_safe(gc_i, next, &f2fs_stat_list, stat_list) {
	 si = gc_i->stat_info;

	 mutex_lock(&si->stat_list);
	 if (!si->sbi) {
	 mutex_unlock(&si->stat_list);
	 continue;
	 }
	 f2fs_update_gc_metric(si->sbi);

	 buf += sprintf(buf, "BDF: %u, avg. vblocks: %u\n",
	 si->bimodal, si->avg_vblocks);
	 mutex_unlock(&si->stat_list);
	}
	return buf - page;
}

static int f2fs_read_mem(char *page, char **start, off_t off,
	 int count, int *eof, void *data)
{
	struct f2fs_gc_info *gc_i, *next;
	struct f2fs_stat_info *si;
	char *buf = page;

	list_for_each_entry_safe(gc_i, next, &f2fs_stat_list, stat_list) {
	 struct f2fs_sb_info *sbi = gc_i->stat_info->sbi;
	 unsigned npages;
	 unsigned base_mem = 0, cache_mem = 0;

	 si = gc_i->stat_info;
	 mutex_lock(&si->stat_list);
	 if (!si->sbi) {
	 mutex_unlock(&si->stat_list);
	 continue;
	 }
	 base_mem += sizeof(struct f2fs_sb_info) + sbi->sb->s_blocksize;
	 base_mem += 2 * sizeof(struct f2fs_inode_info);
	 base_mem += sizeof(*sbi->ckpt);

	 /* build sm */
	 base_mem += sizeof(struct f2fs_sm_info);

	 /* build sit */
	 base_mem += sizeof(struct sit_info);
	 base_mem += TOTAL_SEGS(sbi) * sizeof(struct seg_entry);
	 base_mem += f2fs_bitmap_size(TOTAL_SEGS(sbi));
	 base_mem += 2 * SIT_VBLOCK_MAP_SIZE * TOTAL_SEGS(sbi);
	 if (sbi->log_segs_per_sec)
	 base_mem += sbi->total_sections *
	 sizeof(struct sec_entry);
	 base_mem += __bitmap_size(sbi, SIT_BITMAP);

	 /* build free segmap */
	 base_mem += sizeof(struct free_segmap_info);
	 base_mem += f2fs_bitmap_size(TOTAL_SEGS(sbi));
	 base_mem += f2fs_bitmap_size(sbi->total_sections);

	 /* build curseg */
	 base_mem += sizeof(struct curseg_info) * DEFAULT_CURSEGS;
	 base_mem += PAGE_CACHE_SIZE * DEFAULT_CURSEGS;

	 /* build dirty segmap */
	 base_mem += sizeof(struct dirty_seglist_info);
	 base_mem += NR_DIRTY_TYPE * f2fs_bitmap_size(TOTAL_SEGS(sbi));
	 base_mem += 2 * f2fs_bitmap_size(TOTAL_SEGS(sbi));

	 /* buld nm */
	 base_mem += sizeof(struct f2fs_nm_info);
	 base_mem += __bitmap_size(sbi, NAT_BITMAP);

	 /* build gc */
	 base_mem += sizeof(struct f2fs_gc_info);
	 base_mem += sizeof(struct f2fs_gc_kthread);

	 /* free nids */
	 cache_mem += NM_I(sbi)->fcnt;
	 cache_mem += NM_I(sbi)->nat_cnt;
	 npages = sbi->node_inode->i_mapping->nrpages;
	 cache_mem += npages << PAGE_CACHE_SHIFT;
	 npages = sbi->meta_inode->i_mapping->nrpages;
	 cache_mem += npages << PAGE_CACHE_SHIFT;
	 cache_mem += sbi->n_orphans * sizeof(struct orphan_inode_entry);
	 cache_mem += sbi->n_dirty_dirs * sizeof(struct dir_inode_entry);

	 buf += sprintf(buf, "%u KB = static: %u + cached: %u\n",
	 (base_mem + cache_mem) >> 10,
	 base_mem >> 10,
	 cache_mem >> 10);
	 mutex_unlock(&si->stat_list);
	}
	return buf - page;
}

int f2fs_stat_init(struct f2fs_sb_info *sbi)
{
	struct proc_dir_entry *entry;

	entry = create_proc_entry("f2fs_stat", 0, sbi->s_proc);
	if (!entry)
	 return -ENOMEM;
	entry->read_proc = f2fs_read_gc;
	entry->write_proc = NULL;

	entry = create_proc_entry("f2fs_sit_stat", 0, sbi->s_proc);
	if (!entry) {
	 remove_proc_entry("f2fs_stat", sbi->s_proc);
	 return -ENOMEM;
	}
	entry->read_proc = f2fs_read_sit;
	entry->write_proc = NULL;
	entry = create_proc_entry("f2fs_mem_stat", 0, sbi->s_proc);
	if (!entry) {
	 remove_proc_entry("f2fs_sit_stat", sbi->s_proc);
	 remove_proc_entry("f2fs_stat", sbi->s_proc);
	 return -ENOMEM;
	}
	entry->read_proc = f2fs_read_mem;
	entry->write_proc = NULL;
	return 0;
}

void f2fs_stat_exit(struct f2fs_sb_info *sbi)
{
	if (sbi->s_proc) {
	 remove_proc_entry("f2fs_stat", sbi->s_proc);
	 remove_proc_entry("f2fs_sit_stat", sbi->s_proc);
	 remove_proc_entry("f2fs_mem_stat", sbi->s_proc);
	}
}
#endif

int build_gc_manager(struct f2fs_sb_info *sbi)
{
	struct f2fs_gc_info *gc_i;
	struct f2fs_checkpoint *ckp = F2FS_CKPT(sbi);
#ifdef CONFIG_F2FS_STAT_FS
	struct f2fs_super_block *raw_super = F2FS_RAW_SUPER(sbi);
	struct f2fs_stat_info *si;
#endif

	gc_i = kzalloc(sizeof(struct f2fs_gc_info), GFP_KERNEL);
	if (!gc_i)
	 return -ENOMEM;

	sbi->gc_info = gc_i;
	gc_i->rsvd_segment_count = le32_to_cpu(ckp->rsvd_segment_count);
	gc_i->overp_segment_count = le32_to_cpu(ckp->overprov_segment_count);

	DIRTY_I(sbi)->v_ops = &default_v_ops;

#ifdef CONFIG_F2FS_STAT_FS
	gc_i->stat_info = kzalloc(sizeof(struct f2fs_stat_info),
	 GFP_KERNEL);
	if (!gc_i->stat_info)
	 return -ENOMEM;
	si = gc_i->stat_info;
	mutex_init(&si->stat_list);
	list_add_tail(&gc_i->stat_list, &f2fs_stat_list);

	si->all_area_segs = le32_to_cpu(raw_super->segment_count);
	si->sit_area_segs = le32_to_cpu(raw_super->segment_count_sit);
	si->nat_area_segs = le32_to_cpu(raw_super->segment_count_nat);
	si->ssa_area_segs = le32_to_cpu(raw_super->segment_count_ssa);
	si->main_area_segs = le32_to_cpu(raw_super->segment_count_main);
	si->main_area_sections = le32_to_cpu(raw_super->section_count);
	si->main_area_zones = si->main_area_sections /
	 le32_to_cpu(raw_super->secs_per_zone);
	si->sbi = sbi;
#endif
	return 0;
}

void destroy_gc_manager(struct f2fs_sb_info *sbi)
{
	struct f2fs_gc_info *gc_i = sbi->gc_info;
#ifdef CONFIG_F2FS_STAT_FS
	struct f2fs_stat_info *si = gc_i->stat_info;
#endif
	if (!gc_i)
	 return;

#ifdef CONFIG_F2FS_STAT_FS
	list_del(&gc_i->stat_list);
	mutex_lock(&si->stat_list);
	si->sbi = NULL;
	mutex_unlock(&si->stat_list);
	kfree(gc_i->stat_info);
#endif
	sbi->gc_info = NULL;
	kfree(gc_i);
}

int create_gc_caches(void)
{
	winode_slab = f2fs_kmem_cache_create("f2fs_gc_inodes",
	 sizeof(struct inode_entry), NULL);
	if (!winode_slab)
	 return -ENOMEM;
	return 0;
}

void destroy_gc_caches(void)
{
	kmem_cache_destroy(winode_slab);
}
