/*
 *   dir.c
 *   Copyright (C) 2018 David García Goñi <dagargo@gmail.com>
 *
 *   This file is part of emu3fs.
 *
 *   emu3fs is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   emu3fs is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with emu3fs. If not, see <http://www.gnu.org/licenses/>.
 */

#include "emu3_fs.h"

static void emu3_set_dentry_name(struct emu3_dentry *e3d, struct qstr *q)
{
	memcpy(e3d->name, q->name, q->len);
	memset(&e3d->name[q->len], ' ', EMU3_LENGTH_FILENAME - q->len);
}

static void emu3_filename_fix(char *in, char *out)
{
	int i;
	char c;

	for (i = 0; i < EMU3_LENGTH_FILENAME; i++) {
		c = in[i];
		// 32 <= c <= 126
		if (c == '/')
			c = '?';	//Whatever will be nicer
		out[i] = c;
	}
}

static int emu3_filename_length(const char *filename)
{
	const char *last = &filename[EMU3_LENGTH_FILENAME - 1];
	int len;

	for (len = EMU3_LENGTH_FILENAME; len > 0; len--) {
		if (*last != ' ' && *last != '\0')
			return len;
		last--;
	}

	return -1;		//A dentry with an empty name?
}

static int emu3_strncmp(struct dentry *dentry, struct emu3_dentry *e3d)
{
	int len;
	char fixed[EMU3_LENGTH_FILENAME];

	emu3_filename_fix(e3d->name, fixed);
	len = emu3_filename_length(e3d->name);
	len = len > dentry->d_name.len ? len : dentry->d_name.len;
	return strncmp(fixed, dentry->d_name.name, dentry->d_name.len);
}

static struct emu3_dentry *emu3_find_dentry_by_name_in_blk(struct inode *dir, struct dentry
							   *dentry, struct buffer_head
							   **b, sector_t blknum,
							   unsigned long *ino)
{
	unsigned int i;
	struct emu3_dentry *e3d;

	*b = sb_bread(dir->i_sb, blknum);
	e3d = (struct emu3_dentry *)(*b)->b_data;
	for (i = 0; i < EMU3_ENTRIES_PER_BLOCK; i++, e3d++) {
		if (!EMU3_DENTRY_IS_DIR(e3d) && !EMU3_DENTRY_IS_FILE(e3d))
			continue;

		if (!emu3_strncmp(dentry, e3d)) {
			if (ino)
				*ino = EMU3_I_ID(blknum, i);
			return e3d;
		}

	}
	brelse(*b);
	return NULL;
}

static struct emu3_dentry *emu3_find_dentry_by_name(struct inode *dir,
						    struct dentry *dentry,
						    struct buffer_head **b,
						    unsigned long *ino)
{
	int i;
	short blknum;
	struct buffer_head *db;
	struct emu3_dentry *e3d, *res;
	struct emu3_sb_info *info = EMU3_SB(dir->i_sb);

	if (EMU3_IS_I_ROOT_DIR(dir)) {
		for (i = 0; i < info->root_blocks; i++) {
			blknum = le16_to_cpu(info->start_root_block) + i;
			if (blknum < 0)
				break;

			res =
			    emu3_find_dentry_by_name_in_blk(dir, dentry, b,
							    blknum, ino);
			if (res)
				return res;
		}
		return NULL;
	}

	e3d = emu3_find_dentry_by_inode(dir, &db);

	if (!e3d)
		return NULL;

	if (!EMU3_DENTRY_IS_DIR(e3d))
		goto cleanup;

	for (i = 0; i < EMU3_BLOCKS_PER_DIR; i++) {
		blknum = le16_to_cpu(e3d->dattrs.block_list[i]);
		if (blknum < 0)
			continue;

		res =
		    emu3_find_dentry_by_name_in_blk(dir, dentry, b,
						    blknum, ino);
		if (res) {
			brelse(db);
			return res;
		}
	}

 cleanup:
	brelse(db);
	return NULL;
}

static int emu3_emit(struct dir_context *ctx,
		     struct emu3_dentry *e3d, unsigned int blknum,
		     unsigned int offset, unsigned type,
		     struct emu3_sb_info *info)
{
	int len;
	unsigned int ino;
	char fixed[EMU3_LENGTH_FILENAME];

	emu3_filename_fix(e3d->name, fixed);
	len = emu3_filename_length(fixed);
	ino = emu3_get_or_add_i_map(info, EMU3_I_ID(blknum, offset));
	ctx->pos++;
	return dir_emit(ctx, fixed, len, ino, type);
}

static int emu3_iterate_dir(struct file *f, struct dir_context *ctx,
			    struct inode *dir, struct emu3_sb_info *info)
{
	loff_t k;
	unsigned int i, j;
	short blknum;
	struct buffer_head *b;
	struct buffer_head *db;
	struct emu3_dentry *e3d;
	struct emu3_dentry *e3d_dir;

	k = 2;
	e3d_dir = emu3_find_dentry_by_inode(dir, &db);

	if (!EMU3_DENTRY_IS_DIR(e3d_dir))
		goto cleanup;

	for (i = 0; i < EMU3_BLOCKS_PER_DIR; i++) {
		blknum = le16_to_cpu(e3d_dir->dattrs.block_list[i]);
		if (blknum < 0)
			break;

		b = sb_bread(dir->i_sb, blknum);
		e3d = (struct emu3_dentry *)b->b_data;
		for (j = 0; j < EMU3_ENTRIES_PER_BLOCK; j++, e3d++) {
			if (!EMU3_DENTRY_IS_FILE(e3d))
				continue;

			if (ctx->pos == k) {
				if (!emu3_emit
				    (ctx, e3d, blknum, j, DT_REG, info)) {
					brelse(b);
					goto cleanup;
				}
			}
			k++;
		}
		brelse(b);
	}

 cleanup:
	brelse(db);
	return k;
}

static int emu3_iterate_root(struct file *f, struct dir_context *ctx,
			     struct inode *dir, struct emu3_sb_info *info)
{
	loff_t k;
	unsigned int i, j, blknum;
	struct emu3_dentry *e3d;
	struct buffer_head *b;

	k = 2;
	for (i = 0; i < info->root_blocks; i++) {
		blknum = info->start_root_block + i;
		b = sb_bread(dir->i_sb, blknum);
		e3d = (struct emu3_dentry *)b->b_data;

		for (j = 0; j < EMU3_ENTRIES_PER_BLOCK; j++, e3d++) {
			if (!EMU3_DENTRY_IS_DIR(e3d))
				continue;

			if (ctx->pos == k) {
				if (!emu3_emit
				    (ctx, e3d, blknum, j, DT_DIR, info)) {
					brelse(b);
					return k;
				}
			}
			k++;
		}
		brelse(b);
	}

	return k;
}

static int emu3_iterate(struct file *f, struct dir_context *ctx)
{
	struct inode *dir = file_inode(f);
	struct emu3_sb_info *info = EMU3_SB(dir->i_sb);

	if (!EMU3_IS_I_ROOT_DIR(dir) && !EMU3_IS_I_REG_DIR(dir))
		return -EBADF;

	if (ctx->pos == 0) {
		ctx->pos++;
		if (!dir_emit(ctx, ".", 1, dir->i_ino, DT_DIR))
			return 0;
	}

	if (ctx->pos == 1) {
		ctx->pos++;
		if (!dir_emit
		    (ctx, "..", 2, f->f_path.dentry->d_inode->i_ino, DT_DIR))
			return 0;
	}

	if (EMU3_IS_I_ROOT_DIR(dir))
		return emu3_iterate_root(f, ctx, dir, info);
	else
		return emu3_iterate_dir(f, ctx, dir, info);
}

static struct dentry *emu3_lookup(struct inode *dir,
				  struct dentry *dentry, unsigned int flags)
{
	struct inode *inode = NULL;
	struct buffer_head *b;
	struct emu3_dentry *e3d;
	unsigned long ino;

	if (dentry->d_name.len > EMU3_LENGTH_FILENAME)
		return ERR_PTR(-ENAMETOOLONG);

	e3d = emu3_find_dentry_by_name(dir, dentry, &b, &ino);

	if (e3d) {
		brelse(b);
		inode = emu3_get_inode(dir->i_sb, ino);
		if (IS_ERR(inode))
			return ERR_CAST(inode);
	}

	return d_splice_alias(inode, dentry);
}

static int emu3_get_free_file_id(struct inode *dir)
{
	int i, j, id = -1;
	short *block;
	short blknum;
	bool ids[EMU3_MAX_FILES_PER_DIR];
	struct buffer_head *b, *db;
	struct emu3_dentry *e3d, *e3d_dir;
	struct emu3_sb_info *info = EMU3_SB(dir->i_sb);

	for (i = 0; i < EMU3_MAX_FILES_PER_DIR; i++)
		ids[i] = 0;

	e3d_dir = emu3_find_dentry_by_inode(dir, &db);

	if (!e3d_dir)
		return -1;

	if (!EMU3_DENTRY_IS_DIR(e3d_dir))
		goto cleanup;

	block = e3d_dir->dattrs.block_list;

	for (i = 0; i < EMU3_BLOCKS_PER_DIR; i++, block++) {
		blknum = le16_to_cpu(*block);
		if (!EMU3_DIR_BLOCK_OK(blknum, info))
			break;

		b = sb_bread(dir->i_sb, *block);

		e3d = (struct emu3_dentry *)b->b_data;

		for (j = 0; j < EMU3_ENTRIES_PER_BLOCK; j++, e3d++) {
			if (EMU3_DENTRY_IS_FILE(e3d))
				ids[e3d->id] = 1;
		}

		brelse(b);
	}

	for (i = 0; i < EMU3_MAX_FILES_PER_DIR; i++)
		if (!ids[i]) {
			id = i;
			break;
		}

 cleanup:
	brelse(db);
	return id;
}

static unsigned int emu3_count_used_dir_blocks(struct inode *dir)
{
	int i;
	unsigned int blocks = 0;
	short *block;
	short blknum;
	struct buffer_head *b;
	struct emu3_dentry *e3d;
	struct emu3_sb_info *info = EMU3_SB(dir->i_sb);

	e3d = emu3_find_dentry_by_inode(dir, &b);

	block = e3d->dattrs.block_list;
	for (i = 0; i < EMU3_BLOCKS_PER_DIR; i++, block++) {
		blknum = le16_to_cpu(*block);
		if (EMU3_DIR_BLOCK_OK(blknum, info))
			blocks++;
	}

	brelse(b);

	return blocks;
}

static struct emu3_dentry *emu3_find_empty_file_dentry(struct inode *dir,
						       struct buffer_head **b,
						       unsigned int *ino,
						       unsigned int *dir_blocks)
{
	int i, j;
	short *block;
	short blknum;
	struct buffer_head *db;
	struct emu3_dentry *e3d;
	struct emu3_dentry *e3d_dir;
	struct emu3_sb_info *info = EMU3_SB(dir->i_sb);

	e3d_dir = emu3_find_dentry_by_inode(dir, &db);

	if (!e3d_dir)
		return NULL;

	if (!EMU3_DENTRY_IS_DIR(e3d_dir))
		goto cleanup;

	block = e3d_dir->dattrs.block_list;
	for (i = 0; i < EMU3_BLOCKS_PER_DIR; i++, block++) {
		blknum = le16_to_cpu(*block);
		if (!EMU3_DIR_BLOCK_OK(blknum, info))
			break;

		*b = sb_bread(dir->i_sb, blknum);

		e3d = (struct emu3_dentry *)(*b)->b_data;
		for (j = 0; j < EMU3_ENTRIES_PER_BLOCK; j++, e3d++) {
			if (!EMU3_DENTRY_IS_FILE(e3d)) {
				*ino = EMU3_I_ID(blknum, j);
				brelse(db);
				return e3d;
			}
		}

		brelse(*b);
	}

	if (i == EMU3_BLOCKS_PER_DIR)
		goto cleanup;

	for (j = 0; j < info->dir_content_blocks; j++) {
		if (!info->dir_content_block_list[j]) {
			info->dir_content_block_list[j] = 1;
			blknum = info->start_dir_content_block + j;
			e3d_dir->dattrs.block_list[i] = cpu_to_le16(blknum);
			mark_buffer_dirty_inode(db, dir);
			*b = sb_bread(dir->i_sb, blknum);
			*ino = EMU3_I_ID(blknum, 0);
			e3d = (struct emu3_dentry *)(*b)->b_data;
			brelse(db);
			(*dir_blocks)++;
			return e3d;
		}
	}

 cleanup:
	brelse(db);
	return NULL;
}

static int emu3_add_file_dentry(struct inode *dir, struct dentry *dentry,
				unsigned int *ino, short *start_cluster,
				unsigned int *dir_blocks)
{
	int id;
	int ret = 0;
	struct buffer_head *b;
	struct emu3_dentry *e3d;
	struct super_block *sb = dir->i_sb;
	struct emu3_sb_info *info = EMU3_SB(sb);

	if (!dentry->d_name.len)
		return -ENOENT;

	if (dentry->d_name.len > EMU3_LENGTH_FILENAME)
		return -ENAMETOOLONG;

	*start_cluster = emu3_next_free_cluster(info);

	if (*start_cluster < 0) {
		ret = -ENOSPC;
		goto cleanup;
	}

	e3d = emu3_find_empty_file_dentry(dir, &b, ino, dir_blocks);

	if (!e3d)
		return -ENOSPC;

	id = emu3_get_free_file_id(dir);

	if (id < 0) {
		printk(KERN_CRIT
		       "%s: No ID available for a newly created file\n",
		       EMU3_MODULE_NAME);
		ret = -ENOSPC;
		goto cleanup;
	}

	emu3_set_dentry_name(e3d, &dentry->d_name);
	e3d->unknown = 0;
	e3d->id = id;
	e3d->fattrs.start_cluster = le16_to_cpu(*start_cluster);
	e3d->fattrs.clusters = le16_to_cpu(1);
	e3d->fattrs.blocks = le16_to_cpu(1);
	e3d->fattrs.bytes = le16_to_cpu(0);
	e3d->fattrs.type = EMU3_FTYPE_STD;
	memset(e3d->fattrs.props, 0, 5);
	//TODO: fix timestamps
	dir->i_mtime = current_time(dir);

	mark_buffer_dirty_inode(b, dir);

 cleanup:
	brelse(b);
	return ret;
}

static int emu3_create_inode(struct inode *dir, struct dentry *dentry,
			     umode_t mode, struct inode **inode_ret)
{
	int err;
	struct inode *inode;
	unsigned int dir_blocks, new_dir_blocks, ino;
	short start_cluster;
	struct super_block *sb = dir->i_sb;
	struct emu3_sb_info *info = EMU3_SB(sb);

	//Files are not allowed at root
	if (EMU3_IS_I_ROOT_DIR(dir))
		return -EPERM;

	inode = new_inode(sb);
	if (!inode)
		return -ENOSPC;

	dir_blocks = emu3_count_used_dir_blocks(dir);
	new_dir_blocks = dir_blocks;
	err =
	    emu3_add_file_dentry(dir, dentry, &ino, &start_cluster,
				 &new_dir_blocks);
	if (err) {
		iput(inode);
		return err;
	}

	inode_init_owner(inode, dir, mode);
	inode->i_mtime = inode->i_atime = inode->i_ctime = current_time(inode);
	inode->i_blocks = info->blocks_per_cluster * EMU3_BSIZE;
	inode->i_op = &emu3_inode_operations_file;
	inode->i_fop = &emu3_file_operations_file;
	inode->i_mapping->a_ops = &emu3_aops;
	inode->i_ino = ino;
	inode->i_size = 0;
	EMU3_I(inode)->start_cluster = start_cluster;
	emu3_init_cluster_list(inode);
	insert_inode_hash(inode);
	mark_inode_dirty(inode);

	if (dir_blocks != new_dir_blocks) {
		dir->i_blocks = new_dir_blocks;
		dir->i_size = new_dir_blocks * EMU3_BSIZE;
		dir->i_mtime = current_time(inode);
		mark_inode_dirty(dir);
	}

	d_instantiate(dentry, inode);

	if (inode_ret)
		*inode_ret = inode;

	return 0;
}

static int emu3_create(struct inode *dir, struct dentry *dentry, umode_t mode,
		       bool excl)
{
	int err;
	struct super_block *sb = dir->i_sb;
	struct emu3_sb_info *info = EMU3_SB(sb);

	mutex_lock(&info->lock);
	err = emu3_create_inode(dir, dentry, mode, NULL);
	mutex_unlock(&info->lock);

	return err;
}

static bool emu3_is_dir_blk_used(struct emu3_dentry *e3d)
{
	int i;

	for (i = 0; i < EMU3_ENTRIES_PER_BLOCK; i++, e3d++) {
		if (EMU3_DENTRY_IS_FILE(e3d))
			return 1;
	}

	return 0;
}

static bool emu3_is_dir_empty(struct emu3_dentry *e3d_dir,
			      struct super_block *sb)
{
	int i;
	struct buffer_head *b;
	struct emu3_dentry *e3d;
	short *block = e3d_dir->dattrs.block_list;

	for (i = 0; i < EMU3_BLOCKS_PER_DIR; i++, block++) {
		if (EMU3_IS_DIR_BLOCK_FREE(*block))
			break;

		b = sb_bread(sb, *block);

		e3d = (struct emu3_dentry *)b->b_data;

		if (emu3_is_dir_blk_used(e3d)) {
			brelse(b);
			return 0;
		}

		brelse(b);
	}

	return 1;
}

static struct emu3_dentry *emu3_find_empty_dir_dentry(struct super_block *sb,
						      struct buffer_head **b,
						      unsigned long *ino)
{
	int i, j;
	struct emu3_dentry *e3d;
	struct emu3_sb_info *info = EMU3_SB(sb);

	for (i = 0; i < info->root_blocks; i++) {
		*b = sb_bread(sb, info->start_root_block + i);

		e3d = (struct emu3_dentry *)(*b)->b_data;

		for (j = 0; j < EMU3_ENTRIES_PER_BLOCK; j++, e3d++) {
			if (!EMU3_DENTRY_IS_DIR(e3d)) {
				*ino = EMU3_I_ID(info->start_root_block + i, j);
				return e3d;
			}
		}

		brelse(*b);
	}

	return NULL;
}

static int emu3_add_dir_dentry(struct inode *dir, struct qstr *q,
			       unsigned long *ino)
{
	int i;
	struct buffer_head *b;
	struct emu3_dentry *e3d;
	struct emu3_sb_info *info = EMU3_SB(dir->i_sb);
	struct super_block *sb = dir->i_sb;

	if (!q->len)
		return -ENOENT;

	if (q->len > EMU3_LENGTH_FILENAME)
		return -ENAMETOOLONG;

	e3d = emu3_find_empty_dir_dentry(sb, &b, ino);

	if (!e3d)
		return -ENOSPC;

	for (i = 0; i < info->dir_content_blocks; i++)
		if (!info->dir_content_block_list[i])
			break;

	if (i == info->dir_content_blocks) {
		brelse(b);
		return -ENOSPC;
	}

	info->dir_content_block_list[i] = 1;

	emu3_set_dentry_name(e3d, q);
	e3d->unknown = 0;
	e3d->id = EMU3_DTYPE_1;
	e3d->dattrs.block_list[0] =
	    cpu_to_le16(info->start_dir_content_block + i);
	for (i = 1; i < EMU3_BLOCKS_PER_DIR; i++) {
		e3d->dattrs.block_list[i] = cpu_to_le16(EMU3_FREE_DIR_BLOCK);
	}
	//TODO: fix timestamps
	dir->i_mtime = current_time(dir);
	mark_buffer_dirty_inode(b, dir);
	brelse(b);

	return 0;
}

static int emu3_unlink(struct inode *dir, struct dentry *dentry)
{
	struct buffer_head *b;
	struct emu3_dentry *e3d;
	struct inode *inode = dentry->d_inode;
	struct emu3_sb_info *info = EMU3_SB(inode->i_sb);

	e3d = emu3_find_dentry_by_name(dir, dentry, &b, NULL);

	if (e3d == NULL)
		return -ENOENT;

	mutex_lock(&info->lock);

	e3d->fattrs.type = EMU3_FTYPE_DEL;
	mark_buffer_dirty_inode(b, dir);
	dir->i_ctime = dir->i_mtime = current_time(dir);
	mark_inode_dirty(dir);
	inode->i_ctime = dir->i_ctime;
	inode_dec_link_count(inode);
	emu3_clear_cluster_list(inode);
	brelse(b);

	mutex_unlock(&info->lock);

	return 0;
}

static int emu3_rename(struct inode *old_dir, struct dentry *old_dentry,
		       struct inode *new_dir, struct dentry *new_dentry,
		       unsigned int flags)
{
	int err = 0;
	struct super_block *sb = old_dentry->d_inode->i_sb;
	struct emu3_sb_info *info = EMU3_SB(sb);
	struct buffer_head *old_b, *new_b;
	struct emu3_dentry *old_e3d, *new_e3d;

	if (flags & ~RENAME_NOREPLACE)
		return -EINVAL;

	// As inode numbers are addresses, we can not move a file to another directory and keep the original inode number.
	if (old_dir != new_dir)
		return -EPERM;

	mutex_lock(&info->lock);

	old_e3d = emu3_find_dentry_by_name(old_dir, old_dentry, &old_b, NULL);
	if (!old_e3d) {
		err = -ENOENT;
		goto end;
	}

	emu3_set_dentry_name(old_e3d, &new_dentry->d_name);
	mark_buffer_dirty_inode(old_b, old_dir);
	old_dir->i_mtime = current_time(old_dir);
	mark_inode_dirty(old_dir);

	if (new_dentry->d_inode) {
		if (flags & RENAME_NOREPLACE) {
			err = -EEXIST;
			goto end;
		}

		new_e3d =
		    emu3_find_dentry_by_name(new_dir, new_dentry, &new_b, NULL);
		if (new_e3d) {
			new_e3d->fattrs.type = EMU3_FTYPE_DEL;
			mark_buffer_dirty_inode(new_b, new_dir);
			new_dir->i_mtime = current_time(new_dir);
			mark_inode_dirty(new_dir);
		} else
			printk(KERN_WARNING
			       "%s: No entry found. As it was meant to be deleted we can continue safely.\n",
			       EMU3_MODULE_NAME);

		emu3_clear_cluster_list(new_dentry->d_inode);
		inode_dec_link_count(new_dentry->d_inode);
		d_delete(new_dentry);
	}

	brelse(old_b);

 end:
	mutex_unlock(&info->lock);
	return err;
}

static int emu3_mkdir(struct inode *dir, struct dentry *dentry, umode_t mode)
{
	int err;
	unsigned long ino;
	struct inode *inode;
	struct super_block *sb = dir->i_sb;
	struct emu3_sb_info *info = EMU3_SB(sb);

	if (!EMU3_IS_I_ROOT_DIR(dir))
		return -EPERM;

	inode = new_inode(sb);
	if (!inode)
		return -ENOSPC;

	mutex_lock(&info->lock);

	err = emu3_add_dir_dentry(dir, &dentry->d_name, &ino);

	if (err) {
		mutex_unlock(&info->lock);
		iput(inode);
		return err;
	}

	inode_init_owner(inode, dir, EMU3_DIR_MODE);
	inode->i_blocks = 1;
	inode->i_op = &emu3_inode_operations_dir;
	inode->i_fop = &emu3_file_operations_dir;
	inode->i_ino = ino;
	inode->i_size = EMU3_BSIZE;
	inode->i_mtime = inode->i_atime = inode->i_ctime = current_time(inode);
	insert_inode_hash(inode);
	mark_inode_dirty(inode);
	mutex_unlock(&info->lock);

	d_instantiate(dentry, inode);

	return 0;
}

static int emu3_rmdir(struct inode *dir, struct dentry *dentry)
{
	int i, ret = 0;
	short blknum;
	struct buffer_head *b;
	struct emu3_dentry *e3d;
	struct inode *inode = d_inode(dentry);
	struct emu3_sb_info *info = EMU3_SB(inode->i_sb);

	mutex_lock(&info->lock);

	e3d = emu3_find_dentry_by_inode(inode, &b);
	if (!e3d) {
		ret = -ENOENT;
		goto end;
	}

	if (!EMU3_DENTRY_IS_DIR(e3d)) {
		ret = -ENOTDIR;
		goto cleanup;
	}

	if (!emu3_is_dir_empty(e3d, inode->i_sb)) {
		ret = -ENOTEMPTY;
		goto cleanup;
	}

	e3d->unknown = 0;
	e3d->id = 0;
	for (i = 0; i < EMU3_BLOCKS_PER_DIR; i++) {
		if (EMU3_IS_DIR_BLOCK_FREE(e3d->dattrs.block_list[i]))
			break;
		blknum =
		    le16_to_cpu(e3d->dattrs.block_list[i]) -
		    info->start_dir_content_block;
		info->dir_content_block_list[blknum] = 0;
	}
	memset(e3d, 0, sizeof(struct emu3_dentry));
	inode_dec_link_count(dir);
	inode_dec_link_count(inode);
	mark_buffer_dirty_inode(b, dir);
 cleanup:
	brelse(b);
 end:
	mutex_unlock(&info->lock);
	return ret;
}

const struct file_operations emu3_file_operations_dir = {
	.read = generic_read_dir,
	.iterate = emu3_iterate,
	.fsync = generic_file_fsync,
	.llseek = generic_file_llseek,
};

const struct inode_operations emu3_inode_operations_dir = {
	.create = emu3_create,
	.lookup = emu3_lookup,
	.unlink = emu3_unlink,
	.rename = emu3_rename,
	.mkdir = emu3_mkdir,
	.rmdir = emu3_rmdir
};
