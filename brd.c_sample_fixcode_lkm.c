[drivers/block/brd.c [램으로 동작하는 블락 디바이스 드라이버]


커널 버전: linux-4.16.1 (kernel.org에서 내려받은 커스톰 커널 소스코드)
아래 램 블록 디바이스 드라이버는 insmod brd로 로드후 echo “testechotf8!” >> /dev/ram0[~]을 입력하면 cat /dev/ram0[~]으로 입력한 데이터를 램디스크로 저장하고 있다가 해당 데이터를 출력해준다
삭제되지 않고 유지된다
블락 디바이스 드라이버를 구현하는 방법에 대해서 알 필요가 있으면 아래 소스코드도 좋은 가르침을 준다
이제 분석하고 코드를 조작해서 원하는대로 동작하게 만들어보는 연습과 코드익히기 외우기가 실현되어야 한다고 판단되는 시점이다 2024-7-13(토) 1:47분
분석100.
%
끝났다.
읽기/쓰기에 디버깅을넣었는데,쓰기읽기순으로클라이언트를작성했는데읽기쓰기순으로만로깅이되지만
어쨋든한줄한줄해서두줄로깅되는거니까정상적이다라고할순있겠다.
해커!

/*
 * Ram backed block device driver.
 *
 * Copyright (C) 2007 Nick Piggin
 * Copyright (C) 2007 Novell Inc.
 *
 * Parts derived from drivers/block/rd.c, and drivers/block/loop.c, copyright
 * of their respective owners.
 */

#include <linux/init.h>
#include <linux/initrd.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/major.h>
#include <linux/blkdev.h> // linux/blkdev.h 블락장치 지원
#include <linux/bio.h>
#include <linux/highmem.h>
#include <linux/mutex.h>
#include <linux/radix-tree.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/backing-dev.h>

#include <linux/uaccess.h>

#define SECTOR_SHIFT		9
#define PAGE_SECTORS_SHIFT	(PAGE_SHIFT - SECTOR_SHIFT)
#define PAGE_SECTORS		(1 << PAGE_SECTORS_SHIFT)

/*
 * Each block ramdisk device has a radix_tree brd_pages of pages that stores
 * the pages containing the block device's contents. A brd page's ->index is
 * its offset in PAGE_SIZE units. This is similar to, but in no way connected
 * with, the kernel's pagecache or buffer cache (which sit above our block
 * device).
 */
struct brd_device {
	int		brd_number;

	struct request_queue	*brd_queue;
	struct gendisk		*brd_disk; // gendisk 객체 구조체로 brd_disk 포인터를 선언해서 사용한다.
	struct list_head	brd_list;

	/*
	 * Backing store of pages and lock to protect it. This is the contents
	 * of the block device.
	 */
	spinlock_t		brd_lock;
	struct radix_tree_root	brd_pages; // radix_tree_root 객체로 brd_pages[페이지 구조체]를 선언해서 사용한다…
};

/*
 * Look up and return a brd's page for a given sector.
 */
static struct page *brd_lookup_page(struct brd_device *brd, sector_t sector)
{
	pgoff_t idx;
	struct page *page;

	/*
	 * The page lifetime is protected by the fact that we have opened the
	 * device node -- brd pages will never be deleted under us, so we
	 * don't need any further locking or refcounting.
	 *
	 * This is strictly true for the radix-tree nodes as well (ie. we
	 * don't actually need the rcu_read_lock()), however that is not a
	 * documented feature of the radix-tree API so it is better to be
	 * safe here (we don't have total exclusion from radix tree updates
	 * here, only deletes).
	 */
	rcu_read_lock();
	idx = sector >> PAGE_SECTORS_SHIFT; /* sector to page index */
	page = radix_tree_lookup(&brd->brd_pages, idx);
	rcu_read_unlock();

	BUG_ON(page && page->index != idx);

	return page;
}

/*
 * Look up and return a brd's page for a given sector.
 * If one does not exist, allocate an empty page, and insert that. Then
 * return it.
 */
static struct page *brd_insert_page(struct brd_device *brd, sector_t sector)
{
	pgoff_t idx;
	struct page *page;
	gfp_t gfp_flags;

	page = brd_lookup_page(brd, sector);
	if (page)
		return page;

	/*
	 * Must use NOIO because we don't want to recurse back into the
	 * block or filesystem layers from page reclaim.
	 *
	 * Cannot support DAX and highmem, because our ->direct_access
	 * routine for DAX must return memory that is always addressable.
	 * If DAX was reworked to use pfns and mmap throughout, this
	 * restriction might be able to be lifted.
	 */
	gfp_flags = GFP_NOIO | __GFP_ZERO;
	page = alloc_page(gfp_flags);
	if (!page)
		return NULL;

	if (radix_tree_preload(GFP_NOIO)) {
		__free_page(page);
		return NULL;
	}

	spin_lock(&brd->brd_lock);
	idx = sector >> PAGE_SECTORS_SHIFT;
	page->index = idx;
	if (radix_tree_insert(&brd->brd_pages, idx, page)) { // brd->brd_pages[brd.ko의 brd_pages[가상메모리 페이지]에 할당한 page를 idx[인덱스]에 맞게 새로 삽입
		__free_page(page);
		page = radix_tree_lookup(&brd->brd_pages, idx);
		BUG_ON(!page);
		BUG_ON(page->index != idx);
	}
	spin_unlock(&brd->brd_lock);

	radix_tree_preload_end();

	return page;
}

/*
 * Free all backing store pages and radix tree. This must only be called when
 * there are no other users of the device.
 */
#define FREE_BATCH 16
static void brd_free_pages(struct brd_device *brd)
{
	unsigned long pos = 0;
	struct page *pages[FREE_BATCH];
	int nr_pages;

	do {
		int i;

		nr_pages = radix_tree_gang_lookup(&brd->brd_pages,
				(void **)pages, pos, FREE_BATCH);

		for (i = 0; i < nr_pages; i++) {
			void *ret;

			BUG_ON(pages[i]->index < pos);
			pos = pages[i]->index;
			ret = radix_tree_delete(&brd->brd_pages, pos);
			BUG_ON(!ret || ret != pages[i]);
			__free_page(pages[i]);
		}

		pos++;

		/*
		 * This assumes radix_tree_gang_lookup always returns as
		 * many pages as possible. If the radix-tree code changes,
		 * so will this have to.
		 */
	} while (nr_pages == FREE_BATCH);
}

/*
 * copy_to_brd_setup must be called before copy_to_brd. It may sleep.
 */
static int copy_to_brd_setup(struct brd_device *brd, sector_t sector, size_t n)
{
	unsigned int offset = (sector & (PAGE_SECTORS-1)) << SECTOR_SHIFT;
	size_t copy;

	copy = min_t(size_t, n, PAGE_SIZE - offset);
	if (!brd_insert_page(brd, sector))
		return -ENOSPC;
	if (copy < n) {
		sector += copy >> SECTOR_SHIFT;
		if (!brd_insert_page(brd, sector))
			return -ENOSPC;
	}
	return 0;
}

/*
 * Copy n bytes from src to the brd starting at sector. Does not sleep.
 */
// 블락 디바이스에 데이터[블록단위]로 쓰는 함수
static void copy_to_brd(struct brd_device *brd, const void *src,
			sector_t sector, size_t n)
{
	struct page *page;
	void *dst;
	unsigned int offset = (sector & (PAGE_SECTORS-1)) << SECTOR_SHIFT;
	size_t copy;

	copy = min_t(size_t, n, PAGE_SIZE - offset);
	page = brd_lookup_page(brd, sector);
	BUG_ON(!page);

	dst = kmap_atomic(page);
	memcpy(dst + offset, src, copy);
	kunmap_atomic(dst);

	if (copy < n) {
		src += copy;
		sector += copy >> SECTOR_SHIFT;
		copy = n - copy;
		page = brd_lookup_page(brd, sector);
		BUG_ON(!page);

		dst = kmap_atomic(page);
		memcpy(dst, src, copy);
		kunmap_atomic(dst);
	}
}

/*
 * Copy n bytes to dst from the brd starting at sector. Does not sleep.
 */
// 블락장치에서 쓰여있는 데이터[블록]을 복사해서 유저스페이스에 전달해주는 함수
static void copy_from_brd(void *dst, struct brd_device *brd,
			sector_t sector, size_t n)
{
	struct page *page;
	void *src;
	unsigned int offset = (sector & (PAGE_SECTORS-1)) << SECTOR_SHIFT;
	size_t copy;

	copy = min_t(size_t, n, PAGE_SIZE - offset);
	page = brd_lookup_page(brd, sector);
	if (page) {
		src = kmap_atomic(page);
		memcpy(dst, src + offset, copy);
		kunmap_atomic(src);
	} else
		memset(dst, 0, copy);

	if (copy < n) {
		dst += copy;
		sector += copy >> SECTOR_SHIFT;
		copy = n - copy;
		page = brd_lookup_page(brd, sector);
		if (page) {
			src = kmap_atomic(page);
			memcpy(dst, src, copy);
			kunmap_atomic(src);
		} else
			memset(dst, 0, copy);
	}
}

/*
 * Process a single bvec of a bio.
 */
// bio하나의 단일 블락 벡터[bvec]를 읽기/쓰기하는 데이터 I/o 함수
static int brd_do_bvec(struct brd_device *brd, struct page *page,
			unsigned int len, unsigned int off, bool is_write,
			sector_t sector)
{
	void *mem;
	int err = 0;

	if (is_write) {
		err = copy_to_brd_setup(brd, sector, len);
		if (err)
			goto out;
	}

	mem = kmap_atomic(page); // 가상 메모리[페이지]에 메모리를 할당해 블락데이터를 읽기/쓰기 수행하는 코드영역 —
	if (!is_write) {
		copy_from_brd(mem + off, brd, sector, len);
		flush_dcache_page(page);
	} else {
		flush_dcache_page(page);
		copy_to_brd(brd, mem + off, sector, len);
	}
        // —
	kunmap_atomic(mem);

out:
	return err;
}

static blk_qc_t brd_make_request(struct request_queue *q, struct bio *bio)
{
	struct brd_device *brd = bio->bi_disk->private_data;
	struct bio_vec bvec;
	sector_t sector;
	struct bvec_iter iter;

	sector = bio->bi_iter.bi_sector;
	if (bio_end_sector(bio) > get_capacity(bio->bi_disk))
		goto io_error;

	bio_for_each_segment(bvec, bio, iter) {
		unsigned int len = bvec.bv_len;
		int err;

		err = brd_do_bvec(brd, bvec.bv_page, len, bvec.bv_offset,
					op_is_write(bio_op(bio)), sector);
		if (err)
			goto io_error;
		sector += len >> SECTOR_SHIFT;
	}

	bio_endio(bio);
	return BLK_QC_T_NONE;
io_error:
	bio_io_error(bio);
	return BLK_QC_T_NONE;
}

// 블락 디바이스에서 가상메모리 페이지에 읽기및쓰기[rw]가 발생할시 호출되는 커널 드라이버 함수
static int brd_rw_page(struct block_device *bdev, sector_t sector,
		       struct page *page, bool is_write)
{
	struct brd_device *brd = bdev->bd_disk->private_data;
	int err;

	if (PageTransHuge(page))
		return -ENOTSUPP;
	err = brd_do_bvec(brd, page, PAGE_SIZE, 0, is_write, sector);  // brd_do_bvec()으로 데이터[섹터] 읽기/쓰기 수행
	page_endio(page, is_write, err);
	return err;
}

// block_device_operations 구조를 쓰고, brd_fops를 선언하여 .rw_page를 brd_rw_page구현커널드라이버 함수가 호출되도로 선언한다
static const struct block_device_operations brd_fops = {
	.owner =		THIS_MODULE,
	.rw_page =		brd_rw_page,
};

/*
 * And now the modules code and kernel interface.
 */
static int rd_nr = CONFIG_BLK_DEV_RAM_COUNT;
module_param(rd_nr, int, S_IRUGO);
MODULE_PARM_DESC(rd_nr, "Maximum number of brd devices");

unsigned long rd_size = CONFIG_BLK_DEV_RAM_SIZE;
module_param(rd_size, ulong, S_IRUGO);
MODULE_PARM_DESC(rd_size, "Size of each RAM disk in kbytes.");

static int max_part = 1;
module_param(max_part, int, S_IRUGO);
MODULE_PARM_DESC(max_part, "Num Minors to reserve between devices");

MODULE_LICENSE("GPL");
MODULE_ALIAS_BLOCKDEV_MAJOR(RAMDISK_MAJOR);
MODULE_ALIAS("rd");

#ifndef MODULE
/* Legacy boot options - nonmodular */
static int __init ramdisk_size(char *str)
{
	rd_size = simple_strtol(str, NULL, 0);
	return 1;
}
__setup("ramdisk_size=", ramdisk_size);
#endif

/*
 * The device scheme is derived from loop.c. Keep them in synch where possible
 * (should share code eventually).
 */
static LIST_HEAD(brd_devices);
static DEFINE_MUTEX(brd_devices_mutex);

static struct brd_device *brd_alloc(int i)
{
	struct brd_device *brd;
	struct gendisk *disk;

	brd = kzalloc(sizeof(*brd), GFP_KERNEL);
	if (!brd)
		goto out;
	brd->brd_number		= i;
	spin_lock_init(&brd->brd_lock);
	INIT_RADIX_TREE(&brd->brd_pages, GFP_ATOMIC);

	brd->brd_queue = blk_alloc_queue(GFP_KERNEL); // 큐방식으로 동작하기위해 큐할당
	if (!brd->brd_queue)
		goto out_free_dev;

	blk_queue_make_request(brd->brd_queue, brd_make_request);
	blk_queue_max_hw_sectors(brd->brd_queue, 1024);

	/* This is so fdisk will align partitions on 4k, because of
	 * direct_access API needing 4k alignment, returning a PFN
	 * (This is only a problem on very small devices <= 4M,
	 *  otherwise fdisk will align on 1M. Regardless this call
	 *  is harmless)
	 */
	blk_queue_physical_block_size(brd->brd_queue, PAGE_SIZE);
	disk = brd->brd_disk = alloc_disk(max_part); // alloc_disk로 블락 디바이스 할당
	if (!disk)
		goto out_free_queue;

        // disk[할당한 블락 디바이스]를 설치
	disk->major		= RAMDISK_MAJOR;
	disk->first_minor	= i * max_part;
	disk->fops		= &brd_fops;
	disk->private_data	= brd;
	disk->queue		= brd->brd_queue;
	disk->flags		= GENHD_FL_EXT_DEVT;
	sprintf(disk->disk_name, "ram%d", i); // disk->disk_name에 장치명을 숫자를 더해 디바이스 명 할당과 선언에 사용되도록 함
	set_capacity(disk, rd_size * 2);
	disk->queue->backing_dev_info->capabilities |= BDI_CAP_SYNCHRONOUS_IO;

	return brd;

out_free_queue:
	blk_cleanup_queue(brd->brd_queue);
out_free_dev:
	kfree(brd);
out:
	return NULL;
}

static void brd_free(struct brd_device *brd)
{
	put_disk(brd->brd_disk);
	blk_cleanup_queue(brd->brd_queue);
	brd_free_pages(brd);
	kfree(brd);
}

// brd.ko블락장치 초기화 함수
static struct brd_device *brd_init_one(int i, bool *new)
{
	struct brd_device *brd;

	*new = false;
	list_for_each_entry(brd, &brd_devices, brd_list) {
		if (brd->brd_number == i)
			goto out;
	}

	brd = brd_alloc(i); // brd_alloc()으로 brd_device 모듈해지
	if (brd) {
		add_disk(brd->brd_disk); // add_disk로 해당 블락장치 삽입
		list_add_tail(&brd->brd_list, &brd_devices);
	}
	*new = true;
out:
	return brd;
}

static void brd_del_one(struct brd_device *brd)
{
	list_del(&brd->brd_list);
	del_gendisk(brd->brd_disk);
	brd_free(brd);
}

// brd.ko 장치 프로브 함수[시작함수]
static struct kobject *brd_probe(dev_t dev, int *part, void *data)
{
	struct brd_device *brd;
	struct kobject *kobj;
	bool new;

	mutex_lock(&brd_devices_mutex);
	brd = brd_init_one(MINOR(dev) / max_part, &new); // brd_init_one()초기화 함수 호출
	kobj = brd ? get_disk_and_module(brd->brd_disk) : NULL;
	mutex_unlock(&brd_devices_mutex);

	if (new)
		*part = 0;

	return kobj;
}

// 엔트리 포인트
static int __init brd_init(void)
{
	struct brd_device *brd, *next;
	int i;

	/*
	 * brd module now has a feature to instantiate underlying device
	 * structure on-demand, provided that there is an access dev node.
	 *
	 * (1) if rd_nr is specified, create that many upfront. else
	 *     it defaults to CONFIG_BLK_DEV_RAM_COUNT
	 * (2) User can further extend brd devices by create dev node themselves
	 *     and have kernel automatically instantiate actual device
	 *     on-demand. Example:
	 *		mknod /path/devnod_name b 1 X	# 1 is the rd major
	 *		fdisk -l /path/devnod_name
	 *	If (X / max_part) was not already created it will be created
	 *	dynamically.
	 */

        // register_blkdev로 “Ramdisk”라는 클래스로 블락디바이스 등록
	if (register_blkdev(RAMDISK_MAJOR, "ramdisk"))
		return -EIO;

	if (unlikely(!max_part))
		max_part = 1;

	for (i = 0; i < rd_nr; i++) {
		brd = brd_alloc(i); // rd_nr 모듈 파라메타만큼 brd_alloc()함수를 호출해서 장치드라이버 디바이스 선할당.

		if (!brd)
			goto out_free;
		list_add_tail(&brd->brd_list, &brd_devices);
	}

	/* point of no return */

	list_for_each_entry(brd, &brd_devices, brd_list)
		add_disk(brd->brd_disk);

        // blk_register_region()으로 블락장치를 처리할 brd_probe[프로빙시작기술함수] 모듈전용 함수 호출
	blk_register_region(MKDEV(RAMDISK_MAJOR, 0), 1UL << MINORBITS,
				  THIS_MODULE, brd_probe, NULL, NULL);

	pr_info("brd: module loaded\n");
	return 0; // 모듈 로딩 성공으로 0을 반환

out_free:
	list_for_each_entry_safe(brd, next, &brd_devices, brd_list) {
		list_del(&brd->brd_list);
		brd_free(brd);
	}
	unregister_blkdev(RAMDISK_MAJOR, "ramdisk");

	pr_info("brd: module NOT loaded !!!\n");
	return -ENOMEM;
}

static void __exit brd_exit(void)
{
	struct brd_device *brd, *next;

	list_for_each_entry_safe(brd, next, &brd_devices, brd_list)
		brd_del_one(brd);

        // blk_unregister_region()으로 블락장치 영역 등록해제/unregister_blkdev()로 블락 디바이스 등록해제
	blk_unregister_region(MKDEV(RAMDISK_MAJOR, 0), 1UL << MINORBITS);
	unregister_blkdev(RAMDISK_MAJOR, "ramdisk");

	pr_info("brd: module unloaded\n");
}

module_init(brd_init);
module_exit(brd_exit);
