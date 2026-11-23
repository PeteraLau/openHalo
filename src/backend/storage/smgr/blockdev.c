/* Local block-device storage for a dedicated teaching tablespace. */
#include "postgres.h"

#ifdef __linux__
#include <linux/fs.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "access/transam.h"
#include "access/xlog.h"
#include "catalog/pg_tablespace_d.h"
#include "miscadmin.h"
#include "storage/blockdev.h"
#include "storage/fd.h"

#define DEVICE_PATH "openhalo.block"
#define MAGIC "OHBLK8K1"
#define EXTENT_BLOCKS 128
#define DIR_SLOTS 15

/* Each descriptor occupies a sector. Offset zero is the format marker. */
typedef struct DeviceSlot
{
	RelFileNode node;
	uint32 forknum;
	uint32 nblocks;
	char padding[512 - sizeof(RelFileNode) - 2 * sizeof(uint32)];
} DeviceSlot;

typedef struct DeviceDirectory
{
	char header[512];
	DeviceSlot slot[DIR_SLOTS];
} DeviceDirectory;

typedef enum DeviceOp
{
	BD_EXISTS, BD_CREATE, BD_DROP, BD_SIZE, BD_READ,
	BD_WRITE, BD_EXTEND, BD_TRUNCATE, BD_SYNC
} DeviceOp;

static Oid device_space = InvalidOid;

void
bdinit(void)
{
	FILE *fp;
	unsigned int oid;
	char extra;
	int matched;

	StaticAssertStmt(sizeof(DeviceDirectory) == 8192, "unexpected device layout");
	fp = AllocateFile("openhalo_block.oid", "r");
	if (!fp)
	{
		if (errno == ENOENT)
			return;
		elog(FATAL, "could not open openhalo_block.oid: %m");
	}
	matched = fscanf(fp, "%u %c", &oid, &extra);
	FreeFile(fp);
	if (matched != 1 || oid < FirstNormalObjectId || BLCKSZ != 8192)
		elog(FATAL, "block storage requires a user tablespace OID and 8 KiB pages");
	device_space = oid;
}

bool
bdhandles(Oid tablespace, BackendId backend)
{
	return OidIsValid(device_space) && tablespace == device_space &&
		backend == InvalidBackendId;
}

void
bdopen(SMgrRelation rel)
{
	/* Like mdopen(), defer opening the physical store until the first I/O. */
	rel->blockdev_file = -1;
}

void
bdclose(SMgrRelation rel, ForkNumber forknum)
{
	/* All forks of this relation share one virtual file descriptor. */
	if (rel->blockdev_file >= 0)
	{
		FileClose(rel->blockdev_file);
		rel->blockdev_file = -1;
	}
}

static void
transfer(int fd, void *buf, size_t len, off_t pos, bool writing)
{
	while (len > 0)
	{
		ssize_t n = writing ? pg_pwrite(fd, buf, len, pos) :
			pg_pread(fd, buf, len, pos);

		if (n < 0 && errno == EINTR)
		{
			CHECK_FOR_INTERRUPTS();
			continue;
		}
		if (n < 0)
			elog(ERROR, "block device I/O at offset %lld failed: %m", (long long) pos);
		if (n == 0)
			elog(ERROR, "short I/O on block device at offset %lld", (long long) pos);
		buf = (char *) buf + n;
		pos += n;
		len -= n;
	}
}

static BlockNumber
access_slot(int fd, RelFileNode node, ForkNumber forknum, DeviceOp op,
			BlockNumber block, char *buffer, bool redo)
{
	DeviceDirectory dir;
	uint64 bytes;
	int slots;
	int i;
	int vacant = -1;
	DeviceSlot *entry;
	off_t base;
	static const char zero_page[BLCKSZ] = {0};

	if (ioctl(fd, BLKGETSIZE64, &bytes) < 0)
		elog(ERROR, "cannot determine size of local block device: %m");
	transfer(fd, &dir, sizeof(dir), 0, false);
	if (memcmp(dir.header, MAGIC, sizeof(MAGIC) - 1) != 0)
		elog(ERROR, "unrecognized block device format");
	slots = Min((bytes - sizeof(dir)) / (EXTENT_BLOCKS * BLCKSZ), DIR_SLOTS);
	for (i = 0; i < slots; i++)
	{
		entry = &dir.slot[i];
		if (!OidIsValid(entry->node.relNode) && vacant < 0)
			vacant = i;
		if (RelFileNodeEquals(entry->node, node) && entry->forknum == forknum)
			break;
	}
	if (op == BD_EXISTS)
		return i < slots;
	if (i == slots)
	{
		if (op == BD_DROP)
			return 0;
		if (op != BD_CREATE)
			elog(ERROR, "missing block-device relation %u fork %d", node.relNode, forknum);
		if (vacant < 0)
			ereport(ERROR, (errcode(ERRCODE_DISK_FULL), errmsg("no free block-device extent")));
		i = vacant;
	}
	entry = &dir.slot[i];
	if (entry->nblocks > EXTENT_BLOCKS)
		elog(ERROR, "invalid extent length in block device directory");
	base = sizeof(dir) + (off_t) i * EXTENT_BLOCKS * BLCKSZ;

	switch (op)
	{
		case BD_CREATE:
			if (OidIsValid(entry->node.relNode))
			{
				if (redo || InRecovery)
					return 0;
				elog(ERROR, "block-device relation fork already exists");
			}
			MemSet(entry, 0, sizeof(*entry));
			entry->node = node;
			entry->forknum = forknum;
			break;
		case BD_DROP:
			MemSet(entry, 0, sizeof(*entry));
			break;
		case BD_SIZE:
			return entry->nblocks;
		case BD_READ:
		case BD_WRITE:
			if (block >= entry->nblocks)
				elog(ERROR, "block %u past end of relation %u", block, node.relNode);
			transfer(fd, buffer, BLCKSZ, base + (off_t) block * BLCKSZ, op == BD_WRITE);
			return 0;
		case BD_EXTEND:
			if (block >= EXTENT_BLOCKS)
				ereport(ERROR, (errcode(ERRCODE_DISK_FULL),
					errmsg("block-device relation fork exceeds %d pages", EXTENT_BLOCKS)));
			for (BlockNumber n = entry->nblocks; n < block; n++)
				transfer(fd, (void *) zero_page, BLCKSZ, base + (off_t) n * BLCKSZ, true);
			transfer(fd, buffer, BLCKSZ, base + (off_t) block * BLCKSZ, true);
			entry->nblocks = Max(entry->nblocks, block + 1);
			break;
		case BD_TRUNCATE:
			if (block > entry->nblocks && !InRecovery)
				elog(ERROR, "cannot enlarge a block-device relation by truncation");
			entry->nblocks = Min(entry->nblocks, block);
			break;
		case BD_SYNC:
			if (pg_fsync(fd) < 0)
				elog(ERROR, "could not sync local block device: %m");
			return 0;
		default:
			elog(ERROR, "unknown block-device operation");
	}
	/* O_DSYNC orders the new data before the directory exposes it. */
	transfer(fd, entry, sizeof(*entry), offsetof(DeviceDirectory, slot) +
			 (off_t) i * sizeof(*entry), true);
	return 0;
}

static BlockNumber
device_io(SMgrRelation rel, ForkNumber forknum, DeviceOp op,
		  BlockNumber block, char *buf, bool redo)
{
	int fd;
	BlockNumber answer;
	int lock = (op == BD_READ || op == BD_SIZE || op == BD_EXISTS) ? LOCK_SH : LOCK_EX;

	if (rel->blockdev_file < 0)
	{
		rel->blockdev_file = PathNameOpenFile(DEVICE_PATH, O_RDWR | O_DSYNC);
		if (rel->blockdev_file < 0)
			elog(ERROR, "could not open %s: %m", DEVICE_PATH);
	}
	fd = FileGetRawDesc(rel->blockdev_file);
	if (fd < 0)
		elog(ERROR, "could not reopen %s: %m", DEVICE_PATH);
	while (flock(fd, lock) < 0)
	{
		if (errno != EINTR)
			elog(ERROR, "could not lock local block device: %m");
		CHECK_FOR_INTERRUPTS();
	}
	PG_TRY();
	{
		answer = access_slot(fd, rel->smgr_rnode.node, forknum, op, block, buf, redo);
	}
	PG_FINALLY();
	{
		if (flock(fd, LOCK_UN) < 0)
			elog(PANIC, "could not release local block device lock: %m");
	}
	PG_END_TRY();
	return answer;
}

void
bdcreate(SMgrRelation rel, ForkNumber f, bool redo)
{
	device_io(rel, f, BD_CREATE, 0, NULL, redo);
}

bool
bdexists(SMgrRelation rel, ForkNumber f)
{
	return device_io(rel, f, BD_EXISTS, 0, NULL, false) != 0;
}

void
bdunlink(RelFileNodeBackend node, ForkNumber f, bool redo)
{
	SMgrRelation rel = smgropen(node.node, node.backend);
	MemoryContext old = CurrentMemoryContext;

	/* Post-commit cleanup may warn but must not abort the transaction. */
	PG_TRY();
	{
		device_io(rel, f, BD_DROP, 0, NULL, redo);
	}
	PG_CATCH();
	{
		ErrorData *err;

		MemoryContextSwitchTo(old);
		err = CopyErrorData();
		FlushErrorState();
		elog(WARNING, "could not remove block-device fork: %s", err->message);
		FreeErrorData(err);
	}
	PG_END_TRY();
}

void
bdextend(SMgrRelation rel, ForkNumber f, BlockNumber b, char *buf, bool skipFsync)
{
	device_io(rel, f, BD_EXTEND, b, buf, false);
}

bool
bdprefetch(SMgrRelation rel, ForkNumber f, BlockNumber b)
{
	/* Prefetch is a hint; the caller will issue a normal read. */
	return bdexists(rel, f);
}

void
bdread(SMgrRelation rel, ForkNumber f, BlockNumber b, char *buf)
{
	device_io(rel, f, BD_READ, b, buf, false);
}

void
bdwrite(SMgrRelation rel, ForkNumber f, BlockNumber b, char *buf, bool skipFsync)
{
	device_io(rel, f, BD_WRITE, b, buf, false);
}

void
bdwriteback(SMgrRelation rel, ForkNumber f, BlockNumber b, BlockNumber count)
{
	/* Every write to the device has already completed synchronously. */
}

BlockNumber
bdnblocks(SMgrRelation rel, ForkNumber f)
{
	return device_io(rel, f, BD_SIZE, 0, NULL, false);
}

void
bdtruncate(SMgrRelation rel, ForkNumber f, BlockNumber old, BlockNumber n)
{
	device_io(rel, f, BD_TRUNCATE, n, NULL, false);
}

void
bdimmedsync(SMgrRelation rel, ForkNumber f)
{
	device_io(rel, f, BD_SYNC, 0, NULL, false);
}
#endif /* __linux__ */
