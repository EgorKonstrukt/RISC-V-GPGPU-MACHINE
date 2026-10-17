"""A program waiting for an answer on a socket is not woken for room to write (docs/system/nanox.md).

A program writes a question to a Unix socket and sleeps reading the answer. When the other
side takes the question, the socket has room again, and that woke everyone asleep on it: the
reader ran, found nothing and slept again, two switches of process for every question to the
window system. A reader now sleeps through a wake that is only for room to write. Idempotent."""
import sys
root = sys.argv[1]
MARK = 'ShaderEmu unixwake'


def patch(rel, pairs):
    path = root + '/' + rel
    s = open(path).read()
    if MARK in s:
        return
    for old, new in pairs:
        assert s.count(old) == 1, (rel, old)
        s = s.replace(old, new)
    open(path, 'w').write(s)
    print('unixwake hook:', rel)


patch('net/unix/af_unix.c', [
('''static long unix_stream_data_wait(struct sock *sk, long timeo,
				  struct sk_buff *last, unsigned int last_len,
				  bool freezable)
{
	struct sk_buff *tail;
	DEFINE_WAIT(wait);
''', '''/* ShaderEmu unixwake: a wake that says only "there is room to write" is not a reader's */
static int unix_data_wake(struct wait_queue_entry *wq_entry, unsigned int mode, int sync, void *key)
{
	__poll_t bits = key_to_poll(key);

	if (bits && !(bits & ~(EPOLLOUT | EPOLLWRNORM | EPOLLWRBAND)))
		return 0;
	return autoremove_wake_function(wq_entry, mode, sync, key);
}

static long unix_stream_data_wait(struct sock *sk, long timeo,
				  struct sk_buff *last, unsigned int last_len,
				  bool freezable)
{
	struct sk_buff *tail;
	DEFINE_WAIT_FUNC(wait, unix_data_wake);
'''),
])
