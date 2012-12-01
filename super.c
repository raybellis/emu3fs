/*
 *	super.c
 *	Copyright (C) 2011 David García Goñi <dagargo at gmail dot com>
 *
 *   This file is part of EMU3 Filesystem Tools.
 *
 *   EMU3 Filesystem Tools is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   EMU3 Filesystem Tool is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with EMU3 Filesystem Tool.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>

#include "emu3_fs.h"

struct kmem_cache * emu3_inode_cachep;

static int init_inodecache(void)
{
	emu3_inode_cachep = kmem_cache_create("emu3_inode_cache",
					     sizeof(struct emu3_inode),
					     0, (SLAB_RECLAIM_ACCOUNT|
						SLAB_MEM_SPREAD),
					     emu3_init_once);
	if (emu3_inode_cachep == NULL)
		return -ENOMEM;
	return 0;
}

static void destroy_inodecache(void)
{
	kmem_cache_destroy(emu3_inode_cachep);
}

static int emu3_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct super_block *sb = dentry->d_sb;
	struct emu3_sb_info *info = EMU3_SB(sb);
	u64 id = huge_encode_dev(sb->s_bdev->bd_dev);
	int free_clusters = 0;

	buf->f_type = EMU3_FS_TYPE;
	buf->f_bsize = EMU3_BSIZE;
	buf->f_blocks = info->clusters * info->blocks_per_cluster;
	buf->f_bfree = free_clusters * info->blocks_per_cluster;
	buf->f_bavail = buf->f_bfree;
	buf->f_files = info->used_inodes;
	buf->f_ffree = EMU3_MAX_FILES - info->used_inodes;
	buf->f_fsid.val[0] = (u32)id;
	buf->f_fsid.val[1] = (u32)(id >> 32);
	buf->f_namelen = LENGTH_FILENAME;
	return 0;
}

void emu3_mark_as_non_empty(struct super_block *sb) {
	struct emu3_sb_info *info = EMU3_SB(sb);
	struct buffer_head *bh;	
	char * data;

	bh = sb_bread(sb, 1);
	data = (char *) bh->b_data;
	data[0x0] = 0x0a;
	mark_buffer_dirty(bh);
	brelse(bh);
	
	bh = sb_bread(sb, info->start_info_block);
	data = (char *) bh->b_data;
	data[0x12] = 0x09;
	data[0x13] = 0x00;
	mark_buffer_dirty(bh);
	brelse(bh);
}

static void emu3_put_super(struct super_block *sb)
{
	struct emu3_sb_info *info = EMU3_SB(sb);

	mutex_lock(&info->lock);
	emu3_write_cluster_list(sb);
	if (info->used_inodes > 0) {
		emu3_mark_as_non_empty(sb);
	}
	mutex_unlock(&info->lock);	

	mutex_destroy(&info->lock);

	if (info) {
		kfree(info->cluster_list);
		kfree(info);
		sb->s_fs_info = NULL;
	}
}

//Base 0 search
int emu3_expand_cluster_list(struct inode * inode, sector_t block) {
	struct emu3_sb_info *info = EMU3_SB(inode->i_sb);
	struct emu3_inode *e3i = EMU3_I(inode);
	int cluster = ((int)block) / info->blocks_per_cluster;
	int next = e3i->start_cluster;
	int i = 0;
	while (info->cluster_list[next] != cpu_to_le16(LAST_CLUSTER_OF_FILE)) {
		next = info->cluster_list[next];
		i++;
	}
	while (i < cluster) {
		int new = emu3_next_available_cluster(info);
		if (new < 0) {
			return -ENOSPC;
		}
		info->cluster_list[next] = new;
		next = new;
		i++;
	}
	info->cluster_list[next] = cpu_to_le16(LAST_CLUSTER_OF_FILE);
	return 0;
}


//Base 0 search
int emu3_get_cluster(struct inode * inode, int n) {
	struct emu3_sb_info *info = EMU3_SB(inode->i_sb);
	struct emu3_inode *e3i = EMU3_I(inode);
	int next = e3i->start_cluster;
	int i = 0;
	while (i < n) {
		if (info->cluster_list[next] == cpu_to_le16(LAST_CLUSTER_OF_FILE)) {
			return -1;
		}
		next = info->cluster_list[next];
		i++;
	}
	return next;
}

void emu3_init_cluster_list(struct inode * inode) {
	struct emu3_sb_info *info = EMU3_SB(inode->i_sb);
	struct emu3_inode *e3i = EMU3_I(inode);
	info->cluster_list[e3i->start_cluster] = cpu_to_le16(LAST_CLUSTER_OF_FILE);
}

void emu3_clear_cluster_list(struct inode * inode) {
	struct emu3_sb_info *info = EMU3_SB(inode->i_sb);
	struct emu3_inode *e3i = EMU3_I(inode);
	int next = e3i->start_cluster;
	while (info->cluster_list[next] != cpu_to_le16(LAST_CLUSTER_OF_FILE)) {
		int prev = next;
		next = info->cluster_list[next];
		info->cluster_list[prev] = 0;
	}
	info->cluster_list[next] = 0;
}

//Prunes the cluster list to the real inode size
void emu3_update_cluster_list(struct inode * inode) {
	struct emu3_sb_info *info = EMU3_SB(inode->i_sb);
	short int clusters, last_cluster;
	int prunning;
	emu3_get_file_geom(inode, &clusters, NULL, NULL);
	last_cluster = emu3_get_cluster(inode, clusters - 1);
	prunning = 0; 
	while (info->cluster_list[last_cluster] != cpu_to_le16(LAST_CLUSTER_OF_FILE)) {
		int next = info->cluster_list[last_cluster];
		if (prunning) {
			info->cluster_list[last_cluster] = 0;
		}
		else {
			info->cluster_list[last_cluster] = cpu_to_le16(LAST_CLUSTER_OF_FILE);
		}
		last_cluster = next;
		prunning = 1;
	}
	if (prunning) {
		info->cluster_list[last_cluster] = 0;
	}
}

int emu3_next_available_cluster(struct emu3_sb_info * info) {
	int i;
	for (i = 1; i <= info->clusters; i++) {
		if (info->cluster_list[i] == 0) {
			return i;
		}
	}
	return -ENOSPC;
}

unsigned int emu3_get_phys_block(struct inode * inode, sector_t block) {
	struct emu3_sb_info *info = EMU3_SB(inode->i_sb);
	int cluster = ((int)block) / info->blocks_per_cluster; //cluster amount
	int offset = ((int)block) % info->blocks_per_cluster;
	cluster = emu3_get_cluster(inode, cluster);
	if (cluster == -1) {
		return -1;
	}
	return info->start_data_block + ((cluster - 1) * info->blocks_per_cluster) + offset;
}

static const struct super_operations emu3_super_operations = {
	.alloc_inode	= emu3_alloc_inode,
	.destroy_inode	= emu3_destroy_inode,
	.write_inode	= emu3_write_inode,
	.evict_inode	= emu3_evict_inode,
	.put_super      = emu3_put_super,
	.statfs		    = emu3_statfs
};

void emu3_write_cluster_list(struct super_block *sb) {
	struct emu3_sb_info *info = EMU3_SB(sb);
	struct buffer_head *bh;
	int i;
		
	for (i = 0; i < info->cluster_list_blocks; i++) {
		bh = sb_bread(sb, info->start_cluster_list_block + i);
		memcpy(bh->b_data, &info->cluster_list[EMU3_CENTRIES_PER_BLOCK * i], EMU3_BSIZE);
		mark_buffer_dirty(bh);
		brelse(bh);
	}
}

void emu3_read_cluster_list(struct super_block *sb) {
	struct emu3_sb_info *info = EMU3_SB(sb);
	struct buffer_head *bh;
	int i;
		
	for (i = 0; i < info->cluster_list_blocks; i++) {
		bh = sb_bread(sb, info->start_cluster_list_block + i);
		memcpy(&info->cluster_list[EMU3_CENTRIES_PER_BLOCK * i], bh->b_data, EMU3_BSIZE);
		brelse(bh);
	}
}

static int emu3_fill_super(struct super_block *sb, void *data, int silent)
{
	struct emu3_sb_info *info;
	struct buffer_head *sbh;
	unsigned char * e3sb;
	struct inode * inode;
	int err = 0;
	unsigned int * parameters;
	struct emu3_dentry * e3d;
	struct buffer_head *bh;
	int i, j;

	if (sb_set_blocksize(sb, EMU3_BSIZE) != EMU3_BSIZE) {
		printk(KERN_ERR "%s: impossible to mount. Linux does not allow 512B block size on this device.", EMU3_MODULE_NAME);
		return -EINVAL;
	}

	info = kzalloc(sizeof(struct emu3_sb_info), GFP_KERNEL);
	if (!info) {
		return -ENOMEM;
	}
	
	sb->s_fs_info = info;
		
	sbh = sb_bread(sb, 0);
	
	if (sbh) {
		e3sb = (unsigned char *)sbh->b_data;
		
		//Check EMU3 string
		if (strncmp(EMU3_FS_SIGNATURE, e3sb, 4) != 0) {
			err = -EINVAL;
			printk(KERN_ERR "%s: volume is not an EMU3 disk.", EMU3_MODULE_NAME);
		}
		else {
			parameters = (unsigned int *) e3sb;
			
			info->blocks = cpu_to_le32(parameters[1]); //TODO: add 1 ??? Do we really use this?
			info->start_info_block = cpu_to_le32(parameters[2]);
			info->info_blocks = cpu_to_le32(parameters[3]);
			info->start_root_dir_block = cpu_to_le32(parameters[4]);
			info->root_dir_blocks = cpu_to_le32(parameters[5]);
			info->start_cluster_list_block = cpu_to_le32(parameters[6]);
			info->cluster_list_blocks = cpu_to_le32(parameters[7]);
			info->start_data_block = cpu_to_le32(parameters[8]);
			info->blocks_per_cluster = (0x10000 << (e3sb[0x28] - 1)) / EMU3_BSIZE;
			info->clusters = cpu_to_le32(parameters[9]);
			
			//We need to calculate the used inodes
			info->used_inodes = 0;

			for (i = 0; i < info->root_dir_blocks; i++) {
				bh = sb_bread(sb, info->start_root_dir_block + i);
	
				e3d = (struct emu3_dentry *)bh->b_data;
		
				for (j = 0; j < MAX_ENTRIES_PER_BLOCK; j++) {
					if (IS_EMU3_FILE(e3d)) {
						info->used_inodes++;
					}
					e3d++;
				}
				brelse(bh);
			}
			//Calculations done.
			
			//Now it's time to read the cluster list
			info->cluster_list = kzalloc(EMU3_BSIZE * info->cluster_list_blocks, GFP_KERNEL);
			if (!info->cluster_list) {
				return -ENOMEM;
			}
			emu3_read_cluster_list(sb);
			//Done.

			printk("%s: %d blocks, %d clusters, b/c %d.\n", EMU3_MODULE_NAME, info->blocks, info->clusters, info->blocks_per_cluster);
			printk("%s: info init block @ %d + %d blocks.\n", EMU3_MODULE_NAME, info->start_info_block, info->info_blocks);
			printk("%s: cluster list init block @ %d + %d blocks.\n", EMU3_MODULE_NAME, info->start_cluster_list_block, info->cluster_list_blocks);
			printk("%s: root init block @ %d + %d blocks.\n", EMU3_MODULE_NAME, info->start_root_dir_block, info->root_dir_blocks);
			printk("%s: data init block @ %d + %d clusters.\n", EMU3_MODULE_NAME, info->start_data_block, info->clusters);
						
			sb->s_op = &emu3_super_operations;

			inode = emu3_get_inode(sb, ROOT_DIR_INODE_ID);

			if (!inode) {
				err = -EIO;
			}
			else {
		    	sb->s_root = d_make_root(inode);
		    	if (!sb->s_root) {
		            iput(inode);
					err = -EIO;
		    	}
        	}	
		}
	}
	
	if (!err) {
		mutex_init(&info->lock);
	}
	else {
		kfree(info);
		sb->s_fs_info = NULL;
	}
	
	brelse(sbh);
	return err;
}

static struct dentry *emu3_fs_mount(struct file_system_type *fs_type,
	int flags, const char *dev_name, void *data)
{
	return mount_bdev(fs_type, flags, dev_name, data, emu3_fill_super);
}

static struct file_system_type emu3_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "emu3",
	.mount		= emu3_fs_mount,
	.kill_sb	= kill_block_super,
	.fs_flags	= FS_REQUIRES_DEV,
};

static int __init init(void)
{
	int err;
	printk(KERN_INFO "Init %s.\n", EMU3_MODULE_NAME);
	err = init_inodecache();
	if (err) {
		return err;
	}
	err = register_filesystem(&emu3_fs_type);
	if (err) {
		destroy_inodecache();
	}
	return err;
}

static void __exit exit(void)
{
	unregister_filesystem(&emu3_fs_type);
	destroy_inodecache();
	printk(KERN_INFO "Exit %s.\n", EMU3_MODULE_NAME);
}

module_init(init);
module_exit(exit);

MODULE_LICENSE("GPL");

MODULE_AUTHOR("David García Goñi <dagargo at gmail dot com>");
MODULE_DESCRIPTION("E-mu E3 sampler family filesystem for Linux");