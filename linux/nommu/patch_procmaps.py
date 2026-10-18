"""No MMU: a read of /proc/PID/maps that reached the last mapping kept PID's mmap read lock, so PID's next
mmap waited forever. m_stop now unlocks whenever m_start locked, as upstream's fe4419801617 (Linux 6.8)."""
import sys
p = sys.argv[1] + '/fs/proc/task_nommu.c'
s = open(p).read()
MARK = '/* m_start kept the lock and the task only if priv->task is set */'
start_old = '''	mm = priv->mm;
	if (!mm || !mmget_not_zero(mm))
		return NULL;

	if (mmap_read_lock_killable(mm)) {
		mmput(mm);
		return ERR_PTR(-EINTR);
	}

	/* start from the Nth VMA */
	for (p = rb_first(&mm->mm_rb); p; p = rb_next(p))
		if (n-- == 0)
			return p;

	mmap_read_unlock(mm);
	mmput(mm);
	return NULL;
}
'''
start_new = '''	mm = priv->mm;
	if (!mm || !mmget_not_zero(mm)) {
		put_task_struct(priv->task);
		priv->task = NULL;
		return NULL;
	}

	if (mmap_read_lock_killable(mm)) {
		mmput(mm);
		put_task_struct(priv->task);
		priv->task = NULL;
		return ERR_PTR(-EINTR);
	}

	/* start from the Nth VMA */
	for (p = rb_first(&mm->mm_rb); p; p = rb_next(p))
		if (n-- == 0)
			return p;

	mmap_read_unlock(mm);
	mmput(mm);
	put_task_struct(priv->task);
	priv->task = NULL;
	return NULL;
}
'''
stop_old = '''static void m_stop(struct seq_file *m, void *_vml)
{
	struct proc_maps_private *priv = m->private;

	if (!IS_ERR_OR_NULL(_vml)) {
		mmap_read_unlock(priv->mm);
		mmput(priv->mm);
	}
	if (priv->task) {
		put_task_struct(priv->task);
		priv->task = NULL;
	}
}
'''
stop_new = '''static void m_stop(struct seq_file *m, void *_vml)
{
	struct proc_maps_private *priv = m->private;

	''' + MARK + '''
	if (!priv->task)
		return;
	mmap_read_unlock(priv->mm);
	mmput(priv->mm);
	put_task_struct(priv->task);
	priv->task = NULL;
}
'''
if MARK not in s:
    assert s.count(start_old) == 1 and s.count(stop_old) == 1
    s = s.replace(start_old, start_new).replace(stop_old, stop_new)
    open(p, 'w').write(s)
print('task_nommu.c patched')
