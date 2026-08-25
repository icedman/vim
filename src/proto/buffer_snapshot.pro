/* buffer_snapshot.c */
buf_snapshot_T *buf_snapshot_acquire(buf_T *buf);
void buf_snapshot_clear(buf_T *buf);
void buf_snapshot_invalidate(buf_T *buf);
int buf_snapshot_threadsafe(void);
void buf_snapshot_ref(buf_snapshot_T *snapshot);
void buf_snapshot_unref(buf_snapshot_T *snapshot);
varnumber_T buf_snapshot_tick(const buf_snapshot_T *snapshot);
linenr_T buf_snapshot_line_count(const buf_snapshot_T *snapshot);
const char_u *buf_snapshot_line(const buf_snapshot_T *snapshot, linenr_T lnum);
colnr_T buf_snapshot_line_len(const buf_snapshot_T *snapshot, linenr_T lnum);
size_t buf_snapshot_size(const buf_snapshot_T *snapshot);
/* vim: set ft=c : */
