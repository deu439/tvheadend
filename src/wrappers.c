#define __USE_GNU
#include "tvheadend.h"
#include <fcntl.h>
#include <sys/types.h>          /* See NOTES */
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>

#ifdef PLATFORM_LINUX
#include <sys/prctl.h>
#include <sys/syscall.h>
#endif

#ifdef PLATFORM_FREEBSD
#include <pthread_np.h>
#endif

/*
 * filedescriptor routines
 */

int
tvh_open(const char *pathname, int flags, mode_t mode)
{
  int fd;

  pthread_mutex_lock(&fork_lock);
  fd = open(pathname, flags, mode);
  if (fd != -1)
    fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
  pthread_mutex_unlock(&fork_lock);
  return fd;
}

int
tvh_socket(int domain, int type, int protocol)
{
  int fd;

  pthread_mutex_lock(&fork_lock);
  fd = socket(domain, type, protocol);
  if (fd != -1)
    fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
  pthread_mutex_unlock(&fork_lock);
  return fd;
}

int
tvh_pipe(int flags, th_pipe_t *p)
{
  int fd[2], err;
  pthread_mutex_lock(&fork_lock);
  err = pipe(fd);
  if (err != -1) {
    fcntl(fd[0], F_SETFD, fcntl(fd[0], F_GETFD) | FD_CLOEXEC);
    fcntl(fd[1], F_SETFD, fcntl(fd[1], F_GETFD) | FD_CLOEXEC);
    fcntl(fd[0], F_SETFL, fcntl(fd[0], F_GETFL) | flags);
    fcntl(fd[1], F_SETFL, fcntl(fd[1], F_GETFL) | flags);
    p->rd = fd[0];
    p->wr = fd[1];
  }
  pthread_mutex_unlock(&fork_lock);
  return err;
}

void
tvh_pipe_close(th_pipe_t *p)
{
  close(p->rd);
  close(p->wr);
  p->rd = -1;
  p->wr = -1;
}

int
tvh_write(int fd, const void *buf, size_t len)
{
  int64_t limit = mclk() + sec2mono(25);
  ssize_t c;

  while (len) {
    c = write(fd, buf, len);
    if (c < 0) {
      if (ERRNO_AGAIN(errno)) {
        if (mclk() > limit)
          break;
        tvh_safe_usleep(100);
        continue;
      }
      break;
    }
    len -= c;
    buf += c;
  }

  return len ? 1 : 0;
}

FILE *
tvh_fopen(const char *filename, const char *mode)
{
  FILE *f;
  int fd;
  pthread_mutex_lock(&fork_lock);
  f = fopen(filename, mode);
  if (f) {
    fd = fileno(f);
    fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
  }
  pthread_mutex_unlock(&fork_lock);
  return f;
}

/*
 * thread routines
 */

static void doquit(int sig)
{
}

struct
thread_state {
  void *(*run)(void*);
  void *arg;
  char name[17];
};

static void *
thread_wrapper ( void *p )
{
  struct thread_state *ts = p;
  sigset_t set;

#if defined(PLATFORM_LINUX)
  /* Set name */
  prctl(PR_SET_NAME, ts->name);
#elif defined(PLATFORM_FREEBSD)
  /* Set name of thread */
  pthread_set_name_np(pthread_self(), ts->name);
#elif defined(PLATFORM_DARWIN)
  pthread_setname_np(ts->name);
#endif

  sigemptyset(&set);
  sigaddset(&set, SIGTERM);
  sigaddset(&set, SIGQUIT);
  pthread_sigmask(SIG_UNBLOCK, &set, NULL);

  signal(SIGTERM, doexit);
  signal(SIGQUIT, doquit);

  /* Run */
  tvhtrace("thread", "created thread %ld [%s / %p(%p)]",
           (long)pthread_self(), ts->name, ts->run, ts->arg);
  void *r = ts->run(ts->arg);
  free(ts);

  return r;
}

int
tvhthread_create
  (pthread_t *thread, const pthread_attr_t *attr,
   void *(*start_routine) (void *), void *arg, const char *name)
{
  int r;
  struct thread_state *ts = calloc(1, sizeof(struct thread_state));
  strncpy(ts->name, "tvh:", 4);
  strncpy(ts->name+4, name, sizeof(ts->name)-4);
  ts->name[sizeof(ts->name)-1] = '\0';
  ts->run  = start_routine;
  ts->arg  = arg;
  r = pthread_create(thread, attr, thread_wrapper, ts);
  return r;
}

/* linux style: -19 .. 20 */
int
tvhtread_renice(int value)
{
  int ret = 0;
#ifdef SYS_gettid
  pid_t tid;
  tid = syscall(SYS_gettid);
  ret = setpriority(PRIO_PROCESS, tid, value);
#elif ENABLE_ANDROID
  pid_t tid;
  tid = gettid();
  ret = setpriority(PRIO_PROCESS, tid, value);
#else
#warning "Implement renice for your platform!"
#endif
  return ret;
}

int
tvh_mutex_timedlock
  ( pthread_mutex_t *mutex, int64_t usec )
{
  int64_t finish = getfastmonoclock() + usec;
  int retcode;

  while ((retcode = pthread_mutex_trylock (mutex)) == EBUSY) {
    if (getfastmonoclock() >= finish)
      return ETIMEDOUT;

    tvh_safe_usleep(10000);
  }

  return retcode;
}

/*
 * thread condition variables - monotonic clocks
 */

int
tvh_cond_init
  ( tvh_cond_t *cond )
{
  int r;

  pthread_condattr_t attr;
  pthread_condattr_init(&attr);
  r = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
  if (r) {
    fprintf(stderr, "Unable to set monotonic clocks for conditions! (%d)", r);
    abort();
  }
  return pthread_cond_init(&cond->cond, &attr);
}

int
tvh_cond_destroy
  ( tvh_cond_t *cond )
{
  return pthread_cond_destroy(&cond->cond);
}

int
tvh_cond_signal
  ( tvh_cond_t *cond, int broadcast )
{
  if (broadcast)
    return pthread_cond_broadcast(&cond->cond);
  else
    return pthread_cond_signal(&cond->cond);
}

int
tvh_cond_wait
  ( tvh_cond_t *cond, pthread_mutex_t *mutex)
{
  return pthread_cond_wait(&cond->cond, mutex);
}

int
tvh_cond_timedwait
  ( tvh_cond_t *cond, pthread_mutex_t *mutex, int64_t monoclock )
{
  struct timespec ts;
  ts.tv_sec = monoclock / MONOCLOCK_RESOLUTION;
  ts.tv_nsec = (monoclock % MONOCLOCK_RESOLUTION) *
               (1000000000ULL/MONOCLOCK_RESOLUTION);
  return pthread_cond_timedwait(&cond->cond, mutex, &ts);
}

/*
 * clocks
 */

void
tvh_safe_usleep(int64_t us)
{
  int64_t r;
  if (us <= 0)
    return;
  do {
    r = tvh_usleep(us);
    if (r < 0) {
      if (ERRNO_AGAIN(-r))
        continue;
      break;
    }
    us = r;
  } while (r > 0);
}

int64_t
tvh_usleep(int64_t us)
{
  struct timespec ts;
  int64_t val;
  int r;
  if (us <= 0)
    return 0;
  ts.tv_sec = us / 1000000LL;
  ts.tv_nsec = (us % 1000000LL) * 1000LL;
  r = clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, &ts);
  val = (ts.tv_sec * 1000000LL) + ((ts.tv_nsec + 500LL) / 1000LL);
  if (ERRNO_AGAIN(r))
    return val;
  return r ? -r : 0;
}

int64_t
tvh_usleep_abs(int64_t us)
{
  struct timespec ts;
  int64_t val;
  int r;
  if (us <= 0)
    return 0;
  ts.tv_sec = us / 1000000LL;
  ts.tv_nsec = (us % 1000000LL) * 1000LL;
  r = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, &ts);
  val = (ts.tv_sec * 1000000LL) + ((ts.tv_nsec + 500LL) / 1000LL);
  if (ERRNO_AGAIN(r))
    return val;
  return r ? -r : 0;
}

/*
 * qsort
 */

#if ! ENABLE_QSORT_R
/*
 * qsort_r wrapper for pre GLIBC 2.8
 */

static __thread struct {
  int (*cmp) ( const void *a, const void *b, void *p );
  void *aux;
} qsort_r_data;

static int
qsort_r_wrap ( const void *a, const void *b )
{
  return qsort_r_data.cmp(a, b, qsort_r_data.aux);
}

void
qsort_r(void *base, size_t nmemb, size_t size,
       int (*cmp)(const void *, const void *, void *), void *aux)
{
  qsort_r_data.cmp = cmp;
  qsort_r_data.aux = aux;
  qsort(base, nmemb, size, qsort_r_wrap);
}
#endif /* ENABLE_QSORT_R */


#if defined(PLATFORM_FREEBSD) || defined(PLATFORM_DARWIN)
struct tvh_qsort_data {
    void *arg;
    int (*compar)(const void *, const void *, void *);
};


static int
tvh_qsort_swap(void *arg, const void *a, const void *b)
{
    struct tvh_qsort_data *data = arg;
    return data->compar(a, b, data->arg);
}
#endif /* PLATFORM_FREEBSD || PLATFORM_DARWIN */


void
tvh_qsort_r(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *, void *), void *arg)
{
#if defined(PLATFORM_FREEBSD) || defined(PLATFORM_DARWIN)
    struct tvh_qsort_data swap_arg = {arg, compar};
    qsort_r(base, nmemb, size, &swap_arg, tvh_qsort_swap);
#else
    qsort_r(base, nmemb, size, compar, arg);
#endif
}
