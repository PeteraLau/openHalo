/* Dedicated local block-device storage manager. */
#ifndef BLOCKDEV_H
#define BLOCKDEV_H

#include "storage/smgr.h"

extern bool bdhandles(Oid tablespace, BackendId backend);
extern void bdinit(void);
extern void bdopen(SMgrRelation reln);
extern void bdclose(SMgrRelation reln, ForkNumber forknum);
extern void bdcreate(SMgrRelation reln, ForkNumber forknum, bool isRedo);
extern bool bdexists(SMgrRelation reln, ForkNumber forknum);
extern void bdunlink(RelFileNodeBackend rnode, ForkNumber forknum, bool isRedo);
extern void bdextend(SMgrRelation reln, ForkNumber forknum,
					 BlockNumber blocknum, char *buffer, bool skipFsync);
extern bool bdprefetch(SMgrRelation reln, ForkNumber forknum,
					   BlockNumber blocknum);
extern void bdread(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
				   char *buffer);
extern void bdwrite(SMgrRelation reln, ForkNumber forknum,
					BlockNumber blocknum, char *buffer, bool skipFsync);
extern void bdwriteback(SMgrRelation reln, ForkNumber forknum,
						BlockNumber blocknum, BlockNumber nblocks);
extern BlockNumber bdnblocks(SMgrRelation reln, ForkNumber forknum);
extern void bdtruncate(SMgrRelation reln, ForkNumber forknum,
					   BlockNumber old_blocks, BlockNumber nblocks);
extern void bdimmedsync(SMgrRelation reln, ForkNumber forknum);

#endif
