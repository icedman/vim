/* vi:set ts=8 sts=4 sw=4 noet:
 *
 * VIM - Vi IMproved	by Bram Moolenaar
 *
 * Do ":help uganda"  in Vim to read copying and usage conditions.
 * Do ":help credits" in Vim to see a list of people who contributed.
 * See README.txt for an overview of the Vim source code.
 */

/*
 * buffer_snapshot.c: immutable, read-only buffer snapshots.
 *
 * This is the Phase 1 mirror implementation.  Snapshot construction and
 * reference management are main-thread-only until Vim has a portable atomic
 * reference-count implementation.  Queries touch only immutable memory.
 */

#include "vim.h"

#define SNAPSHOT_LEAF_TARGET 4096

#if defined(MSWIN)
typedef volatile LONG snapshot_ref_T;
# define SNAPSHOT_ATOMIC_REFS
#elif defined(__clang__) || (defined(__GNUC__) \
	&& (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 7)))
typedef int snapshot_ref_T;
# define SNAPSHOT_ATOMIC_REFS
#else
typedef int snapshot_ref_T;
#endif

typedef struct
{
    linenr_T	sl_first;
    linenr_T	sl_count;
    size_t	sl_data_size;
    size_t	*sl_offsets;
    colnr_T	*sl_lengths;
    char_u	*sl_data;
} snapshot_leaf_T;

struct buf_snapshot
{
    snapshot_ref_T	bs_refcount;
    varnumber_T		bs_revision;
    linenr_T		bs_line_count;
    int			bs_leaf_count;
    snapshot_leaf_T	**bs_leaves;
    size_t		bs_allocated;
};

/*
 * Free snapshot-owned memory without reading Vim globals on worker threads.
 */
    static void
snapshot_mem_free(void *ptr)
{
#if defined(SNAPSHOT_ATOMIC_REFS) && !defined(MEM_PROFILE)
    free(ptr);
#else
    vim_free(ptr);
#endif
}

/*
 * Free a snapshot leaf.
 */
    static void
snapshot_leaf_free(snapshot_leaf_T *leaf)
{
    if (leaf == NULL)
	return;
    snapshot_mem_free(leaf->sl_offsets);
    snapshot_mem_free(leaf->sl_lengths);
    snapshot_mem_free(leaf->sl_data);
    snapshot_mem_free(leaf);
}

/*
 * Free an unreferenced snapshot.
 */
    static void
snapshot_free(buf_snapshot_T *snapshot)
{
    int i;

    if (snapshot == NULL)
	return;
    for (i = 0; i < snapshot->bs_leaf_count; ++i)
	snapshot_leaf_free(snapshot->bs_leaves[i]);
    snapshot_mem_free(snapshot->bs_leaves);
    snapshot_mem_free(snapshot);
}

/*
 * Build one immutable leaf starting at "first".  Set "next" to the first line
 * not included in the leaf.
 */
    static snapshot_leaf_T *
snapshot_leaf_build(buf_T *buf, linenr_T first, linenr_T line_count,
	linenr_T *next)
{
    snapshot_leaf_T	*leaf;
    linenr_T		lnum;
    linenr_T		count = 0;
    size_t		data_size = 0;
    size_t		offset = 0;

    for (lnum = first; lnum <= line_count; ++lnum)
    {
	colnr_T len = buf_line_len(buf, lnum);
	size_t needed = (size_t)len + 1;

	if (count > 0 && data_size + needed > SNAPSHOT_LEAF_TARGET)
	    break;
	if (data_size + needed < data_size)
	    return NULL;
	data_size += needed;
	++count;
    }

    leaf = alloc_clear_id(sizeof(snapshot_leaf_T), aid_buf_snapshot);
    if (leaf == NULL)
	return NULL;
    leaf->sl_offsets = alloc_clear_id(
				(size_t)count * sizeof(size_t), aid_buf_snapshot);
    leaf->sl_lengths = alloc_clear_id(
				(size_t)count * sizeof(colnr_T), aid_buf_snapshot);
    leaf->sl_data = alloc_clear_id(data_size, aid_buf_snapshot);
    if (leaf->sl_offsets == NULL || leaf->sl_lengths == NULL
							|| leaf->sl_data == NULL)
    {
	snapshot_leaf_free(leaf);
	return NULL;
    }

    leaf->sl_first = first;
    leaf->sl_count = count;
    leaf->sl_data_size = data_size;
    for (lnum = first; lnum < first + count; ++lnum)
    {
	linenr_T index = lnum - first;
	colnr_T len = buf_line_len(buf, lnum);
	char_u *line = buf_line(buf, lnum, FALSE);

	leaf->sl_offsets[index] = offset;
	leaf->sl_lengths[index] = len;
	mch_memmove(leaf->sl_data + offset, line, (size_t)len + 1);
	offset += (size_t)len + 1;
    }

    *next = first + count;
    return leaf;
}

/*
 * Capture the current text of "buf" in immutable storage.
 * Must be called on the main editor thread.
 */
    buf_snapshot_T *
buf_snapshot_acquire(buf_T *buf)
{
    buf_snapshot_T	*snapshot;
    linenr_T		lnum;
    linenr_T		line_count;
    int			leaf_count = 0;

    if (buf == NULL || !buf_is_loaded(buf))
	return NULL;

    line_count = buf_line_count(buf);
    snapshot = alloc_clear_id(sizeof(buf_snapshot_T), aid_buf_snapshot);
    if (snapshot == NULL)
	return NULL;
    snapshot->bs_leaves = alloc_clear_id(
		(size_t)line_count * sizeof(snapshot_leaf_T *), aid_buf_snapshot);
    if (snapshot->bs_leaves == NULL)
    {
	snapshot_mem_free(snapshot);
	return NULL;
    }

    lnum = 1;
    while (lnum <= line_count)
    {
	snapshot_leaf_T *leaf = snapshot_leaf_build(buf, lnum, line_count,
								&lnum);

	if (leaf == NULL)
	{
	    snapshot->bs_leaf_count = leaf_count;
	    snapshot_free(snapshot);
	    return NULL;
	}
	snapshot->bs_leaves[leaf_count++] = leaf;
    }

    snapshot->bs_refcount = 1;
    snapshot->bs_revision = buf->b_snapshot_revision;
    snapshot->bs_line_count = line_count;
    snapshot->bs_leaf_count = leaf_count;
    snapshot->bs_allocated = sizeof(*snapshot)
			+ (size_t)line_count * sizeof(snapshot_leaf_T *);
    for (int i = 0; i < leaf_count; ++i)
	snapshot->bs_allocated += sizeof(snapshot_leaf_T)
		+ (size_t)snapshot->bs_leaves[i]->sl_count
					* (sizeof(size_t) + sizeof(colnr_T))
		+ snapshot->bs_leaves[i]->sl_data_size;
    return snapshot;
}

/*
 * Return TRUE when snapshot references may cross threads in this build.
 * MEM_PROFILE makes Vim's allocation accounting unsuitable for worker-thread
 * final release, even when atomic counters are available.
 */
    int
buf_snapshot_threadsafe(void)
{
#if defined(SNAPSHOT_ATOMIC_REFS) && !defined(MEM_PROFILE)
    return TRUE;
#else
    return FALSE;
#endif
}

/*
 * Add a reference to "snapshot".
 */
    void
buf_snapshot_ref(buf_snapshot_T *snapshot)
{
    if (snapshot == NULL)
	return;
#if defined(MSWIN)
    (void)InterlockedIncrement(&snapshot->bs_refcount);
#elif defined(SNAPSHOT_ATOMIC_REFS)
    (void)__atomic_add_fetch(&snapshot->bs_refcount, 1, __ATOMIC_RELAXED);
#else
    ++snapshot->bs_refcount;
#endif
}

/*
 * Drop a reference to "snapshot" and free it after the final release.
 */
    void
buf_snapshot_unref(buf_snapshot_T *snapshot)
{
    int is_last;

    if (snapshot == NULL)
	return;
#if defined(MSWIN)
    is_last = InterlockedDecrement(&snapshot->bs_refcount) == 0;
#elif defined(SNAPSHOT_ATOMIC_REFS)
    is_last = __atomic_sub_fetch(&snapshot->bs_refcount, 1,
							__ATOMIC_ACQ_REL) == 0;
#else
    is_last = --snapshot->bs_refcount == 0;
#endif
    if (is_last)
	snapshot_free(snapshot);
}

/*
 * Return the immutable text revision captured by "snapshot".
 */
    varnumber_T
buf_snapshot_tick(const buf_snapshot_T *snapshot)
{
    return snapshot == NULL ? 0 : snapshot->bs_revision;
}

/*
 * Return the number of lines in "snapshot".
 */
    linenr_T
buf_snapshot_line_count(const buf_snapshot_T *snapshot)
{
    return snapshot == NULL ? 0 : snapshot->bs_line_count;
}

/*
 * Find the leaf containing "lnum".
 */
    static snapshot_leaf_T *
snapshot_find_leaf(const buf_snapshot_T *snapshot, linenr_T lnum)
{
    int low = 0;
    int high = snapshot->bs_leaf_count - 1;

    while (low <= high)
    {
	int middle = low + (high - low) / 2;
	snapshot_leaf_T *leaf = snapshot->bs_leaves[middle];

	if (lnum < leaf->sl_first)
	    high = middle - 1;
	else if (lnum >= leaf->sl_first + leaf->sl_count)
	    low = middle + 1;
	else
	    return leaf;
    }
    return NULL;
}

/*
 * Return an immutable line.  The pointer remains valid until the caller drops
 * its snapshot reference.
 */
    const char_u *
buf_snapshot_line(const buf_snapshot_T *snapshot, linenr_T lnum)
{
    snapshot_leaf_T *leaf;
    linenr_T index;

    if (snapshot == NULL || lnum < 1 || lnum > snapshot->bs_line_count)
	return NULL;
    leaf = snapshot_find_leaf(snapshot, lnum);
    if (leaf == NULL)
	return NULL;
    index = lnum - leaf->sl_first;
    return leaf->sl_data + leaf->sl_offsets[index];
}

/*
 * Return a snapshot line length, excluding the terminating NUL.
 */
    colnr_T
buf_snapshot_line_len(const buf_snapshot_T *snapshot, linenr_T lnum)
{
    snapshot_leaf_T *leaf;

    if (snapshot == NULL || lnum < 1 || lnum > snapshot->bs_line_count)
	return 0;
    leaf = snapshot_find_leaf(snapshot, lnum);
    return leaf == NULL ? 0 : leaf->sl_lengths[lnum - leaf->sl_first];
}

/*
 * Return bytes allocated by this Phase 1 snapshot mirror.
 */
    size_t
buf_snapshot_size(const buf_snapshot_T *snapshot)
{
    return snapshot == NULL ? 0 : snapshot->bs_allocated;
}
