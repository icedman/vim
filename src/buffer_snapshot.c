/* vi:set ts=8 sts=4 sw=4 noet:
 *
 * VIM - Vi IMproved	by Bram Moolenaar
 *
 * Do ":help uganda"  in Vim to read copying and usage conditions.
 * Do ":help credits" in Vim to see a list of people who contributed.
 * See README.txt for an overview of the Vim source code.
 */

/*
 * buffer_snapshot.c: persistent immutable buffer snapshots.
 *
 * A snapshot owns one reference to an AVL rope.  Branch nodes contain only
 * relative subtree information, so an edit can share every node outside the
 * path to the edited leaf.  Leaves store their offset table, length table and
 * text in one packed slab.  Published revisions and all nodes reachable from
 * them are immutable.
 */

#include "vim.h"

#ifndef SNAPSHOT_LEAF_TARGET
# define SNAPSHOT_LEAF_TARGET 4096
#endif

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

typedef struct snapshot_node snapshot_node_T;

struct snapshot_node
{
    snapshot_ref_T	sn_refcount;
    linenr_T		sn_line_count;
    size_t		sn_allocated;
    unsigned short	sn_height;
    unsigned char	sn_is_leaf;
    union
    {
	struct
	{
	    snapshot_node_T *left;
	    snapshot_node_T *right;
	} branch;
	struct
	{
	    size_t	data_size;
	    void	*slab;
	} leaf;
    } sn_u;
};

struct buf_snapshot
{
    snapshot_ref_T	bs_refcount;
    varnumber_T		bs_revision;
    varnumber_T		bs_base_revision;
    linenr_T		bs_line_count;
    snapshot_node_T	*bs_root;
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

    static void
snapshot_node_ref(snapshot_node_T *node)
{
    if (node == NULL)
	return;
#if defined(MSWIN)
    (void)InterlockedIncrement(&node->sn_refcount);
#elif defined(SNAPSHOT_ATOMIC_REFS)
    (void)__atomic_add_fetch(&node->sn_refcount, 1, __ATOMIC_RELAXED);
#else
    ++node->sn_refcount;
#endif
}

    static void
snapshot_node_unref(snapshot_node_T *node)
{
    int is_last;

    if (node == NULL)
	return;
#if defined(MSWIN)
    is_last = InterlockedDecrement(&node->sn_refcount) == 0;
#elif defined(SNAPSHOT_ATOMIC_REFS)
    is_last = __atomic_sub_fetch(&node->sn_refcount, 1,
							__ATOMIC_ACQ_REL) == 0;
#else
    is_last = --node->sn_refcount == 0;
#endif
    if (!is_last)
	return;
    if (node->sn_is_leaf)
	snapshot_mem_free(node->sn_u.leaf.slab);
    else
    {
	snapshot_node_unref(node->sn_u.branch.left);
	snapshot_node_unref(node->sn_u.branch.right);
    }
    snapshot_mem_free(node);
}

    static size_t *
snapshot_leaf_offsets(snapshot_node_T *leaf)
{
    return (size_t *)leaf->sn_u.leaf.slab;
}

    static colnr_T *
snapshot_leaf_lengths(snapshot_node_T *leaf)
{
    return (colnr_T *)(snapshot_leaf_offsets(leaf) + leaf->sn_line_count);
}

    static char_u *
snapshot_leaf_data(snapshot_node_T *leaf)
{
    return (char_u *)(snapshot_leaf_lengths(leaf) + leaf->sn_line_count);
}

/*
 * Allocate a packed leaf.  "lines" and "lengths" describe "count" strings;
 * the strings need not be NUL terminated by their owner.
 */
    static snapshot_node_T *
snapshot_leaf_new(const char_u **lines, const colnr_T *lengths, linenr_T count)
{
    snapshot_node_T	*leaf;
    size_t		table_size;
    size_t		data_size = 0;
    size_t		slab_size;
    size_t		offset = 0;
    linenr_T		i;

    if (count < 1 || (size_t)count > SIZE_MAX / sizeof(size_t)
	    || (size_t)count > SIZE_MAX / sizeof(colnr_T))
	return NULL;
    table_size = (size_t)count * (sizeof(size_t) + sizeof(colnr_T));
    for (i = 0; i < count; ++i)
    {
	size_t needed;

	if (lengths[i] < 0)
	    return NULL;
	needed = (size_t)lengths[i] + 1;
	if (needed == 0 || data_size > SIZE_MAX - needed)
	    return NULL;
	data_size += needed;
    }
    if (table_size > SIZE_MAX - data_size)
	return NULL;
    slab_size = table_size + data_size;

    leaf = alloc_clear_id(sizeof(*leaf), aid_buf_snapshot);
    if (leaf == NULL)
	return NULL;
    leaf->sn_u.leaf.slab = alloc_clear_id(slab_size, aid_buf_snapshot);
    if (leaf->sn_u.leaf.slab == NULL)
    {
	snapshot_mem_free(leaf);
	return NULL;
    }
    leaf->sn_refcount = 1;
    leaf->sn_line_count = count;
    leaf->sn_allocated = sizeof(*leaf) + slab_size;
    leaf->sn_height = 1;
    leaf->sn_is_leaf = TRUE;
    leaf->sn_u.leaf.data_size = data_size;
    for (i = 0; i < count; ++i)
    {
	snapshot_leaf_offsets(leaf)[i] = offset;
	snapshot_leaf_lengths(leaf)[i] = lengths[i];
	mch_memmove(snapshot_leaf_data(leaf) + offset, lines[i],
						(size_t)lengths[i]);
	snapshot_leaf_data(leaf)[offset + lengths[i]] = NUL;
	offset += (size_t)lengths[i] + 1;
    }
    return leaf;
}

    static snapshot_node_T *
snapshot_branch_new(snapshot_node_T *left, snapshot_node_T *right)
{
    snapshot_node_T *node;

    if (left == NULL || right == NULL
	    || left->sn_line_count > MAXLNUM - right->sn_line_count)
	return NULL;
    node = alloc_clear_id(sizeof(*node), aid_buf_snapshot);
    if (node == NULL)
	return NULL;
    node->sn_refcount = 1;
    node->sn_line_count = left->sn_line_count + right->sn_line_count;
    node->sn_allocated = sizeof(*node) + left->sn_allocated
							+ right->sn_allocated;
    node->sn_height = (unsigned short)(
		left->sn_height > right->sn_height ? left->sn_height + 1
							: right->sn_height + 1);
    node->sn_u.branch.left = left;
    node->sn_u.branch.right = right;
    snapshot_node_ref(left);
    snapshot_node_ref(right);
    return node;
}

/*
 * Return a balanced tree containing "left" followed by "right".  Arguments
 * are borrowed; the returned tree has one owned reference.
 */
    static snapshot_node_T *
snapshot_balance(snapshot_node_T *left, snapshot_node_T *right)
{
    snapshot_node_T *a;
    snapshot_node_T *b;
    snapshot_node_T *result;

    if (left == NULL)
    {
	snapshot_node_ref(right);
	return right;
    }
    if (right == NULL)
    {
	snapshot_node_ref(left);
	return left;
    }
    if (left->sn_height > right->sn_height + 1)
    {
	snapshot_node_T *ll = left->sn_u.branch.left;
	snapshot_node_T *lr = left->sn_u.branch.right;

	if (ll->sn_height >= lr->sn_height)
	{
	    b = snapshot_branch_new(lr, right);
	    if (b == NULL)
		return NULL;
	    result = snapshot_branch_new(ll, b);
	    snapshot_node_unref(b);
	    return result;
	}
	a = snapshot_branch_new(ll, lr->sn_u.branch.left);
	if (a == NULL)
	    return NULL;
	b = snapshot_branch_new(lr->sn_u.branch.right, right);
	if (b == NULL)
	{
	    snapshot_node_unref(a);
	    return NULL;
	}
	result = snapshot_branch_new(a, b);
	snapshot_node_unref(a);
	snapshot_node_unref(b);
	return result;
    }
    if (right->sn_height > left->sn_height + 1)
    {
	snapshot_node_T *rl = right->sn_u.branch.left;
	snapshot_node_T *rr = right->sn_u.branch.right;

	if (rr->sn_height >= rl->sn_height)
	{
	    a = snapshot_branch_new(left, rl);
	    if (a == NULL)
		return NULL;
	    result = snapshot_branch_new(a, rr);
	    snapshot_node_unref(a);
	    return result;
	}
	a = snapshot_branch_new(left, rl->sn_u.branch.left);
	if (a == NULL)
	    return NULL;
	b = snapshot_branch_new(rl->sn_u.branch.right, rr);
	if (b == NULL)
	{
	    snapshot_node_unref(a);
	    return NULL;
	}
	result = snapshot_branch_new(a, b);
	snapshot_node_unref(a);
	snapshot_node_unref(b);
	return result;
    }
    return snapshot_branch_new(left, right);
}

/*
 * Split an oversized multi-line leaf and return one owned balanced subtree.
 * The input leaf is borrowed.
 */
    static snapshot_node_T *
snapshot_leaf_split(snapshot_node_T *leaf)
{
    const char_u **lines;
    colnr_T *lengths;
    snapshot_node_T *left_leaf;
    snapshot_node_T *right_leaf;
    snapshot_node_T *left;
    snapshot_node_T *right;
    snapshot_node_T *result;
    linenr_T middle;
    linenr_T i;

    if (leaf->sn_u.leaf.data_size <= SNAPSHOT_LEAF_TARGET
						|| leaf->sn_line_count == 1)
    {
	snapshot_node_ref(leaf);
	return leaf;
    }
    lines = alloc_clear_id((size_t)leaf->sn_line_count * sizeof(*lines),
							aid_buf_snapshot);
    lengths = alloc_clear_id((size_t)leaf->sn_line_count * sizeof(*lengths),
							aid_buf_snapshot);
    if (lines == NULL || lengths == NULL)
    {
	vim_free(lines);
	vim_free(lengths);
	return NULL;
    }
    for (i = 0; i < leaf->sn_line_count; ++i)
    {
	lines[i] = snapshot_leaf_data(leaf) + snapshot_leaf_offsets(leaf)[i];
	lengths[i] = snapshot_leaf_lengths(leaf)[i];
    }
    {
	size_t half = leaf->sn_u.leaf.data_size / 2;
	size_t used = 0;

	middle = leaf->sn_line_count - 1;
	for (i = 0; i < leaf->sn_line_count - 1; ++i)
	{
	    used += (size_t)lengths[i] + 1;
	    if (used >= half)
	    {
		middle = i + 1;
		break;
	    }
	}
    }
    left_leaf = snapshot_leaf_new(lines, lengths, middle);
    right_leaf = snapshot_leaf_new(lines + middle, lengths + middle,
						leaf->sn_line_count - middle);
    vim_free(lines);
    vim_free(lengths);
    if (left_leaf == NULL || right_leaf == NULL)
    {
	snapshot_node_unref(left_leaf);
	snapshot_node_unref(right_leaf);
	return NULL;
    }
    left = snapshot_leaf_split(left_leaf);
    right = snapshot_leaf_split(right_leaf);
    snapshot_node_unref(left_leaf);
    snapshot_node_unref(right_leaf);
    if (left == NULL || right == NULL)
    {
	snapshot_node_unref(left);
	snapshot_node_unref(right);
	return NULL;
    }
    result = snapshot_balance(left, right);
    snapshot_node_unref(left);
    snapshot_node_unref(right);
    return result;
}

/* Build a replacement for one leaf, inserting, replacing, or deleting. */
    static snapshot_node_T *
snapshot_leaf_edit(snapshot_node_T *leaf, linenr_T index,
	const char_u *line, colnr_T len, int operation)
{
    const char_u	**lines;
    colnr_T	*lengths;
    snapshot_node_T *result;
    linenr_T	new_count = leaf->sn_line_count + (operation == 1 ? 1 : 0)
						- (operation == -1 ? 1 : 0);
    linenr_T	i;
    linenr_T	j = 0;

    if (new_count == 0)
	return NULL;
    lines = alloc_clear_id((size_t)new_count * sizeof(*lines),
							aid_buf_snapshot);
    lengths = alloc_clear_id((size_t)new_count * sizeof(*lengths),
							aid_buf_snapshot);
    if (lines == NULL || lengths == NULL)
    {
	vim_free(lines);
	vim_free(lengths);
	return NULL;
    }
    for (i = 0; i < leaf->sn_line_count; ++i)
    {
	if (operation == 1 && i == index)
	{
	    lines[j] = line;
	    lengths[j++] = len;
	}
	if (operation == -1 && i == index)
	    continue;
	if (operation == 0 && i == index)
	{
	    lines[j] = line;
	    lengths[j++] = len;
	}
	else
	{
	    lines[j] = snapshot_leaf_data(leaf)
						+ snapshot_leaf_offsets(leaf)[i];
	    lengths[j++] = snapshot_leaf_lengths(leaf)[i];
	}
    }
    if (operation == 1 && index == leaf->sn_line_count)
    {
	lines[j] = line;
	lengths[j++] = len;
    }
    result = snapshot_leaf_new(lines, lengths, new_count);
    vim_free(lines);
    vim_free(lengths);
    if (result != NULL)
    {
	snapshot_node_T *split = snapshot_leaf_split(result);

	snapshot_node_unref(result);
	result = split;
    }
    return result;
}

/* operation: 1 inserts before index, 0 replaces index, -1 deletes index. */
    static snapshot_node_T *
snapshot_edit(snapshot_node_T *node, linenr_T index, const char_u *line,
	colnr_T len, int operation, int *failed)
{
    snapshot_node_T *changed;
    snapshot_node_T *result;
    linenr_T left_count;

    if (node->sn_is_leaf)
    {
	changed = snapshot_leaf_edit(node, index, line, len, operation);
	if (changed == NULL && !(operation == -1 && node->sn_line_count == 1))
	    *failed = TRUE;
	return changed;
    }
    left_count = node->sn_u.branch.left->sn_line_count;
    if (index < left_count)
    {
	changed = snapshot_edit(node->sn_u.branch.left, index, line, len,
							operation, failed);
	if (*failed)
	    return NULL;
	result = snapshot_balance(changed, node->sn_u.branch.right);
    }
    else
    {
	changed = snapshot_edit(node->sn_u.branch.right, index - left_count,
						line, len, operation, failed);
	if (*failed)
	    return NULL;
	result = snapshot_balance(node->sn_u.branch.left, changed);
    }
    snapshot_node_unref(changed);
    if (result == NULL)
	*failed = TRUE;
    return result;
}

/* Build one immutable leaf and set "next" to its first excluded line. */
    static snapshot_node_T *
snapshot_leaf_build(buf_T *buf, linenr_T first, linenr_T line_count,
	linenr_T *next)
{
    const char_u	**lines;
    colnr_T	*lengths;
    snapshot_node_T *leaf;
    linenr_T	lnum;
    linenr_T	count = 0;
    size_t	data_size = 0;

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
    lines = alloc_clear_id((size_t)count * sizeof(*lines),
							aid_buf_snapshot);
    lengths = alloc_clear_id((size_t)count * sizeof(*lengths),
							aid_buf_snapshot);
    if (lines == NULL || lengths == NULL)
    {
	vim_free(lines);
	vim_free(lengths);
	return NULL;
    }
    for (lnum = 0; lnum < count; ++lnum)
    {
	lengths[lnum] = buf_line_len(buf, first + lnum);
	lines[lnum] = alloc_clear_id((size_t)lengths[lnum] + 1,
							aid_buf_snapshot);
	if (lines[lnum] != NULL)
	    mch_memmove((char_u *)lines[lnum],
		buf_line(buf, first + lnum, FALSE), (size_t)lengths[lnum]);
	if (lines[lnum] == NULL)
	{
	    while (lnum > 0)
		vim_free((char_u *)lines[--lnum]);
	    vim_free(lines);
	    vim_free(lengths);
	    return NULL;
	}
    }
    leaf = snapshot_leaf_new(lines, lengths, count);
    for (lnum = 0; lnum < count; ++lnum)
	vim_free((char_u *)lines[lnum]);
    vim_free(lines);
    vim_free(lengths);
    if (leaf != NULL)
	*next = first + count;
    return leaf;
}

/* Append one leaf to an AVL tree.  Arguments are borrowed. */
    static snapshot_node_T *
snapshot_tree_append(snapshot_node_T *root, snapshot_node_T *leaf)
{
    snapshot_node_T *right;
    snapshot_node_T *result;

    if (root == NULL)
    {
	snapshot_node_ref(leaf);
	return leaf;
    }
    if (root->sn_is_leaf)
	return snapshot_balance(root, leaf);
    right = snapshot_tree_append(root->sn_u.branch.right, leaf);
    if (right == NULL)
	return NULL;
    result = snapshot_balance(root->sn_u.branch.left, right);
    snapshot_node_unref(right);
    return result;
}

/* Build an immutable AVL revision from the current text of "buf". */
    static buf_snapshot_T *
snapshot_build(buf_T *buf)
{
    buf_snapshot_T *snapshot;
    snapshot_node_T *root = NULL;
    linenr_T lnum;
    linenr_T line_count;

    if (buf == NULL || !buf_is_loaded(buf))
	return NULL;
    line_count = buf_line_count(buf);
    lnum = 1;
    while (lnum <= line_count)
    {
	snapshot_node_T *leaf = snapshot_leaf_build(buf, lnum, line_count, &lnum);
	snapshot_node_T *new_root;

	if (leaf == NULL)
	{
	    snapshot_node_unref(root);
	    return NULL;
	}
	new_root = snapshot_tree_append(root, leaf);
	snapshot_node_unref(root);
	snapshot_node_unref(leaf);
	if (new_root == NULL)
	    return NULL;
	root = new_root;
    }
    snapshot = alloc_clear_id(sizeof(*snapshot), aid_buf_snapshot);
    if (snapshot == NULL)
    {
	snapshot_node_unref(root);
	return NULL;
    }
    snapshot->bs_refcount = 1;
    snapshot->bs_revision = buf->b_snapshot_revision;
    snapshot->bs_base_revision = buf->b_snapshot_revision;
    snapshot->bs_line_count = line_count;
    snapshot->bs_root = root;
    snapshot->bs_allocated = sizeof(*snapshot) + root->sn_allocated;
    return snapshot;
}

    static void
snapshot_free(buf_snapshot_T *snapshot)
{
    if (snapshot == NULL)
	return;
    snapshot_node_unref(snapshot->bs_root);
    snapshot_mem_free(snapshot);
}

    buf_snapshot_T *
buf_snapshot_acquire(buf_T *buf)
{
    if (buf == NULL || !buf_is_loaded(buf))
	return NULL;
    if (buf->b_snapshot == NULL)
    {
	buf->b_snapshot = snapshot_build(buf);
	if (buf->b_snapshot == NULL)
	    return NULL;
    }
    buf_snapshot_ref(buf->b_snapshot);
    return buf->b_snapshot;
}

    void
buf_snapshot_clear(buf_T *buf)
{
    if (buf == NULL || buf->b_snapshot == NULL)
	return;
    buf_snapshot_unref(buf->b_snapshot);
    buf->b_snapshot = NULL;
}

    void
buf_snapshot_invalidate(buf_T *buf)
{
    if (buf == NULL || buf->b_snapshot == NULL)
	return;
    buf_snapshot_clear(buf);
    if (buf->b_snapshot_revision < VARNUM_MAX)
	++buf->b_snapshot_revision;
    else
	buf->b_snapshot_revision = 0;
}

    int
buf_snapshot_threadsafe(void)
{
#if defined(SNAPSHOT_ATOMIC_REFS) && !defined(MEM_PROFILE)
    return TRUE;
#else
    return FALSE;
#endif
}

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

    varnumber_T
buf_snapshot_tick(const buf_snapshot_T *snapshot)
{
    return snapshot == NULL ? 0 : snapshot->bs_revision;
}

    linenr_T
buf_snapshot_line_count(const buf_snapshot_T *snapshot)
{
    return snapshot == NULL ? 0 : snapshot->bs_line_count;
}

    static snapshot_node_T *
snapshot_find_leaf(const buf_snapshot_T *snapshot, linenr_T lnum,
	linenr_T *index)
{
    snapshot_node_T *node = snapshot->bs_root;
    linenr_T zero_based = lnum - 1;

    while (!node->sn_is_leaf)
    {
	linenr_T left_count = node->sn_u.branch.left->sn_line_count;

	if (zero_based < left_count)
	    node = node->sn_u.branch.left;
	else
	{
	    zero_based -= left_count;
	    node = node->sn_u.branch.right;
	}
    }
    *index = zero_based;
    return node;
}

    const char_u *
buf_snapshot_line(const buf_snapshot_T *snapshot, linenr_T lnum)
{
    snapshot_node_T *leaf;
    linenr_T index;

    if (snapshot == NULL || lnum < 1 || lnum > snapshot->bs_line_count)
	return NULL;
    leaf = snapshot_find_leaf(snapshot, lnum, &index);
    return snapshot_leaf_data(leaf) + snapshot_leaf_offsets(leaf)[index];
}

    colnr_T
buf_snapshot_line_len(const buf_snapshot_T *snapshot, linenr_T lnum)
{
    snapshot_node_T *leaf;
    linenr_T index;

    if (snapshot == NULL || lnum < 1 || lnum > snapshot->bs_line_count)
	return 0;
    leaf = snapshot_find_leaf(snapshot, lnum, &index);
    return snapshot_leaf_lengths(leaf)[index];
}

    size_t
buf_snapshot_size(const buf_snapshot_T *snapshot)
{
    return snapshot == NULL ? 0 : snapshot->bs_allocated;
}

    static void
snapshot_node_stats(snapshot_node_T *node, buf_snapshot_stats_T *stats)
{
    if (node->sn_is_leaf)
    {
	size_t table_size = (size_t)node->sn_line_count
					* (sizeof(size_t) + sizeof(colnr_T));

	++stats->bss_leaf_count;
	stats->bss_text_bytes += node->sn_u.leaf.data_size;
	stats->bss_table_bytes += table_size;
	if (stats->bss_largest_leaf < node->sn_u.leaf.data_size)
	    stats->bss_largest_leaf = node->sn_u.leaf.data_size;
	return;
    }
    ++stats->bss_branch_count;
    snapshot_node_stats(node->sn_u.branch.left, stats);
    snapshot_node_stats(node->sn_u.branch.right, stats);
}

/*
 * Return storage reachable from one revision.  Text byte counts include each
 * line's terminating NUL.  Shared nodes are counted once per queried revision.
 */
    void
buf_snapshot_stats(const buf_snapshot_T *snapshot, buf_snapshot_stats_T *stats)
{
    if (stats == NULL)
	return;
    vim_memset(stats, 0, sizeof(*stats));
    if (snapshot == NULL)
	return;
    stats->bss_reachable_bytes = snapshot->bs_allocated;
    snapshot_node_stats(snapshot->bs_root, stats);
}

    static int
snapshot_node_contains(snapshot_node_T *node, snapshot_node_T *wanted)
{
    if (node == wanted)
	return TRUE;
    if (node->sn_is_leaf)
	return FALSE;
    return snapshot_node_contains(node->sn_u.branch.left, wanted)
	|| snapshot_node_contains(node->sn_u.branch.right, wanted);
}

    static size_t
snapshot_node_shared_size(snapshot_node_T *node, snapshot_node_T *other)
{
    if (snapshot_node_contains(other, node))
	return node->sn_is_leaf ? node->sn_allocated : sizeof(*node);
    if (node->sn_is_leaf)
	return 0;
    return snapshot_node_shared_size(node->sn_u.branch.left, other)
	+ snapshot_node_shared_size(node->sn_u.branch.right, other);
}

/* Return physical node and leaf bytes shared by two revisions. */
    size_t
buf_snapshot_shared_size(
    const buf_snapshot_T *one,
    const buf_snapshot_T *two)
{
    if (one == NULL || two == NULL)
	return 0;
    return snapshot_node_shared_size(one->bs_root, two->bs_root);
}

    static varnumber_T
snapshot_next_revision(varnumber_T revision)
{
    return revision < VARNUM_MAX ? revision + 1 : 0;
}

    static buf_snapshot_T *
snapshot_prepare(buf_T *buf, linenr_T index, const char_u *line, colnr_T len,
	int operation)
{
    buf_snapshot_T *base;
    buf_snapshot_T *candidate;
    snapshot_node_T *root;
    int failed = FALSE;

    if (buf == NULL || !buf_is_loaded(buf) || len < 0
	    || (operation != -1 && line == NULL))
	return NULL;
    base = buf_snapshot_acquire(buf);
    if (base == NULL)
	return NULL;

    if (index < 0 || index > base->bs_line_count
	    || (operation != 1 && index == base->bs_line_count))
    {
	buf_snapshot_unref(base);
	return NULL;
    }
    if (operation == -1 && base->bs_line_count == 1)
    {
	static const char_u empty[] = "";
	root = snapshot_edit(base->bs_root, 0, empty, 0, 0, &failed);
    }
    else
	root = snapshot_edit(base->bs_root, index, line, len, operation, &failed);
    if (failed || root == NULL)
    {
	snapshot_node_unref(root);
	buf_snapshot_unref(base);
	return NULL;
    }
    candidate = alloc_clear_id(sizeof(*candidate), aid_buf_snapshot);
    if (candidate == NULL)
    {
	snapshot_node_unref(root);
	buf_snapshot_unref(base);
	return NULL;
    }
    candidate->bs_refcount = 1;
    candidate->bs_base_revision = base->bs_revision;
    candidate->bs_revision = snapshot_next_revision(base->bs_revision);
    candidate->bs_line_count = root->sn_line_count;
    candidate->bs_root = root;
    candidate->bs_allocated = sizeof(*candidate) + root->sn_allocated;
    buf_snapshot_unref(base);
    return candidate;
}

/*
 * Publish finalized text changes in the inclusive line range.  This is the
 * commit boundary for cached-line writers that bypass the mutation facade.
 */
    void
buf_snapshot_sync(buf_T *buf, linenr_T first, linenr_T last)
{
    buf_snapshot_T *base;
    buf_snapshot_T *candidate;
    snapshot_node_T *root;
    int changed = FALSE;
    linenr_T lnum;

    if (buf == NULL || buf->b_snapshot == NULL || !buf_is_loaded(buf))
	return;
    if (buf_snapshot_line_count(buf->b_snapshot) != buf_line_count(buf))
    {
	buf_snapshot_invalidate(buf);
	return;
    }
    if (first < 1)
	first = 1;
    if (last > buf_line_count(buf))
	last = buf_line_count(buf);
    if (first > last)
	return;

    base = buf->b_snapshot;
    root = base->bs_root;
    snapshot_node_ref(root);
    for (lnum = first; lnum <= last; ++lnum)
    {
	colnr_T len = buf_line_len(buf, lnum);
	char_u *line = buf_line(buf, lnum, FALSE);
	const char_u *oldline = buf_snapshot_line(base, lnum);
	int failed = FALSE;
	snapshot_node_T *new_root;

	if (len == buf_snapshot_line_len(base, lnum)
				&& STRNCMP(line, oldline, (size_t)len + 1) == 0)
	    continue;
	new_root = snapshot_edit(root, lnum - 1, line, len, 0, &failed);
	if (failed || new_root == NULL)
	{
	    snapshot_node_unref(new_root);
	    snapshot_node_unref(root);
	    buf_snapshot_invalidate(buf);
	    return;
	}
	snapshot_node_unref(root);
	root = new_root;
	changed = TRUE;
    }
    if (!changed)
    {
	snapshot_node_unref(root);
	return;
    }
    candidate = alloc_clear_id(sizeof(*candidate), aid_buf_snapshot);
    if (candidate == NULL)
    {
	snapshot_node_unref(root);
	buf_snapshot_invalidate(buf);
	return;
    }
    candidate->bs_refcount = 1;
    candidate->bs_base_revision = base->bs_revision;
    candidate->bs_revision = snapshot_next_revision(base->bs_revision);
    candidate->bs_line_count = root->sn_line_count;
    candidate->bs_root = root;
    candidate->bs_allocated = sizeof(*candidate) + root->sn_allocated;
    if (buf_snapshot_publish(buf, candidate) == FAIL)
    {
	buf_snapshot_unref(candidate);
	buf_snapshot_invalidate(buf);
    }
}

/* Construct a candidate with "line" inserted after "after". */
    buf_snapshot_T *
buf_snapshot_prepare_append(buf_T *buf, linenr_T after, const char_u *line,
	colnr_T len)
{
    return snapshot_prepare(buf, after, line, len, 1);
}

/* Construct a candidate with line "lnum" replaced. */
    buf_snapshot_T *
buf_snapshot_prepare_replace(buf_T *buf, linenr_T lnum, const char_u *line,
	colnr_T len)
{
    if (lnum < 1)
	return NULL;
    return snapshot_prepare(buf, lnum - 1, line, len, 0);
}

/* Construct a candidate with line "lnum" deleted. */
    buf_snapshot_T *
buf_snapshot_prepare_delete(buf_T *buf, linenr_T lnum)
{
    if (lnum < 1)
	return NULL;
    return snapshot_prepare(buf, lnum - 1, NULL, 0, -1);
}

/*
 * Publish a prepared candidate.  On success ownership of the caller's
 * reference is transferred to the buffer.  A stale candidate is rejected.
 */
    int
buf_snapshot_publish(buf_T *buf, buf_snapshot_T *candidate)
{
    buf_snapshot_T *old;
    varnumber_T current_revision;

    if (buf == NULL || candidate == NULL || !buf_is_loaded(buf))
	return FAIL;
    old = buf->b_snapshot;
    current_revision = old == NULL ? buf->b_snapshot_revision
							: old->bs_revision;
    if (candidate->bs_base_revision != current_revision)
	return FAIL;
    buf->b_snapshot = candidate;
    buf->b_snapshot_revision = candidate->bs_revision;
    buf_snapshot_unref(old);
    return OK;
}
