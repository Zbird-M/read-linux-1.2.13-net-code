/*
 *  linux/fs/minix/inode.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

#ifdef MODULE
#include <linux/module.h>
#include <linux/version.h>
#else
#define MOD_INC_USE_COUNT
#define MOD_DEC_USE_COUNT
#endif

#include <linux/sched.h>
#include <linux/minix_fs.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/stat.h>
#include <linux/locks.h>

#include <asm/system.h>
#include <asm/segment.h>
#include <asm/bitops.h>

void minix_put_inode(struct inode *inode)
{
	if (inode->i_nlink)
		return;
	// 文件大小变成0
	inode->i_size = 0;
	// 删除inode的内容
	minix_truncate(inode);
	// 释放inode节点，插入空闲队列
	minix_free_inode(inode);
}

// 回写超级块内容
static void minix_commit_super (struct super_block * sb,
			       struct minix_super_block * ms)
{
	// sb->u.minix_sb.s_sbh是管理该超级块的buffer 
	mark_buffer_dirty(sb->u.minix_sb.s_sbh, 1);
	sb->s_dirt = 0;
}
// 回写超级块信息到硬盘
void minix_write_super (struct super_block * sb)
{
	struct minix_super_block * ms;
	// 没有设置只读标记
	if (!(sb->s_flags & MS_RDONLY)) {
		ms = sb->u.minix_sb.s_ms;

		if (ms->s_state & MINIX_VALID_FS)
			ms->s_state &= ~MINIX_VALID_FS;
		minix_commit_super (sb, ms);
	}
	sb->s_dirt = 0;
}

// 回写超级块的内容 
void minix_put_super(struct super_block *sb)
{
	int i;
	// 加锁
	lock_super(sb);
	// 
	if (!(sb->s_flags & MS_RDONLY)) {
		sb->u.minix_sb.s_ms->s_state = sb->u.minix_sb.s_mount_state;
		mark_buffer_dirty(sb->u.minix_sb.s_sbh, 1);
	}
	// 置为0说明没有不在使用
	sb->s_dev = 0;
	// 回写inode位图信息
	for(i = 0 ; i < MINIX_I_MAP_SLOTS ; i++)
		brelse(sb->u.minix_sb.s_imap[i]);
	// 回写块位图信息
	for(i = 0 ; i < MINIX_Z_MAP_SLOTS ; i++)
		brelse(sb->u.minix_sb.s_zmap[i]);
	// 回写超级块其他字段
	brelse (sb->u.minix_sb.s_sbh);
	// 解锁
	unlock_super(sb);
	MOD_DEC_USE_COUNT;
	return;
}

static struct super_operations minix_sops = { 
	minix_read_inode,
	NULL,
	minix_write_inode,
	minix_put_inode,
	minix_put_super,
	minix_write_super,
	minix_statfs,
	minix_remount
};
// 重新挂载文件系统，其实只能改变flag，而不是卸载再挂载
int minix_remount (struct super_block * sb, int * flags, char * data)
{
	struct minix_super_block * ms;
	// s_ms保存的minix文件系统超级块的信息
	ms = sb->u.minix_sb.s_ms;
	if ((*flags & MS_RDONLY) == (sb->s_flags & MS_RDONLY))
		return 0;
	if (*flags & MS_RDONLY) {
		if (ms->s_state & MINIX_VALID_FS ||
		    !(sb->u.minix_sb.s_mount_state & MINIX_VALID_FS))
			return 0;
		/* Mounting a rw partition read-only. */
		ms->s_state = sb->u.minix_sb.s_mount_state;
		mark_buffer_dirty(sb->u.minix_sb.s_sbh, 1);
		sb->s_dirt = 1;
		minix_commit_super (sb, ms);
	}
	else {
	  	/* Mount a partition which is read-only, read-write. */
		sb->u.minix_sb.s_mount_state = ms->s_state;
		ms->s_state &= ~MINIX_VALID_FS;
		mark_buffer_dirty(sb->u.minix_sb.s_sbh, 1);
		sb->s_dirt = 1;

		if (!(sb->u.minix_sb.s_mount_state & MINIX_VALID_FS))
			printk ("MINIX-fs warning: remounting unchecked fs, "
				"running fsck is recommended.\n");
		else if ((sb->u.minix_sb.s_mount_state & MINIX_ERROR_FS))
			printk ("MINIX-fs warning: remounting fs with errors, "
				"running fsck is recommended.\n");
	}
	return 0;
}

// 读超级块
struct super_block *minix_read_super(struct super_block *s,void *data, 
				     int silent)
{
	struct buffer_head *bh;
	struct minix_super_block *ms;
	int i,dev=s->s_dev,block;

	if (32 != sizeof (struct minix_inode))
		panic("bad i-node size");
	MOD_INC_USE_COUNT;
	lock_super(s);
	set_blocksize(dev, BLOCK_SIZE);
	// 读入超级块
	if (!(bh = bread(dev,1,BLOCK_SIZE))) {
		// 重置为未使用标记
		s->s_dev=0;
		unlock_super(s);
		printk("MINIX-fs: unable to read superblock\n");
		MOD_DEC_USE_COUNT;
		return NULL;
	}
	// 转成minix类型的超级块类型
	ms = (struct minix_super_block *) bh->b_data;
	// s是vfs的通用结构体，minix_sb是存储超级块信息的结构，s_ms是保存minix类型超级块的结构
	s->u.minix_sb.s_ms = ms;
	// bh是管理该超级块的buffer
	s->u.minix_sb.s_sbh = bh;
	// 
	s->u.minix_sb.s_mount_state = ms->s_state;
	// 块大小
	s->s_blocksize = 1024;
	// 右移10位，用于算出第几块
	s->s_blocksize_bits = 10;
	// inode的个数
	s->u.minix_sb.s_ninodes = ms->s_ninodes;
	// 文件系统的总块数，包括数据块，保存元数据的块
	s->u.minix_sb.s_nzones = ms->s_nzones;
	// inode位图块数
	s->u.minix_sb.s_imap_blocks = ms->s_imap_blocks;
	// 数据块位图块数
	s->u.minix_sb.s_zmap_blocks = ms->s_zmap_blocks;
	// 第一个数据块在硬盘的块号
	s->u.minix_sb.s_firstdatazone = ms->s_firstdatazone;
	// 右移s_log_zone_size位得到每块的大小，区别于用于存储数据的块大小 
	s->u.minix_sb.s_log_zone_size = ms->s_log_zone_size;
	// 单文件最大字节数 
	s->u.minix_sb.s_max_size = ms->s_max_size;
	// 文件系统魔数
	s->s_magic = ms->s_magic;
	// minix系统的版本
	if (s->s_magic == MINIX_SUPER_MAGIC) {
		// 目录项结构体minix_dir_entry的大小，两个字节存inode号，14个存文件名
		s->u.minix_sb.s_dirsize = 16;
		// 文件名长度
		s->u.minix_sb.s_namelen = 14;
	} else if (s->s_magic == MINIX_SUPER_MAGIC2) {
		s->u.minix_sb.s_dirsize = 32;
		s->u.minix_sb.s_namelen = 30;
	} else {
		// 魔数不对
		s->s_dev = 0;
		unlock_super(s);
		brelse(bh);
		if (!silent)
			printk("VFS: Can't find a minix filesystem on dev 0x%04x.\n", dev);
		MOD_DEC_USE_COUNT;
		return NULL;
	}
	// 初始化
	for (i=0;i < MINIX_I_MAP_SLOTS;i++)
		s->u.minix_sb.s_imap[i] = NULL;
	for (i=0;i < MINIX_Z_MAP_SLOTS;i++)
		s->u.minix_sb.s_zmap[i] = NULL;
	block=2;
	// 从第二块开始读，第一块保存超级块信息了，第二块起的n块保存块位图、inode位图
	for (i=0 ; i < s->u.minix_sb.s_imap_blocks ; i++)
		if ((s->u.minix_sb.s_imap[i]=bread(dev,block,BLOCK_SIZE)) != NULL)
			block++;
		else
			break;
	for (i=0 ; i < s->u.minix_sb.s_zmap_blocks ; i++)
		if ((s->u.minix_sb.s_zmap[i]=bread(dev,block,BLOCK_SIZE)) != NULL)
			block++;
		else
			break;
	// 没有全部读成功，报错
	if (block != 2+s->u.minix_sb.s_imap_blocks+s->u.minix_sb.s_zmap_blocks) {
		for(i=0;i<MINIX_I_MAP_SLOTS;i++)
			brelse(s->u.minix_sb.s_imap[i]);
		for(i=0;i<MINIX_Z_MAP_SLOTS;i++)
			brelse(s->u.minix_sb.s_zmap[i]);
		s->s_dev=0;
		unlock_super(s);
		brelse(bh);
		printk("MINIX-fs: bad superblock or unable to read bitmaps\n");
		MOD_DEC_USE_COUNT;
		return NULL;
	}
	// 操作系统查找空闲项的时候，成功则返回第几项，失败则返回0，所以第0项不使用。否则返回0无法判断语义
	set_bit(0,s->u.minix_sb.s_imap[0]->b_data);
	set_bit(0,s->u.minix_sb.s_zmap[0]->b_data);
	unlock_super(s);
	/* set up enough so that it can read an inode */
	s->s_dev = dev;
	// 操作超级块的函数集
	s->s_op = &minix_sops;
	// 获取第MINIX_ROOT_INO即第1个inode（第0个不用），第一个inode是根inode，即文件系统的起点
	s->s_mounted = iget(s,MINIX_ROOT_INO);
	// 获取失败则报错
	if (!s->s_mounted) {
		s->s_dev = 0;
		brelse(bh);
		printk("MINIX-fs: get root inode failed\n");
		MOD_DEC_USE_COUNT;
		return NULL;
	}
	if (!(s->s_flags & MS_RDONLY)) {
		ms->s_state &= ~MINIX_VALID_FS;
		mark_buffer_dirty(bh, 1);
		s->s_dirt = 1;
	}
	if (!(s->u.minix_sb.s_mount_state & MINIX_VALID_FS))
		printk ("MINIX-fs: mounting unchecked file system, "
			"running fsck is recommended.\n");
 	else if (s->u.minix_sb.s_mount_state & MINIX_ERROR_FS)
		printk ("MINIX-fs: mounting file system with errors, "
			"running fsck is recommended.\n");
	return s;
}
// 显示minix文件系统的参数，块数、可用数、文件名长度等
void minix_statfs(struct super_block *sb, struct statfs *buf)
{
	long tmp;

	put_fs_long(MINIX_SUPER_MAGIC, &buf->f_type);
	put_fs_long(1024, &buf->f_bsize);
	tmp = sb->u.minix_sb.s_nzones - sb->u.minix_sb.s_firstdatazone;
	tmp <<= sb->u.minix_sb.s_log_zone_size;
	put_fs_long(tmp, &buf->f_blocks);
	tmp = minix_count_free_blocks(sb);
	put_fs_long(tmp, &buf->f_bfree);
	put_fs_long(tmp, &buf->f_bavail);
	put_fs_long(sb->u.minix_sb.s_ninodes, &buf->f_files);
	put_fs_long(minix_count_free_inodes(sb), &buf->f_ffree);
	put_fs_long(sb->u.minix_sb.s_namelen, &buf->f_namelen);
	/* Don't know what value to put in buf->f_fsid */
}
// i_data里保存的是文件内容的块号和硬盘块号的对应关系
#define inode_bmap(inode,nr) ((inode)->u.minix_i.i_data[(nr)])
// bh->b_data保存的是文件内容块号和硬盘块号的对应关系
static int block_bmap(struct buffer_head * bh, int nr)
{
	int tmp;

	if (!bh)
		return 0;
	tmp = ((unsigned short *) bh->b_data)[nr];
	brelse(bh);
	return tmp;
}
// 获取文件内容的逻辑块号在硬盘中对应的实际块号
int minix_bmap(struct inode * inode,int block)
{
	int i;

	if (block<0) {
		printk("minix_bmap: block<0");
		return 0;
	}
	if (block >= 7+512+512*512) {
		printk("minix_bmap: block>big");
		return 0;
	}
	// 小于7的块号直接存在inode里
	if (block < 7)
		return inode_bmap(inode,block);
	block -= 7;
	// inode的i_data的第八个元素保存了一个块号，该块里的内容保存了512个块号，即第8块开始的文件内容
	if (block < 512) {
		// 先拿到一级块号
		i = inode_bmap(inode,7);
		if (!i)
			return 0;
		// 把一级块号对应的数据从硬盘读取进来，然后再根据偏移找到对应的二级块号，即文件内容对应的块号
		return block_bmap(bread(inode->i_dev,i,BLOCK_SIZE),block);
	}
	block -= 512;
	/*
		inode的i_data的第九个元素保存了一个块号，该块包括了512块号，
		512块号每个块号对应的块又保存了512个块号，最后的512块号保存了文件内容的块号
	*/
	i = inode_bmap(inode,8);
	if (!i)
		return 0;
	// >>9即除以512算出在哪个二级块
	i = block_bmap(bread(inode->i_dev,i,BLOCK_SIZE),block>>9);
	if (!i)
		return 0;
	// &511算出偏移
	return block_bmap(bread(inode->i_dev,i,BLOCK_SIZE),block & 511);
}
// 读取buffer中某个硬盘数据块对应的内容，create=1，说明没有对应数据块则创建一个
static struct buffer_head * inode_getblk(struct inode * inode, int nr, int create)
{
	int tmp;
	unsigned short *p;
	struct buffer_head * result;

	p = inode->u.minix_i.i_data + nr;
repeat:
	// 硬盘块号
	tmp = *p;
	if (tmp) {
		// 判断是不是在buffer里。
		result = getblk(inode->i_dev, tmp, BLOCK_SIZE);
		// 判断p指向块号是不是变了，是的话说明获取的数据result不是对的
		if (tmp == *p)
			return result;
		brelse(result);
		goto repeat;
	}
	// create是0说明不需要新建，等于1即找不到的时候新建一个
	if (!create)
		return NULL;
	// 从硬盘中创建一个新的块
	tmp = minix_new_block(inode->i_sb);
	if (!tmp)
		return NULL;
	// 
	result = getblk(inode->i_dev, tmp, BLOCK_SIZE);
	// p非空说明该项被使用了，则释放，可能因为中断引起的
	if (*p) {
		minix_free_block(inode->i_sb,tmp);
		brelse(result);
		goto repeat;
	}
	// 保存映射关系
	*p = tmp;
	// 创建时间
	inode->i_ctime = CURRENT_TIME;
	// inode新增了一个映射关系，需要回写到硬盘 
	inode->i_dirt = 1;
	return result;
}

// 读取buffer中硬盘某个块的对应的数据，或者新建一个块，bh->b_data保存了512个文件块号到硬盘块号。逻辑类似上面的函数
static struct buffer_head * block_getblk(struct inode * inode, 
	struct buffer_head * bh, int nr, int create)
{
	int tmp;
	unsigned short *p;
	struct buffer_head * result;

	if (!bh)
		return NULL;
	// 数据不是最新的，则先刷新缓存的数据
	if (!bh->b_uptodate) {
		ll_rw_block(READ, 1, &bh);
		wait_on_buffer(bh);
		if (!bh->b_uptodate) {
			brelse(bh);
			return NULL;
		}
	}
	p = nr + (unsigned short *) bh->b_data;
repeat:
	tmp = *p;
	if (tmp) {
		result = getblk(bh->b_dev, tmp, BLOCK_SIZE);
		if (tmp == *p) {
			brelse(bh);
			return result;
		}
		brelse(result);
		goto repeat;
	}
	if (!create) {
		brelse(bh);
		return NULL;
	}
	tmp = minix_new_block(inode->i_sb);
	if (!tmp) {
		brelse(bh);
		return NULL;
	}
	result = getblk(bh->b_dev, tmp, BLOCK_SIZE);
	if (*p) {
		minix_free_block(inode->i_sb,tmp);
		brelse(result);
		goto repeat;
	}
	*p = tmp;
	mark_buffer_dirty(bh, 1);
	brelse(bh);
	return result;
}
/*
	获取硬盘某块数据的内容，是对上面两个函数的封装，
	主要是计算block的逻辑,inode_getblk,block_getblk是已经知道要获取
	的是硬盘的哪个块。minix_getblk是给这两个函数计算出最终的硬盘块号
*/
struct buffer_head * minix_getblk(struct inode * inode, int block, int create)
{
	struct buffer_head * bh;

	if (block<0) {
		printk("minix_getblk: block<0");
		return NULL;
	}
	if (block >= 7+512+512*512) {
		printk("minix_getblk: block>big");
		return NULL;
	}
	if (block < 7)
		return inode_getblk(inode,block,create);
	block -= 7;
	if (block < 512) {
		bh = inode_getblk(inode,7,create);
		return block_getblk(inode, bh, block, create);
	}
	block -= 512;
	bh = inode_getblk(inode,8,create);
	bh = block_getblk(inode, bh, block>>9, create);
	return block_getblk(inode, bh, block & 511, create);
}
// 读某块内容，先从buffer获取，没有的话再去读取硬盘里的数据
struct buffer_head * minix_bread(struct inode * inode, int block, int create)
{
	struct buffer_head * bh;
	// 从buffer里读取对应硬盘块的数据
	bh = minix_getblk(inode,block,create);
	// 失败或者是最新的则返回
	if (!bh || bh->b_uptodate)
		return bh;
	// 获取到但是不是最新的数据则调驱动层去读取最新的
	ll_rw_block(READ, 1, &bh);
	wait_on_buffer(bh);
	// 是最新的则返回
	if (bh->b_uptodate)
		return bh;
	brelse(bh);
	return NULL;
}

void minix_read_inode(struct inode * inode)
{
	struct buffer_head * bh;
	struct minix_inode * raw_inode;
	int block, ino;
	// inode号
	ino = inode->i_ino;
	inode->i_op = NULL;
	inode->i_mode = 0;
	if (!ino || ino >= inode->i_sb->u.minix_sb.s_ninodes) {
		printk("Bad inode number on dev 0x%04x: %d is out of range\n",
			inode->i_dev, ino);
		return;
	}
	// 文件系统第一块是引导扇区，第二块是超级块，算出inode在硬盘的块号，imap是存储数据块位图相关的信息需要的空大小
	block = 2 + inode->i_sb->u.minix_sb.s_imap_blocks +
		    inode->i_sb->u.minix_sb.s_zmap_blocks +
		    (ino-1)/MINIX_INODES_PER_BLOCK;
	// 把整一块读进来
	if (!(bh=bread(inode->i_dev,block, BLOCK_SIZE))) {
		printk("Major problem: unable to read inode from dev 0x%04x\n",
			inode->i_dev);
		return;
	}
	// 算出inode在该块的块内偏移，得到inode的内容
	raw_inode = ((struct minix_inode *) bh->b_data) +
		    (ino-1)%MINIX_INODES_PER_BLOCK;
	// 赋值过去
	inode->i_mode = raw_inode->i_mode;
	inode->i_uid = raw_inode->i_uid;
	inode->i_gid = raw_inode->i_gid;
	inode->i_nlink = raw_inode->i_nlinks;
	inode->i_size = raw_inode->i_size;
	inode->i_mtime = inode->i_atime = inode->i_ctime = raw_inode->i_time;
	inode->i_blocks = inode->i_blksize = 0;
	// 如果是字符或者块文件，i_zone[0]保存的是设备号
	if (S_ISCHR(inode->i_mode) || S_ISBLK(inode->i_mode))
		inode->i_rdev = raw_inode->i_zone[0];
	// 一般文件i_zone保存的是文件内容的块号
	else for (block = 0; block < 9; block++)
		inode->u.minix_i.i_data[block] = raw_inode->i_zone[block];
	// 用完释放
	brelse(bh);
	// 根据文件类型赋值对应的操作函数集
	if (S_ISREG(inode->i_mode))
		inode->i_op = &minix_file_inode_operations;
	else if (S_ISDIR(inode->i_mode))
		inode->i_op = &minix_dir_inode_operations;
	else if (S_ISLNK(inode->i_mode))
		inode->i_op = &minix_symlink_inode_operations;
	else if (S_ISCHR(inode->i_mode))
		inode->i_op = &chrdev_inode_operations;
	else if (S_ISBLK(inode->i_mode))
		inode->i_op = &blkdev_inode_operations;
	else if (S_ISFIFO(inode->i_mode))
		init_fifo(inode);
}

static struct buffer_head * minix_update_inode(struct inode * inode)
{
	struct buffer_head * bh;
	struct minix_inode * raw_inode;
	int ino, block;

	ino = inode->i_ino;
	if (!ino || ino >= inode->i_sb->u.minix_sb.s_ninodes) {
		printk("Bad inode number on dev 0x%04x: %d is out of range\n",
			inode->i_dev, ino);
		inode->i_dirt = 0;
		return 0;
	}
	// 算出inode在硬盘哪个数据块,s_imap_blocks和s_zmap_blocks是数据位图和inode位图的块数
	block = 2 + inode->i_sb->u.minix_sb.s_imap_blocks + inode->i_sb->u.minix_sb.s_zmap_blocks +
		(ino-1)/MINIX_INODES_PER_BLOCK;
	if (!(bh=bread(inode->i_dev, block, BLOCK_SIZE))) {
		printk("unable to read i-node block\n");
		inode->i_dirt = 0;
		return 0;
	}
	// 算出inode在这个数据块的偏移
	raw_inode = ((struct minix_inode *)bh->b_data) +
		(ino-1)%MINIX_INODES_PER_BLOCK;
	raw_inode->i_mode = inode->i_mode;
	raw_inode->i_uid = inode->i_uid;
	raw_inode->i_gid = inode->i_gid;
	raw_inode->i_nlinks = inode->i_nlink;
	raw_inode->i_size = inode->i_size;
	raw_inode->i_time = inode->i_mtime;
	if (S_ISCHR(inode->i_mode) || S_ISBLK(inode->i_mode))
		raw_inode->i_zone[0] = inode->i_rdev;
	else for (block = 0; block < 9; block++)
		raw_inode->i_zone[block] = inode->u.minix_i.i_data[block];
	inode->i_dirt=0;
	// 回写inode内容到硬盘，上面的代码见read_inode注释
	mark_buffer_dirty(bh, 1);
	return bh;
}

void minix_write_inode(struct inode * inode)
{
	struct buffer_head *bh;
	bh = minix_update_inode(inode);
	brelse(bh);
}
// 回写inode到硬盘
int minix_sync_inode(struct inode * inode)
{
	int err = 0;
	struct buffer_head *bh;
	// 回写
	bh = minix_update_inode(inode);
	// 上面的回写是等待线程定期回写的，失败的话自己调驱动层直接回写
	if (bh && bh->b_dirt)
	{	
		// 驱动层会锁住buffer
		ll_rw_block(WRITE, 1, &bh);
		wait_on_buffer(bh);
		if (bh->b_req && !bh->b_uptodate)
		{
			printk ("IO error syncing minix inode [%04x:%08lx]\n",
				inode->i_dev, inode->i_ino);
			err = -1;
		}
	}
	else if (!bh)
		err = -1;
	brelse (bh);
	return err;
}

#ifdef MODULE

char kernel_version[] = UTS_RELEASE;

static struct file_system_type minix_fs_type = {
	minix_read_super, "minix", 1, NULL
};
// 支持以模块加载
int init_module(void)
{	
	// 注册文件系统
	register_filesystem(&minix_fs_type);
	return 0;
}
// 卸载模块
void cleanup_module(void)
{
	unregister_filesystem(&minix_fs_type);
}

#endif

