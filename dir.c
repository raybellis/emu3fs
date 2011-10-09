#include "emu3_fs.h"

static int emu3_readdir(struct file *f, void *dirent, filldir_t filldir)
{
    int i;
    int block_num;
    int entries_per_block;
    struct dentry *de = f->f_dentry;
   	struct emu3_sb_info *info = EMU3_SB(de->d_inode->i_sb);
   	struct emu3_block *b;
   	struct emu3_dentry * e3d;
   	struct emu3_inode * e3i;
   	
    //TODO: check error.
    if (de->d_inode->i_ino != ROOT_DIR_INODE_ID)
    	return -EBADF;

    if(f->f_pos > 0 )
    	return 0; //Returning an error here (-EBADF) makes ls giving a WRONG DESCRIPTOR FILE.
    
    if (filldir(dirent, ".", 1, f->f_pos++, de->d_inode->i_ino, DT_DIR) < 0)
    	return 0;
    if (filldir(dirent, "..", 2, f->f_pos++, de->d_parent->d_inode->i_ino, DT_DIR) < 0)
    	return 0;

	block_num = info->start_root_dir_block;
	e3i = EMU3_I(de->d_inode);
	for (i = 0; i < e3i->blocks; i++) {
		b = emu3_sb_bread(de->d_inode->i_sb, block_num);
	
		e3d = (struct emu3_dentry *)b->b_data;
	
		entries_per_block = 0;
		while (entries_per_block < MAX_ENTRIES_PER_BLOCK && IS_EMU3_FILE(e3d)) {
			if (filldir(dirent, e3d->name, MAX_LENGTH_FILENAME, f->f_pos++, EMU3_I_ID(e3d), DT_REG) < 0)
    			return 0;
			e3d++;
			entries_per_block++;
		}
	
		emu3_brelse(b);

		if (entries_per_block < MAX_ENTRIES_PER_BLOCK)
			break;

		block_num++;
	}
    return 0;
}

static struct dentry *emu3_lookup(struct inode *dir, struct dentry *dentry,
						struct nameidata *nd)
{
	int i;
	int block_num;
	int entries_per_block;
	struct inode *inode;
	struct emu3_block *b;
	struct emu3_sb_info *info = EMU3_SB(dir->i_sb);
   	struct emu3_dentry * e3d;
   	struct emu3_inode * e3i;

	if (dentry->d_name.len > MAX_LENGTH_FILENAME)
		return ERR_PTR(-ENAMETOOLONG);

	block_num = info->start_root_dir_block;
	e3i = EMU3_I(dir);
	for (i = 0; i < e3i->blocks; i++) {
		b = emu3_sb_bread(dir->i_sb, block_num);
	
		e3d = (struct emu3_dentry *)b->b_data;
	
		entries_per_block = 0;
		while (entries_per_block < MAX_ENTRIES_PER_BLOCK && IS_EMU3_FILE(e3d)) {
			if(strncmp(dentry->d_name.name, e3d->name, MAX_LENGTH_FILENAME) == 0) {
				inode = emu3_iget(dir->i_sb, EMU3_I_ID(e3d));
				d_add(dentry, inode);
				return NULL;
			}
			e3d++;
			entries_per_block++;
		}
	
		emu3_brelse(b);

		if (entries_per_block < MAX_ENTRIES_PER_BLOCK)
			break;

		block_num++;
	}

	return NULL;
}

const struct file_operations emu3_file_operations_dir = {
	.read		= generic_read_dir,
	.readdir	= emu3_readdir,
	.fsync		= generic_file_fsync,
	.llseek		= generic_file_llseek,
};

const struct inode_operations emu3_inode_operations_dir = {
	.create	= NULL,
	.lookup	= emu3_lookup,
	.link   = NULL,
	.unlink	= NULL,
	.rename	= NULL,
};