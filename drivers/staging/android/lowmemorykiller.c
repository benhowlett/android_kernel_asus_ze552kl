/* drivers/misc/lowmemorykiller.c
 *
 * The lowmemorykiller driver lets user-space specify a set of memory thresholds
 * where processes with a range of oom_score_adj values will get killed. Specify
 * the minimum oom_score_adj values in
 * /sys/module/lowmemorykiller/parameters/adj and the number of free pages in
 * /sys/module/lowmemorykiller/parameters/minfree. Both files take a comma
 * separated list of numbers in ascending order.
 *
 * For example, write "0,8" to /sys/module/lowmemorykiller/parameters/adj and
 * "1024,4096" to /sys/module/lowmemorykiller/parameters/minfree to kill
 * processes with a oom_score_adj value of 8 or higher when the free memory
 * drops below 4096 pages and kill processes with a oom_score_adj value of 0 or
 * higher when the free memory drops below 1024 pages.
 *
 * The driver considers memory used for caches to be free, but if a large
 * percentage of the cached memory is locked this can be very inaccurate
 * and processes may not get killed until the normal oom killer is triggered.
 *
 * Copyright (C) 2007-2008 Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/oom.h>
#include <linux/sched.h>
#include <linux/swap.h>
#include <linux/rcupdate.h>
#include <linux/notifier.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/swap.h>
#include <linux/fs.h>
#include <linux/cpuset.h>
#include <linux/vmpressure.h>
#include <linux/zcache.h>
#include <linux/slab.h>

#define CREATE_TRACE_POINTS
#include <trace/events/almk.h>

#ifdef CONFIG_HIGHMEM
#define _ZONE ZONE_HIGHMEM
#else
#define _ZONE ZONE_NORMAL
#endif

#define CREATE_TRACE_POINTS
#include "trace/lowmemorykiller.h"

static unsigned long lowmem_killNothing_timeout = 0;
static unsigned long lowmem_justkilled_timeout = 0;
static int killNothing = 0;
static int killNothing_adj = 2000;
static int justkilled_adj = 2000;
static bool trigger_dumpsys_meminfo = false;
static unsigned long trigger_dumpsys_meminfo_time;
static bool trigger_dump_mem = false;
static bool print_log_flag = false;
static struct work_struct __dumpmem_work;
struct work_struct __dumpthread_work;
static int dumppid;
static int dumppid_tasksize;
static uint32_t lowmem_debug_level = 1;
static short lowmem_adj[6] = {
	0,
	1,
	6,
	12,
};
static int lowmem_adj_size = 4;
static int lowmem_minfree[6] = {
	3 * 512,	/* 6MB */
	2 * 1024,	/* 8MB */
	4 * 1024,	/* 16MB */
	16 * 1024,	/* 64MB */
};
static int lowmem_minfree_size = 4;
static int lmk_fast_run = 1;

//static unsigned long lowmem_deathpending_timeout;

#define lowmem_print(level, x...)			\
	do {						\
		if (lowmem_debug_level >= (level))	\
			pr_info(x);			\
	} while (0)

static unsigned long lowmem_count(struct shrinker *s,
				  struct shrink_control *sc)
{
	return global_page_state(NR_ACTIVE_ANON) +
		global_page_state(NR_ACTIVE_FILE) +
		global_page_state(NR_INACTIVE_ANON) +
		global_page_state(NR_INACTIVE_FILE);
}

static atomic_t shift_adj = ATOMIC_INIT(0);
static short adj_max_shift = 353;

unsigned long time_out;
unsigned long print_log_time_out;
unsigned long dumpmem_time_out;
/* User knob to enable/disable adaptive lmk feature */
static int enable_adaptive_lmk;
module_param_named(enable_adaptive_lmk, enable_adaptive_lmk, int,
	S_IRUGO | S_IWUSR);

/*
 * This parameter controls the behaviour of LMK when vmpressure is in
 * the range of 90-94. Adaptive lmk triggers based on number of file
 * pages wrt vmpressure_file_min, when vmpressure is in the range of
 * 90-94. Usually this is a pseudo minfree value, higher than the
 * highest configured value in minfree array.
 */
static int vmpressure_file_min;
module_param_named(vmpressure_file_min, vmpressure_file_min, int,
	S_IRUGO | S_IWUSR);

enum {
	VMPRESSURE_NO_ADJUST = 0,
	VMPRESSURE_ADJUST_ENCROACH,
	VMPRESSURE_ADJUST_NORMAL,
};

int adjust_minadj(short *min_score_adj)
{
	int ret = VMPRESSURE_NO_ADJUST;

	if (!enable_adaptive_lmk)
		return 0;

	if (atomic_read(&shift_adj) &&
		(*min_score_adj > adj_max_shift)) {
		if (*min_score_adj == OOM_SCORE_ADJ_MAX + 1)
			ret = VMPRESSURE_ADJUST_ENCROACH;
		else
			ret = VMPRESSURE_ADJUST_NORMAL;
		*min_score_adj = adj_max_shift;
	}
	atomic_set(&shift_adj, 0);

	return ret;
}

static int lmk_vmpressure_notifier(struct notifier_block *nb,
			unsigned long action, void *data)
{
	int other_free, other_file;
	unsigned long pressure = action;
	int array_size = ARRAY_SIZE(lowmem_adj);
	
	if (!enable_adaptive_lmk)
		return 0;
	#if 0
	if (pressure >= 95) {
		other_file = global_page_state(NR_FILE_PAGES) + zcache_pages() -
			global_page_state(NR_SHMEM) -
			total_swapcache_pages();
		other_free = global_page_state(NR_FREE_PAGES);

		atomic_set(&shift_adj, 1);
		trace_almk_vmpressure(pressure, other_free, other_file);
	} else 
	#endif
	//if the pressure is larger than 98, and the other file is larger than vmpressure_file_min, activate the a_lmk
	if (pressure >= 98) {
		//printk("lowmem:lmk_vmpressure_notifier pressure=%ld\n", pressure);
		if (lowmem_adj_size < array_size)
			array_size = lowmem_adj_size;
		if (lowmem_minfree_size < array_size)
			array_size = lowmem_minfree_size;

		other_file = global_page_state(NR_FILE_PAGES) + zcache_pages() -
			global_page_state(NR_SHMEM) -
			total_swapcache_pages();

		other_free = global_page_state(NR_FREE_PAGES);
		
		if ((other_free < lowmem_minfree[array_size - 1]) &&
			(other_file < vmpressure_file_min)) {
				atomic_set(&shift_adj, 1);
				trace_almk_vmpressure(pressure, other_free,
					other_file);
				//printk("lowmem:lmk_vmpressure_notifier doing aLMK , other_file=%d, lowmem_minfree[array_size - 1]=%d, other_free=%d,\n", other_file, lowmem_minfree[array_size - 1], other_free);
		}
	} else if (atomic_read(&shift_adj)) {
		/*
		 * shift_adj would have been set by a previous invocation
		 * of notifier, which is not followed by a lowmem_shrink yet.
		 * Since vmpressure has improved, reset shift_adj to avoid
		 * false adaptive LMK trigger.
		 */
		trace_almk_vmpressure(pressure, other_free, other_file);
		atomic_set(&shift_adj, 0);
	}

	return 0;
}

static struct notifier_block lmk_vmpr_nb = {
	.notifier_call = lmk_vmpressure_notifier,
};

static int test_task_flag(struct task_struct *p, int flag)
{
	struct task_struct *t;

	for_each_thread(p, t) {
		task_lock(t);
		if (test_tsk_thread_flag(t, flag)) {
			task_unlock(t);
			return 1;
		}
		task_unlock(t);
	}

	return 0;
}

static DEFINE_MUTEX(scan_mutex);

int can_use_cma_pages(gfp_t gfp_mask)
{
	int can_use = 0;
	int mtype = gfpflags_to_migratetype(gfp_mask);
	int i = 0;
	int *mtype_fallbacks = get_migratetype_fallbacks(mtype);

	if (is_migrate_cma(mtype)) {
		can_use = 1;
	} else {
		for (i = 0;; i++) {
			int fallbacktype = mtype_fallbacks[i];

			if (is_migrate_cma(fallbacktype)) {
				can_use = 1;
				break;
			}

			if (fallbacktype == MIGRATE_RESERVE)
				break;
		}
	}
	return can_use;
}

void tune_lmk_zone_param(struct zonelist *zonelist, int classzone_idx,
					int *other_free, int *other_file,
					int use_cma_pages)
{
	struct zone *zone;
	struct zoneref *zoneref;
	int zone_idx;

	for_each_zone_zonelist(zone, zoneref, zonelist, MAX_NR_ZONES) {
		zone_idx = zonelist_zone_idx(zoneref);
		if (zone_idx == ZONE_MOVABLE) {
			if (!use_cma_pages && other_free)
				*other_free -=
				    zone_page_state(zone, NR_FREE_CMA_PAGES);
			continue;
		}

		if (zone_idx > classzone_idx) {
			if (other_free != NULL)
				*other_free -= zone_page_state(zone,
							       NR_FREE_PAGES);
			if (other_file != NULL)
				*other_file -= zone_page_state(zone,
							       NR_FILE_PAGES)
					- zone_page_state(zone, NR_SHMEM)
					- zone_page_state(zone, NR_SWAPCACHE);
		} else if (zone_idx < classzone_idx) {
			if (zone_watermark_ok(zone, 0, 0, classzone_idx, 0) &&
			    other_free) {
				if (!use_cma_pages) {
					*other_free -= min(
					  zone->lowmem_reserve[classzone_idx] +
					  zone_page_state(
					    zone, NR_FREE_CMA_PAGES),
					  zone_page_state(
					    zone, NR_FREE_PAGES));
				} else {
					*other_free -=
					  zone->lowmem_reserve[classzone_idx];
				}
			} else {
				if (other_free)
					*other_free -=
					  zone_page_state(zone, NR_FREE_PAGES);
			}
		}
	}
}

#ifdef CONFIG_HIGHMEM
void adjust_gfp_mask(gfp_t *gfp_mask)
{
	struct zone *preferred_zone;
	struct zonelist *zonelist;
	enum zone_type high_zoneidx;

	if (current_is_kswapd()) {
		zonelist = node_zonelist(0, *gfp_mask);
		high_zoneidx = gfp_zone(*gfp_mask);
		first_zones_zonelist(zonelist, high_zoneidx, NULL,
				&preferred_zone);

		if (high_zoneidx == ZONE_NORMAL) {
			if (zone_watermark_ok_safe(preferred_zone, 0,
					high_wmark_pages(preferred_zone), 0,
					0))
				*gfp_mask |= __GFP_HIGHMEM;
		} else if (high_zoneidx == ZONE_HIGHMEM) {
			*gfp_mask |= __GFP_HIGHMEM;
		}
	}
}
#else
void adjust_gfp_mask(gfp_t *unused)
{
}
#endif

void tune_lmk_param(int *other_free, int *other_file, struct shrink_control *sc)
{
	gfp_t gfp_mask;
	struct zone *preferred_zone;
	struct zonelist *zonelist;
	enum zone_type high_zoneidx, classzone_idx;
	unsigned long balance_gap;
	int use_cma_pages;

	gfp_mask = sc->gfp_mask;
	adjust_gfp_mask(&gfp_mask);

	zonelist = node_zonelist(0, gfp_mask);
	high_zoneidx = gfp_zone(gfp_mask);
	first_zones_zonelist(zonelist, high_zoneidx, NULL, &preferred_zone);
	classzone_idx = zone_idx(preferred_zone);
	use_cma_pages = can_use_cma_pages(gfp_mask);

	balance_gap = min(low_wmark_pages(preferred_zone),
			  (preferred_zone->present_pages +
			   KSWAPD_ZONE_BALANCE_GAP_RATIO-1) /
			   KSWAPD_ZONE_BALANCE_GAP_RATIO);

	if (likely(current_is_kswapd() && zone_watermark_ok(preferred_zone, 0,
			  high_wmark_pages(preferred_zone) + SWAP_CLUSTER_MAX +
			  balance_gap, 0, 0))) {
		if (lmk_fast_run)
			tune_lmk_zone_param(zonelist, classzone_idx, other_free,
				       other_file, use_cma_pages);
		else
			tune_lmk_zone_param(zonelist, classzone_idx, other_free,
				       NULL, use_cma_pages);

		if (zone_watermark_ok(preferred_zone, 0, 0, _ZONE, 0)) {
			if (!use_cma_pages) {
				*other_free -= min(
				  preferred_zone->lowmem_reserve[_ZONE]
				  + zone_page_state(
				    preferred_zone, NR_FREE_CMA_PAGES),
				  zone_page_state(
				    preferred_zone, NR_FREE_PAGES));
			} else {
				*other_free -=
				  preferred_zone->lowmem_reserve[_ZONE];
			}
		} else {
			*other_free -= zone_page_state(preferred_zone,
						      NR_FREE_PAGES);
		}

		lowmem_print(4, "lowmem_shrink of kswapd tunning for highmem "
			     "ofree %d, %d\n", *other_free, *other_file);
	} else {
		tune_lmk_zone_param(zonelist, classzone_idx, other_free,
			       other_file, use_cma_pages);

		if (!use_cma_pages) {
			*other_free -=
			  zone_page_state(preferred_zone, NR_FREE_CMA_PAGES);
		}

		lowmem_print(4, "lowmem_shrink tunning for others ofree %d, "
			     "%d\n", *other_free, *other_file);
	}
}

#define DTaskMax 4
struct task_node {
     struct list_head node;
     struct task_struct *pTask;
};
enum EListAllocate {
	EListAllocate_Init,
	EListAllocate_Head,
	EListAllocate_Body,
	EListAllocate_Tail
};
static int list_insert(struct task_struct *pTask, struct list_head *pPosition)
{
	int nResult = 0;
	struct task_node *pTaskNode;
	pTaskNode = kmalloc(sizeof(struct task_node), GFP_ATOMIC);
	if (pTaskNode) {
		pTaskNode->pTask = pTask;
		list_add(&pTaskNode->node, pPosition);
		nResult = 1;
	}
	return nResult;
}
static void list_reset(struct list_head *pList)
{
	struct task_node *pTaskIterator;
	struct task_node *pTaskNext;
	list_for_each_entry_safe(pTaskIterator, pTaskNext, pList, node) {
		list_del(&pTaskIterator->node);
		kfree(pTaskIterator);
	}
}

#define ASUS_MEMORY_DEBUG_MAXLEN    (128)
#define ASUS_MEMORY_DEBUG_MAXCOUNT  (256)
#define MINFREE_TO_PRINT_LOG 		(300)
#define HEAD_LINE "PID       RSS    oom_adj       cmdline\n"
char meminfo_str[ASUS_MEMORY_DEBUG_MAXCOUNT][ASUS_MEMORY_DEBUG_MAXLEN];
static unsigned long lowmem_scan_count;
static unsigned long lowmem_escape1_count;
static unsigned long lowmem_escape2_count;
static unsigned long lowmem_escape3_count;
static unsigned long lowmem_kill_count;
static unsigned long lowmem_scan(struct shrinker *s, struct shrink_control *sc)
{
	LIST_HEAD(ListHead);
	int nTaskNum = 0;
	struct task_node *pTaskIterator;
	struct task_node *pTaskNext;
	struct task_struct *tsk;
	struct task_struct *selected = NULL;
	unsigned long rem = 0;
	int tasksize;
	int i;
	int meminfo_str_index = 0;
	int ret = 0;
	short min_score_adj = OOM_SCORE_ADJ_MAX + 1;
	int minfree = 0;
	int selected_tasksize = 0;
	short selected_oom_score_adj;
	int array_size = ARRAY_SIZE(lowmem_adj);
	int other_free;
	int other_file;
	int nr_free_pages = 0;
	int nr_file_pages = 0;
	int nr_zcache_pages = 0;
	int nr_shmem = 0;
	int nr_swapcache_pages = 0;
	short previous_min_score_adj = OOM_SCORE_ADJ_MAX + 1;
	bool bIsAdapterLMK = false;
	int nTaskMax = DTaskMax;

	dumppid_tasksize = 0;
	dumppid = 0;
	
	if (mutex_lock_interruptible(&scan_mutex) < 0)
		return 0;
	
	lowmem_scan_count++;
	
	nr_free_pages = global_page_state(NR_FREE_PAGES);
	nr_file_pages = global_page_state(NR_FILE_PAGES);
	nr_zcache_pages = zcache_pages();
	nr_shmem = global_page_state(NR_SHMEM);
	nr_swapcache_pages = total_swapcache_pages();

	other_free = nr_free_pages;

	if (nr_shmem + nr_swapcache_pages < nr_file_pages + nr_zcache_pages)
		other_file = nr_file_pages + nr_zcache_pages - 	nr_shmem - nr_swapcache_pages;
	else
		other_file = 0;

	tune_lmk_param(&other_free, &other_file, sc);

	if (lowmem_adj_size < array_size)
		array_size = lowmem_adj_size;
	if (lowmem_minfree_size < array_size)
		array_size = lowmem_minfree_size;
	for (i = 0; i < array_size; i++) {
		minfree = lowmem_minfree[i];
		if (other_free < minfree && other_file < minfree) {
			min_score_adj = lowmem_adj[i];
			break;
		}
	}

	previous_min_score_adj = min_score_adj;
	ret = adjust_minadj(&min_score_adj);
	if (previous_min_score_adj != min_score_adj) {
		bIsAdapterLMK = true;
	}
	// if the kswapd comes, directly do the lowmemkiller. others, check if the justkilled/killnothing timeout to sleep or proceed to kill processes.
	// if PowerManagerSer calls, do not wait, and directly run the killer
	
	if((min_score_adj >= justkilled_adj) && time_before_eq(jiffies, lowmem_justkilled_timeout))
	{
		mutex_unlock(&scan_mutex);
		//if(strcmp(current->comm, "PowerManagerSer"))
		//	msleep(50);
		//printk("lowmem:lowmem_justkilled_timeout\n");
		lowmem_escape1_count++;
		return 0;
	}		

	if((min_score_adj >= killNothing_adj) && time_before_eq(jiffies, lowmem_killNothing_timeout))
	{
		mutex_unlock(&scan_mutex);
		//printk("lowmem:min_lowmem_killNothing_timeout\n");
		//if(strcmp(current->comm, "PowerManagerSer"))
		//	msleep(50);
		lowmem_escape2_count++;
		return 0;
		
	}
	//lowmem_print(5, "lowmem_scan %lu, %x, ofree %d %d, ma %hd\n", sc->nr_to_scan, sc->gfp_mask, other_free, other_file, min_score_adj);

	if (min_score_adj == OOM_SCORE_ADJ_MAX + 1) {
		trace_almk_shrink(0, ret, other_free, other_file, 0);
		lowmem_print(5, "lowmem_scan %lu, %x, return 0\n",
			     sc->nr_to_scan, sc->gfp_mask);
		mutex_unlock(&scan_mutex);
		lowmem_escape3_count++;
		return 0;
	}

	selected_oom_score_adj = min_score_adj;
	if(time_after(jiffies,print_log_time_out)){
			print_log_flag = true;
			print_log_time_out =  jiffies + HZ * 10; 
	}
	// if aLMK, kill one app at a time, others, check the min_score_adj, the lower the memory, the more the apps to kill
	if(bIsAdapterLMK)
		nTaskMax = 2;
	else if(min_score_adj >= 1000)
		nTaskMax = 1;
	else if(min_score_adj >= 529)
		nTaskMax = 2;		
	else if(min_score_adj >= 300)
		nTaskMax = 4;	
	else if(min_score_adj >= 117)
		nTaskMax = 5;
	else
		nTaskMax = 6;
	
	rcu_read_lock();
	for_each_process(tsk) {
		struct task_struct *p;
		short oom_score_adj;

		if (tsk->flags & PF_KTHREAD)
			continue;

		/* if task no longer has any memory ignore it */
		if (test_task_flag(tsk, TIF_MM_RELEASED))
			continue;

		
		if (test_task_flag(tsk, TIF_MEMDIE)) 
			continue;
		
		
		p = find_lock_task_mm(tsk);
		if (!p)
			continue;

		oom_score_adj = p->signal->oom_score_adj;

		//save the memory inforation if the oomadj is lower than MINFREE_TO_PRINT_LOG(300) for showing
		if(selected_oom_score_adj < MINFREE_TO_PRINT_LOG)
		{
			tasksize = get_mm_rss(p->mm);
			if(meminfo_str_index >= ASUS_MEMORY_DEBUG_MAXCOUNT )
				meminfo_str_index = ASUS_MEMORY_DEBUG_MAXCOUNT - 1;
			snprintf(meminfo_str[meminfo_str_index++], ASUS_MEMORY_DEBUG_MAXLEN, "%6d  %8ldkB %8d %s\n", p->pid, tasksize * (long)(PAGE_SIZE / 1024),oom_score_adj, p->comm);

		}
		if(dumppid_tasksize < get_mm_rss(p->mm)){
				dumppid_tasksize = get_mm_rss(p->mm);
				dumppid = p->pid;
		}
		//if the system is occupying too much memory, dumpsys and show the smaps. 
		if((get_mm_rss(p->mm) * (long)PAGE_SIZE / 1048576) > 600){
			
			if(strstr(p->comm,"system_server") !=NULL ){
				trigger_dump_mem = true;
				dumppid = p->pid;
			}

			if(print_log_flag)
				printk("lowmemorykiller: %6d  %8ldkB %8d %s\n",p->pid, get_mm_rss(p->mm) * (long)(PAGE_SIZE / 1024),oom_score_adj, p->comm);
		}
		
		//do not kill launcher, acore, gapps, media until the min_score is under 200
		if(strstr(p->comm,"launcher") != NULL  && min_score_adj > 200){
			task_unlock(p);
			//printk("lowmemorykiller: Don't kill launcher when min_socre_adj > 300\n");
			continue;
		}
		if(strstr(p->comm,"auncher3:commo") != NULL  && min_score_adj > 200){
			task_unlock(p);
			//printk("lowmemorykiller: Don't kill launcher when min_socre_adj > 300\n");
			continue;
		}
		if(strstr(p->comm,"process.acore") != NULL && min_score_adj > 200){
			task_unlock(p);
			//printk("lowmemorykiller: Don't kill acore when min_socre_adj > 300\n");
			continue;
		}
		if(strstr(p->comm,"process.gapps") != NULL && min_score_adj > 200){
			task_unlock(p);
			//printk("lowmemorykiller: Don't kill gapps when min_socre_adj > 300\n");
			continue;
		}
		if(strstr(p->comm,"process.media") != NULL && min_score_adj > 200){
			task_unlock(p);
			//printk("lowmemorykiller: Don't kill media when min_socre_adj > 300\n");
			continue;
		}
		if (oom_score_adj < min_score_adj) {
			task_unlock(p);
			continue;
		}
		tasksize = get_mm_rss(p->mm);
		task_unlock(p);
		if (tasksize <= 0)
			continue;
		
		
		//lowmem_print(3, "[Candidate] comm: %s, oom_score_adj: %d, tasksize: %d\n", p->comm, oom_score_adj, tasksize);
		if (nTaskNum == 0) {
			if (list_insert(p, &ListHead))
				nTaskNum++;
			else
				lowmem_print(1, "Unable to allocate memory (%d)\n", EListAllocate_Init);
		} else {
			/* Find the node index that fit for current task */
			int nError = 0;
			struct list_head *pInsertPos = NULL;
			struct task_node *pTaskSearchIterator;
			struct task_node *pTaskSearchNext;
			list_for_each_entry_safe(pTaskSearchIterator, pTaskSearchNext, &ListHead, node) {
				int nTargeteAdj = 0;
				int nTargetSize = 0;
				struct task_struct *pTask;
				pTask = pTaskSearchIterator->pTask;
				if (!pTask)
					continue;
				task_lock(pTask);
				if (pTask->signal)
					nTargeteAdj = pTask->signal->oom_score_adj;
				if (pTask->mm)
					nTargetSize = get_mm_rss(pTask->mm);
				task_unlock(pTask);
				if(!bIsAdapterLMK)
				{
					if (oom_score_adj > nTargeteAdj) {
					} else if (oom_score_adj < nTargeteAdj) {
						break;
					} else {
						if (tasksize > nTargetSize) {
						} else if (tasksize <= nTargetSize) {
							break;
						}
					}
				}
				else
				{
					//sort the process by tasksize only when in a_lmk.
					if (tasksize <= nTargetSize)
						break;
				}
			}

			/* Determine the insert position */
			if (&pTaskSearchIterator->node == &ListHead) {
				/* Add node to tail */
				pInsertPos = ListHead.prev;
				nError = EListAllocate_Tail;
			} else if (&pTaskSearchIterator->node == ListHead.next) {
				if (nTaskNum < nTaskMax) {
					/* Add node to head */
					pInsertPos = &ListHead;
					nError = EListAllocate_Head;
				}
			} else {
				/* Insert node to the list */
				pInsertPos = pTaskSearchIterator->node.prev;
				nError = EListAllocate_Body;
			}
			/* Perform insertion */
			if (pInsertPos) {
				if (list_insert(p, pInsertPos))
					nTaskNum++;
				else
					lowmem_print(1, "Unable to allocate memory (%d)\n", nError);
			}
			/* Delete node if the kept tasks exceed the limit */
			if (nTaskNum > nTaskMax) {
				struct task_node *pDeleteNode = list_entry((ListHead).next, struct task_node, node);
				list_del((ListHead).next);
				kfree(pDeleteNode);
				nTaskNum--;
			}
		}
	}
	killNothing = 1;

	list_for_each_entry_safe_reverse(pTaskIterator, pTaskNext, &ListHead, node) {
		char reason[256];
		long cache_size = other_file * (long)(PAGE_SIZE / 1024);
		long cache_limit = minfree * (long)(PAGE_SIZE / 1024);
		long free = other_free * (long)(PAGE_SIZE / 1024);
		selected = pTaskIterator->pTask;
		if (!selected)
			continue;
		task_lock(selected);
		if (test_tsk_thread_flag(selected, TIF_MEMDIE)) {
			task_unlock(selected);
			continue;
		}
		if (selected->signal)
			selected_oom_score_adj = selected->signal->oom_score_adj;
		if (selected_oom_score_adj < min_score_adj) {
			lowmem_print(1, "lowmem:Skip killing '%s' (%d), adj %hd is lower than min_score_adj %d now\n", selected->comm, selected->pid, selected_oom_score_adj, min_score_adj);
			task_unlock(selected);
			continue;
        }
        if(selected->pid == current->pid)
        {
			lowmem_print(1, "lowmem:Skip killing '%s' (adj=%d), pid=%d itself\n", selected->comm, selected_oom_score_adj, selected->pid);
			task_unlock(selected);
			continue;
		}
		if (selected->mm)
			selected_tasksize = get_mm_rss(selected->mm);
		task_unlock(selected);
		
		// when a_lmk is activated, do not kill until the app is larger than 80MB
		if (bIsAdapterLMK && (selected_tasksize * ((long)PAGE_SIZE /1024 )< (long)(80*1024)))
		{
			printk("lowmem: %s selected(%d MB)(adj=%d) < 80MB\n", selected->comm, (selected_tasksize * 4) / 1024 , selected_oom_score_adj);
			continue;
		}
			
		if (bIsAdapterLMK)
			snprintf(reason, sizeof(reason), "adaptive lmk is triggered and adjusts oom_score_adj to %hd, cache_size=%ldkB\n", min_score_adj, cache_size);
		else
			snprintf(reason, sizeof(reason), "cache_size=%ldkB (file+zcache-shmem-swapcache) is below limit %ldkB for oom_score_adj %hd\n", cache_size, cache_limit, min_score_adj);

		trace_lowmemory_kill(selected, cache_size, cache_limit, free);
		lowmem_print(1, "Killing '%s' (%d), adj %hd,\n" \
				"   to free %ldkB on behalf of '%s' (%d) because\n" \
				"   %s" \
				"   Free memory is %ldkB above reserved.\n" \
				"   Free CMA is %ldkB\n" \
				"   Total reserve is %ldkB\n" \
				"   Beginning free pages is %ldkB, now it's %ldkB\n" \
				"   Beginning file cache is %ldkB, now it's %ldkB\n" \
				"   Beginning zcache is %ldkB, now it's %ldkB\n" \
				"   Beginning shmem is %ldkB, swapcache pages is %ldkB\n" \
				"   GFP mask is 0x%x\n",
			     selected->comm, selected->pid,
			     selected_oom_score_adj,
			     selected_tasksize * (long)(PAGE_SIZE / 1024),
			     current->comm, current->pid,
			     reason,
			     other_free * (long)(PAGE_SIZE / 1024),
			     global_page_state(NR_FREE_CMA_PAGES) * (long)(PAGE_SIZE / 1024),
			     totalreserve_pages * (long)(PAGE_SIZE / 1024),
			     nr_free_pages * (long)(PAGE_SIZE / 1024), global_page_state(NR_FREE_PAGES) * (long)(PAGE_SIZE / 1024),
			     nr_file_pages * (long)(PAGE_SIZE / 1024), global_page_state(NR_FILE_PAGES) * (long)(PAGE_SIZE / 1024),
			     nr_zcache_pages * (long)(PAGE_SIZE / 1024), (long)zcache_pages() * (long)(PAGE_SIZE / 1024),
			     nr_shmem * (long)(PAGE_SIZE / 1024), nr_swapcache_pages * (long)(PAGE_SIZE / 1024),
			     sc->gfp_mask);

		if (selected_oom_score_adj < 100 && !trigger_dumpsys_meminfo) {
			trigger_dumpsys_meminfo = true;
		}
		// we are killing, so set the killnothing flag to 0
		killNothing = 0;
		killNothing_adj = 2000;
		set_tsk_thread_flag(selected, TIF_MEMDIE);
		send_sig(SIGKILL, selected, 0);
		rem += selected_tasksize;
		trace_almk_shrink(selected_tasksize, ret,
			other_free, other_file, selected_oom_score_adj);
		// set the justkilled flag to true, and remember the min_score_adj for reference
		lowmem_justkilled_timeout = jiffies + HZ/nTaskMax;
		justkilled_adj = min_score_adj;
		lowmem_kill_count++;
	}
	
	list_reset(&ListHead);
	//lowmem_print(4, "lowmem_scan %lu, %x, return %lu\n", sc->nr_to_scan, sc->gfp_mask, rem);
	rcu_read_unlock();
	mutex_unlock(&scan_mutex);
	print_log_flag = false;
	if(time_after(jiffies,dumpmem_time_out) && trigger_dump_mem){
			schedule_work(&__dumpmem_work);
			dumpmem_time_out = jiffies + HZ * 120; //1min
			trigger_dump_mem = false;
	}
	if(selected && (selected_oom_score_adj < MINFREE_TO_PRINT_LOG) && time_after(jiffies,time_out)){
			int count = 0;
			printk(HEAD_LINE);
			while (count < meminfo_str_index ){
				printk(meminfo_str[count]);
				count++;
			}
			time_out = jiffies + HZ * 10;
	}

	if(trigger_dumpsys_meminfo && time_after(jiffies, trigger_dumpsys_meminfo_time)) {
		trigger_dumpsys_meminfo = false;
		trigger_dumpsys_meminfo_time = jiffies + 60 * HZ;
		printk("[Vincent] start to schedule __keysavelog_work\n");
		schedule_work(&__dumpmem_work);
	}
	
	if (selected_oom_score_adj == 0) {
		show_mem(SHOW_MEM_FILTER_NODES);
		dump_tasks(NULL, NULL);
	}	

	if(killNothing)
	{
		killNothing_adj = min_score_adj;
		lowmem_killNothing_timeout = jiffies + 2*HZ;
		//printk("lowmem:killnothing min_score_adj=%d \n", killNothing_adj);
	}

	return rem;
}

static struct shrinker lowmem_shrinker = {
	.scan_objects = lowmem_scan,
	.count_objects = lowmem_count,
	.seeks = DEFAULT_SEEKS * 16
};

void dumpmem_func(struct work_struct *work)
{
	int ret = -1;
	char buffer[8];
	char cmdpath[] = "/system/bin/recvkernelevt";
	char *argv[8] = {cmdpath, "dumpmem",NULL};
	char *envp[] = {"HOME=/", "PATH=/sbin:/system/bin:/system/sbin:/vendor/bin", NULL};
	snprintf (buffer,7,"%d",dumppid);
	argv[2] = buffer;
	argv[3] = NULL;
	printk("[Debug+++] dumpsys meminfo on userspace\n");
	ret = call_usermodehelper(argv[0], argv, envp, UMH_WAIT_EXEC);
	printk("[Debug---] dumpsys meminfo on userspace, ret = %d\n", ret);

	return;
}

void dumpthread_func(struct work_struct *work)
{
	int ret = -1;
	char cmdpath[] = "/system/bin/recvkernelevt";
	char *argv[8] = {cmdpath, "dumpbusythread",NULL};
	char *envp[] = {"HOME=/", "PATH=/sbin:/system/bin:/system/sbin:/vendor/bin", NULL};
	printk("[Debug+++] dumpthread  on userspace\n");
	ret = call_usermodehelper(argv[0], argv, envp, UMH_WAIT_EXEC);
	printk("[Debug---] dumpthread  on userspace, ret = %d\n", ret);

	return;
}


#define LOAD_TRACKING_PERIOD 5000
void LoadTrackerExec(unsigned long data);
DEFINE_TIMER(LoadTrackerTimer, LoadTrackerExec, 0, 0);

void LoadTrackerExec(unsigned long data)
{
	static unsigned long last_track_time = 0;
	static unsigned escaped_msec;

	// Initial condition
	if (last_track_time == 0) {
		last_track_time = jiffies;
	} else {
		escaped_msec = jiffies_to_msecs(abs(jiffies - last_track_time));

		printk("[LMK] Elapsed: %u ms, Scan: %lu, Kill: %lu, escape1= %lu, escape2=%lu, escape3=%lu \n", escaped_msec, lowmem_scan_count, lowmem_kill_count, lowmem_escape1_count, lowmem_escape2_count, lowmem_escape3_count);

		lowmem_scan_count = 0;
		lowmem_kill_count = 0;
		lowmem_escape1_count = 0;
		lowmem_escape2_count = 0;
		lowmem_escape3_count = 0;
		last_track_time = jiffies;
	}
	mod_timer(&LoadTrackerTimer, jiffies + msecs_to_jiffies(LOAD_TRACKING_PERIOD));
}

static int __init lowmem_init(void)
{
	register_shrinker(&lowmem_shrinker);
	vmpressure_notifier_register(&lmk_vmpr_nb);

	INIT_WORK(&__dumpmem_work, dumpmem_func);
	INIT_WORK(&__dumpthread_work, dumpthread_func);

	// Track lmk loading
	mod_timer(&LoadTrackerTimer, jiffies + msecs_to_jiffies(LOAD_TRACKING_PERIOD));
	return 0;
}

static void __exit lowmem_exit(void)
{
	unregister_shrinker(&lowmem_shrinker);
}

#ifdef CONFIG_ANDROID_LOW_MEMORY_KILLER_AUTODETECT_OOM_ADJ_VALUES
static short lowmem_oom_adj_to_oom_score_adj(short oom_adj)
{
	if (oom_adj == OOM_ADJUST_MAX)
		return OOM_SCORE_ADJ_MAX;
	else
		return (oom_adj * OOM_SCORE_ADJ_MAX) / -OOM_DISABLE;
}

static void lowmem_autodetect_oom_adj_values(void)
{
	int i;
	short oom_adj;
	short oom_score_adj;
	int array_size = ARRAY_SIZE(lowmem_adj);

	if (lowmem_adj_size < array_size)
		array_size = lowmem_adj_size;

	if (array_size <= 0)
		return;

	oom_adj = lowmem_adj[array_size - 1];
	if (oom_adj > OOM_ADJUST_MAX)
		return;

	oom_score_adj = lowmem_oom_adj_to_oom_score_adj(oom_adj);
	if (oom_score_adj <= OOM_ADJUST_MAX)
		return;

	lowmem_print(1, "lowmem_shrink: convert oom_adj to oom_score_adj:\n");
	for (i = 0; i < array_size; i++) {
		oom_adj = lowmem_adj[i];
		oom_score_adj = lowmem_oom_adj_to_oom_score_adj(oom_adj);
		lowmem_adj[i] = oom_score_adj;
		lowmem_print(1, "oom_adj %d => oom_score_adj %d\n",
			     oom_adj, oom_score_adj);
	}
}

static int lowmem_adj_array_set(const char *val, const struct kernel_param *kp)
{
	int ret;

	ret = param_array_ops.set(val, kp);

	/* HACK: Autodetect oom_adj values in lowmem_adj array */
	lowmem_autodetect_oom_adj_values();

	return ret;
}

static int lowmem_adj_array_get(char *buffer, const struct kernel_param *kp)
{
	return param_array_ops.get(buffer, kp);
}

static void lowmem_adj_array_free(void *arg)
{
	param_array_ops.free(arg);
}

static struct kernel_param_ops lowmem_adj_array_ops = {
	.set = lowmem_adj_array_set,
	.get = lowmem_adj_array_get,
	.free = lowmem_adj_array_free,
};

static const struct kparam_array __param_arr_adj = {
	.max = ARRAY_SIZE(lowmem_adj),
	.num = &lowmem_adj_size,
	.ops = &param_ops_short,
	.elemsize = sizeof(lowmem_adj[0]),
	.elem = lowmem_adj,
};
#endif

module_param_named(cost, lowmem_shrinker.seeks, int, S_IRUGO | S_IWUSR);
#ifdef CONFIG_ANDROID_LOW_MEMORY_KILLER_AUTODETECT_OOM_ADJ_VALUES
module_param_cb(adj, &lowmem_adj_array_ops,
		.arr = &__param_arr_adj, S_IRUGO | S_IWUSR);
__MODULE_PARM_TYPE(adj, "array of short");
#else
module_param_array_named(adj, lowmem_adj, short, &lowmem_adj_size,
			 S_IRUGO | S_IWUSR);
#endif
module_param_array_named(minfree, lowmem_minfree, uint, &lowmem_minfree_size,
			 S_IRUGO | S_IWUSR);
module_param_named(debug_level, lowmem_debug_level, uint, S_IRUGO | S_IWUSR);
module_param_named(lmk_fast_run, lmk_fast_run, int, S_IRUGO | S_IWUSR);

module_init(lowmem_init);
module_exit(lowmem_exit);

MODULE_LICENSE("GPL");

