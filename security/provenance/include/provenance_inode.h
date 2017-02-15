/*
 *
 * Author: Thomas Pasquier <tfjmp@seas.harvard.edu>
 *
 * Copyright (C) 2016 Harvard University
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2, as
 * published by the Free Software Foundation; either version 2 of the License, or
 *	(at your option) any later version.
 *
 */
#ifndef CONFIG_SECURITY_PROVENANCE_INODE
#define CONFIG_SECURITY_PROVENANCE_INODE
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/xattr.h>

#include "provenance_long.h"    // for record_inode_name
#include "provenance_secctx.h"  // for record_inode_name

#define is_inode_dir(inode) S_ISDIR(inode->i_mode)
#define is_inode_socket(inode) S_ISSOCK(inode->i_mode)
#define is_inode_file(inode) S_ISREG(inode->i_mode)

static inline void record_inode_type(uint16_t mode, struct provenance *prov)
{
	uint64_t type = ENT_INODE_UNKNOWN;
	unsigned long irqflags;

	if (S_ISBLK(mode))
		type = ENT_INODE_BLOCK;
	else if (S_ISCHR(mode))
		type = ENT_INODE_CHAR;
	else if (S_ISDIR(mode))
		type = ENT_INODE_DIRECTORY;
	else if (S_ISFIFO(mode))
		type = ENT_INODE_FIFO;
	else if (S_ISLNK(mode))
		type = ENT_INODE_LINK;
	else if (S_ISREG(mode))
		type = ENT_INODE_FILE;
	else if (S_ISSOCK(mode))
		type = ENT_INODE_SOCKET;
	spin_lock_irqsave_nested(prov_lock(prov), irqflags, PROVENANCE_LOCK_INODE);
	prov_msg(prov)->inode_info.mode = mode;
	prov_type(prov_msg(prov)) = type;
	spin_unlock_irqrestore(prov_lock(prov), irqflags);
}

static inline void provenance_mark_as_opaque(const char *name)
{
	struct path path;
	struct provenance *prov;

	if (kern_path(name, LOOKUP_FOLLOW, &path)) {
		printk(KERN_ERR "Provenance: Failed file look up (%s).", name);
		return;
	}
	prov = path.dentry->d_inode->i_provenance;
	if (prov)
		set_opaque(prov_msg(prov));
}

static inline void refresh_inode_provenance(struct inode *inode)
{
	struct provenance *prov = inode->i_provenance;

	// will not be recorded
	if( provenance_is_opaque(prov_msg(prov)) )
		return;

	record_inode_name(inode, prov);
	prov_msg(prov)->inode_info.uid = __kuid_val(inode->i_uid);
	prov_msg(prov)->inode_info.gid = __kgid_val(inode->i_gid);
	security_inode_getsecid(inode, &(prov_msg(prov)->inode_info.secid));
}

static inline struct provenance *branch_mmap(union prov_msg *iprov, union prov_msg *cprov)
{
	//used for private MMAP
	struct provenance *prov;
	union prov_msg relation;

	if (unlikely(iprov == NULL || cprov == NULL)) // should not occur
		return NULL;
	if (!provenance_is_tracked(iprov) && !provenance_is_tracked(cprov) && !prov_all)
		return NULL;
	if (!should_record_relation(RL_MMAP, cprov, iprov, FLOW_ALLOWED))
		return NULL;
	prov = alloc_provenance(ENT_INODE_MMAP, GFP_KERNEL);

	prov_msg(prov)->inode_info.uid = iprov->inode_info.uid;
	prov_msg(prov)->inode_info.gid = iprov->inode_info.gid;
	memcpy(prov_msg(prov)->inode_info.sb_uuid, iprov->inode_info.sb_uuid, 16 * sizeof(uint8_t));
	prov_msg(prov)->inode_info.mode = iprov->inode_info.mode;
	__record_node(iprov);
	memset(&relation, 0, sizeof(union prov_msg));
	__propagate(RL_MMAP, iprov, prov_msg(prov), &relation, FLOW_ALLOWED);
	__record_node(prov_msg(prov));
	__record_relation(RL_MMAP, &(iprov->msg_info.identifier), &(prov_msg(prov)->msg_info.identifier), &relation, FLOW_ALLOWED, NULL);
	return prov;
}

static inline int inode_init_provenance(struct inode *inode, struct dentry *opt_dentry)
{
	struct provenance *prov = inode->i_provenance;
	union prov_msg *buf;
	struct dentry *dentry;
	int rc=0;

	if(prov->initialised)
		return 0;
	//spin_lock_nested(prov_lock(prov), PROVENANCE_LOCK_INODE);
	record_inode_type(inode->i_mode, prov);
	if( !(inode->i_opflags & IOP_XATTR) ) // xattr not support on this inode
		goto out;
	if(opt_dentry)
		dentry = dget(opt_dentry);
	else
		dentry = d_find_alias(inode);
	if(!dentry)
		goto out;
	buf = kmalloc(sizeof(union prov_msg), GFP_NOFS);
	if(!buf){
		rc = -ENOMEM;
		dput(dentry);
		goto out;
	}
	rc = __vfs_getxattr(dentry, inode, XATTR_NAME_PROVENANCE, buf, sizeof(union prov_msg));
	dput(dentry);
	if(rc<0){
		if(rc!=-ENODATA){
			printk(KERN_ERR "Provenance get xattr returned %u", rc);
			kfree(buf);
			goto free_buf;
		}else{
			rc = 0;
			goto initialised;
		}
	}
	memcpy(prov_msg(prov), buf, sizeof(union prov_msg));
	rc = 0;
initialised:
	prov->initialised=true;
free_buf:
	kfree(buf);
out:
	//spin_unlock(prov_lock(prov));
	return rc;
}

static inline struct provenance* inode_provenance(struct inode *inode){
	inode_init_provenance(inode, NULL);
	return inode->i_provenance;
}

static inline struct provenance *dentry_provenance(struct dentry *dentry)
{
	struct inode *inode = d_backing_inode(dentry);

	if (inode == NULL)
		return NULL;
	return inode_provenance(inode);
}

static inline struct provenance *file_provenance(struct file *file)
{
	struct inode *inode = file_inode(file);

	if (inode == NULL)
		return NULL;
	return inode_provenance(inode);
}
#endif
