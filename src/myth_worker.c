/* 
 * myth_worker.c
 */

#include "myth_config.h"
#include "config.h"

#include "myth_worker.h"
#include "myth_worker_func.h"

#if EXPERIMENTAL_SCHEDULER
static myth_thread_t myth_steal_func_with_prob(int rank) __attribute__((unused));
static myth_thread_t myth_steal_func_with_prob_and_min_success(int rank)  __attribute__((unused));
static myth_thread_t myth_steal_func_with_prob_and_double_check(int rank)  __attribute__((unused));
static myth_thread_t myth_steal_func_with_global_status(int rank);
myth_steal_func_t g_myth_steal_func = myth_steal_func_with_global_status;
#else
myth_thread_t myth_default_steal_func(int rank);
myth_steal_func_t g_myth_steal_func = myth_default_steal_func;
#endif

myth_steal_func_t myth_wsapi_set_stealfunc(myth_steal_func_t fn)
{
  myth_steal_func_t prev=g_myth_steal_func;
  g_myth_steal_func=fn;
  return prev;
}

extern myth_running_env_t g_envs;

myth_thread_t myth_default_steal_func(int rank) {
  myth_running_env_t env,busy_env;
  myth_thread_t next_run = NULL;
#if MYTH_WS_PROF_DETAIL
  uint64_t t0, t1;
  t0 = myth_get_rdtsc();
#endif
  //Choose a worker thread that seems to be busy
  env = &g_envs[rank];
  busy_env = myth_env_get_first_busy(env);
  if (busy_env){
    //int ws_victim;
#if 0
#if MYTH_SCHED_LOOP_DEBUG
    myth_dprintf("env %p is trying to steal thread from %p...\n",env,busy_env);
#endif
#endif
    //ws_victim=busy_env->rank;
    //Try to steal thread
    next_run = myth_queue_take(&busy_env->runnable_q);
    if (next_run){
#if MYTH_SCHED_LOOP_DEBUG
      myth_dprintf("env %p is stealing thread %p from %p...\n",env,steal_th,busy_env);
#endif
      myth_assert(next_run->status==MYTH_STATUS_READY);
      //Change worker thread descriptor
    }
  }
#if MYTH_WS_PROF_DETAIL
  t1 = myth_get_rdtsc();
  if (g_sched_prof){
    env->prof_data.ws_attempt_count[busy_env->rank]++;
    if (next_run){
      env->prof_data.ws_hit_cycles += t1 - t0;
      env->prof_data.ws_hit_cnt++;
    }else{
      env->prof_data.ws_miss_cycles += t1 - t0;
      env->prof_data.ws_miss_cnt++;
    }
  }
#endif
  return next_run;
}

#if EXPERIMENTAL_SCHEDULER

static long * myth_steal_prob_table;
static int * myth_min_success_table;
static int * myth_double_check_table;
static domain myth_cpu_domain;

static myth_running_env_t
myth_env_choose_victim(myth_running_env_t e) {
  /* P[i] <= X < P[i+1] ==> i */
  int n = g_attr.n_workers;
  if (n == 1) {
    return NULL;
  } else {
    long * P = e->steal_prob;
    long x = nrand48(e->steal_rg);
    if (P[n - 1] <= x) {
      return &g_envs[n - 1];
    } else {
      int a = 0; int b = n - 1;
      assert(P[a] <= x);
      assert(x < P[b]);
      while (b - a > 1) {
	int c = (a + b) / 2;
	if (P[c] <= x) {
	  a = c;
	} else {
	  b = c;
	}
	assert(P[a] <= x);
	assert(x < P[b]);
      }
      assert(0 <= a);
      assert(a < n - 1);
      assert(P[a] <= x);
      assert(x < P[a + 1]);
      assert(a != e->rank);
      return &g_envs[a];
    }
  }
}

static myth_thread_t myth_steal_func_with_prob(int rank) {
  myth_running_env_t env = &g_envs[rank];
  myth_running_env_t busy_env = myth_env_choose_victim(env);
  if (busy_env){
    myth_thread_t next_run = myth_queue_take(&busy_env->runnable_q);
    if (next_run){
      myth_assert(next_run->status == MYTH_STATUS_READY);
    }
    return next_run;
  } else {
    return NULL;
  }
}

void steal_history_init(steal_history * h, size_t sz) {
  h->n_con_success = 0;
}

long steal_history_put(steal_history * h, char x) {
  if (x) {
    h->n_con_success++;
  } else {
    h->n_con_success = 0;
  }
  return h->n_con_success;
}

static myth_thread_t myth_steal_func_with_prob_and_min_success(int rank) {
  assert(g_attr.n_workers > 1);
  myth_running_env_t env = &g_envs[rank];
  while (1) {
    myth_running_env_t victim_env = myth_env_choose_victim(env);
    if (env->steal_hist.n_con_success + 1 >= env->min_success[victim_env->rank]) {
      myth_thread_t next_run = myth_queue_take(&victim_env->runnable_q);
      steal_history_put(&env->steal_hist, (next_run ? 1 : 0));
      if (next_run) {
	myth_assert(next_run->status == MYTH_STATUS_READY);
      }
      return next_run;
    } else {
      myth_thread_t next_run = myth_queue_peek(&victim_env->runnable_q);
      steal_history_put(&env->steal_hist, (next_run ? 1 : 0));
    }
  }
}

static myth_thread_t myth_steal_func_with_prob_and_double_check(int rank) {
  assert(g_attr.n_workers > 1);
  myth_running_env_t env = &g_envs[rank];
  myth_running_env_t victim_env = myth_env_choose_victim(env);
  myth_thread_t next_run = 0;
  int i;
  int n_checks = env->double_check[victim_env->rank];
  assert(n_checks > 0);
  for (i = 0; i < n_checks; i++) {
    myth_thread_t check_next_run = ((i == n_checks - 1) ?
				    myth_queue_take(&victim_env->runnable_q) :
				    myth_queue_peek(&victim_env->runnable_q));
    if (0 < i && i < n_checks - 1 && check_next_run != next_run) {
      /* bottom of the deque changed, start over */
      return 0;
    }
    next_run = check_next_run;
  }
  return next_run;
}

static int n_cpus_at_level(domain D, int level) {
  int l;
  int n = 1;
  for (l = level; l < D.n_levels; l++) {
    n *= D.levels[l].n_children;
  }
  return n;
}

static void worker_status_init(worker_status * ws, int rank, int nw) {
  int i;
  domain D = myth_cpu_domain;
  int n_levels = D.n_levels;
  int ** busy_count = (int **)myth_malloc(sizeof(int *) * n_levels);
  int * my_index = (int *)myth_malloc(sizeof(int) * n_levels);
  char * status = (char *)myth_malloc(nw);
  for (i = 0; i < n_levels; i++) {
    int * c = (int *)myth_malloc(sizeof(int) * D.levels[i].n_children);
    int j;
    for (j = 0; j < D.levels[i].n_children; j++) {
      c[j] = 0;
    }
    busy_count[i] = c;
  }
  for (i = 0; i < nw; i++) {
    status[i] = 0;
  }
  int rk = rank;
  for (i = 0; i < n_levels; i++) {
    int cpus_per_child = n_cpus_at_level(D, i + 1);
    int idx = rk / cpus_per_child;
    assert(idx < D.levels[i].n_children);
    my_index[i] = idx;
    rk -= idx * cpus_per_child;
  }
  ws->nw = nw;
  ws->n_levels = n_levels;
  ws->busy_count = busy_count;
  ws->my_index = my_index;
  ws->status = status;
}

static int count_busy_in_range(worker_status * ws, int p, int q) {
  int i;
  int nw = ws->nw;
  int s = 0;
  p = (p < nw ? p : nw);
  q = (q < nw ? q : nw);
  for (i = p; i < q; i++) {
    myth_running_env_t victim_env = &g_envs[i];
    myth_thread_t th = myth_queue_peek(&victim_env->runnable_q);
    int x = (th ? 1 : 0);
    ws->status[i] = x;
    s += x;
  }
  return s;
}

/* return l s.t. an interval at level l has at least one empty child */
static void count_busy_at_levels(int l, int a, int b, int rank,
				 worker_status * ws, domain D) {
  if (l < D.n_levels) {
    int np = D.levels[l].n_children;
    int workers_per_child = n_cpus_at_level(D, l + 1);
    int i;
    assert((b - a) / np == workers_per_child);
    assert((b - a) % np == 0);
    for (i = 0; i < np; i++) {
      int p = a +  i      * workers_per_child;
      int q = a + (i + 1) * workers_per_child;
      ws->busy_count[l][i] = count_busy_in_range(ws, p, q);
      if (p <= rank && rank < q) {
	count_busy_at_levels(l + 1, p, q, rank, ws, D);
      }
    }
  }
}

/* return l s.t. an interval at level l has at least one empty child */
static int find_level_to_steal(worker_status * ws, domain D) {
  int l;
  for (l = 0; l < D.n_levels; l++) {
    int np = D.levels[l].n_children;
    int i;
    for (i = 0; i < np; i++) {
      if (ws->busy_count[l][i] == 0) {
	return l;
      }
    }
  }
  assert(0);			/* should not reach here */
}

myth_thread_t steal_from_level(myth_running_env_t env,
			       worker_status * ws, int idx, int l, domain D) {
  int i;
  int n_candidates = 0;
  for (i = 0; i < D.levels[l].n_children; i++) {
    if (i != idx) {
      n_candidates += ws->busy_count[l][i];
    }
  }
  if (n_candidates == 0) {
    assert(l == 0);
    return 0;
  }
  int x = nrand48(env->steal_rg) % n_candidates;
  int y = 0;
  assert(idx < D.levels[l].n_children);

  int a = 0;
  for (i = 0; i < l; i++) {
    a += ws->my_index[i] * n_cpus_at_level(D, i + 1);
  }
  int sz = n_cpus_at_level(D, l + 1);
  myth_running_env_t victim_env = 0;
  for (i = 0; i < D.levels[l].n_children; i++) {
    if (i != idx) {
      int b = a + i * sz;
      int e = b + sz;
      int j;
      for (j = b; j < e; j++) {
	if (ws->status[j]) {
	  if (y == x) {
	    victim_env = &g_envs[j];
	  }
	  y++;
	}
      }
    }
  }
  assert(y == n_candidates);
  assert(victim_env);
  return myth_queue_take(&victim_env->runnable_q);
}

static myth_thread_t myth_steal_func_with_global_status(int rank) {
  /* 
     (1) if any level 1 domain is empty, only one worker in each 
     level 1 domain is active 
     (2) otherwise if any level 2 domain is empty, 
   */
  myth_running_env_t env = &g_envs[rank];
  worker_status * ws = env->worker_status;
  domain D = myth_cpu_domain;
  int n_cpus = n_cpus_at_level(D, 0);
  count_busy_at_levels(0, 0, n_cpus, rank, ws, D);
  int l = find_level_to_steal(ws, D);
  int idx = ws->my_index[l];
  if (ws->busy_count[l][idx]) {
    /* my domain is not empty, but there are some empty siblings.
       let one of them to steal */
    return 0;
  } else {
    /* my domain is empty */
    myth_thread_t th = steal_from_level(env, ws, idx, l, D);
    return th;
  }
}
  
/* probability base stealing works by first building an 
   array of n_cpus x n_cpus and then converting it to 
   n_workers x n_workers.
   p is an array having n_cpus x n_cpus elements.
 */

static long * myth_array_to_int_prob(int * p, int n_cpus, int n_workers) {
  int i, j;
  double * q = (double *)myth_malloc(sizeof(double) * n_workers * n_workers);

  /* copy the appropriate part of p into q, 
     repeating itself if p is smaller than q */
  for (i = 0; i < n_workers; i++) {
    for (j = 0; j < n_workers; j++) {
      int ii = i % n_cpus;
      int jj = j % n_cpus;
      q[i * n_workers + j] = p[ii * n_cpus + jj];
    }
  }
  /* set diagonal line to zero */
  for (i = 0; i < n_workers; i++) {
    q[i * n_workers + i] = 0.0;
  }
  p = 0;

  /* normalize */
  for (i = 0; i < n_workers; i++) {
    double t = 0;
    for (j = 0; j < n_workers; j++) {
      t += q[i * n_workers + j];
    }
    assert(n_workers == 1 || t > 0);
    if (t > 0) {
      for (j = 0; j < n_workers; j++) {
	q[i * n_workers + j] /= t;
      }
    }
  }
  
  /* print */
  if (0) {
    for (i = 0; i < n_workers; i++) {
      for (j = 0; j < n_workers; j++) {
	if (j > 0) printf(" ");
	printf("%d", p[i * n_workers + j]);
      }
      printf("\n");
    }
  }

  /* convert p into P, whose P[i][j] is 2^31 x the 
     probability that i chooses one of 0 ... j-1 as a victim.
     the probablity i steals from j is (P[i][j+1] - P[i][j]) / 2^31.
     the victim is chosen by drawing a random number x from [0,2^31],
     and find j s.t. P[i][j] <= x < P[i][j+1] (binary search)
  */
  long * P = myth_malloc(n_workers * n_workers * sizeof(long));
  for (i = 0; i < n_workers; i++) {
    double x = 0.0;
    for (j = 0; j < n_workers; j++) {
      assert(0 <= x);
      P[i * n_workers + j] = x * (1UL << 31);
      assert(P[i * n_workers + j] <= (1UL << 31));
      x += q[i * n_workers + j];
    }
  }
  return P;
}

/* probability distribution based on a small description
   of the machine, like this:
   4,1.0x8,5.0x2,10.0

   this describes a machine that consists of 4x8x2 processors.
   you can think of it as a node having
   4 sockets at the upper most level,
   8 cores per socket at the second level,
   and 2 hardware threads per core at the third level.

   the numbers after comma describe (relative) probability
   with which a processor apart at that level is chosen as a 
   victim.

   for example, 1.0 : 5.0 : 10.0 means the following.
   let's say you have a processor trying to perform work stealing.
   let's say a processor p is in a different domain at the first level,
   q in the same domain at the first level but in a different domain
   at the second, and r in the same domain in the second domain.
   then the relative probability the first processor chooses p, q, and r
   is 1 : 5 : 10.  q is five times more likely to be chosen than p
   and r is ten times more likely to be chosen than p.

*/

/* convert domain hierarchy of the machine into 
   2D array of values */

static void myth_domain_to_array_rec(int * v, int a, int b, int l,
				     domain D, int n_cpus) {
  if (l == D.n_levels) {
    /* we are bottom of the hierarchy. we should have exactly one core.
       the steal probability is zero */
    assert(b - a == 1);
    v[0] = 0;
  } else {
    /* this level has np partitions */
    int np = D.levels[l].n_children;
    int workers_per_part = (b - a) / np;
    int i;
    assert((b - a) % np == 0);
    /* split the subarray v[a:b,a:b] into np x np partitions */
    for (i = 0; i < np; i++) {
      int s = a + i * workers_per_part;
      int j;
      for (j = 0; j < np; j++) {
	int t = a + j * workers_per_part;
	if (i == j) {
	  /* a tile on diagonal line. recursively decompose it */
	  myth_domain_to_array_rec(v, s, s + workers_per_part, l + 1, D, n_cpus);
	} else {
	  /* a tile not on diagonal line. the probability is
	     what is specified in the machine description */
	  int ii, jj;
	  for (ii = s; ii < s + workers_per_part; ii++) {
	    for (jj = t; jj < t + workers_per_part; jj++) {
	      v[ii * n_cpus + jj] = D.levels[l].val;
	    }
	  }
	}
      }
    }
  }
}

static int * myth_domain_to_array(domain D, int n_cpus) {
  int * p = (int *)myth_malloc(sizeof(int) * n_cpus * n_cpus);
  myth_domain_to_array_rec(p, 0, n_cpus, 0, D, n_cpus);
  return p;
}

/* parse a string like 4,1x8,2 describing domain hierarchy
 */
domain parse_domain_desc(char * s) {
  char * buf = strdup(s);	/* free it! */
  char * p = buf;
  int level = 1;
  int l;
  while (1) {
    char * x = strrchr(p, 'x');
    if (x) {
      p = x + 1;
      level++;
    } else {
      break;
    }
  }
  domain_level * H = (domain_level *)malloc(sizeof(domain_level) * level);
  p = buf;
  for (l = 0; l < level; l++) {
    char * x = strrchr(p, 'x');
    /* terminate string at colon */
    if (x) {
      *x = 0;
    }
    /* look for , */
    char * comma = strrchr(p, ',');
    if (comma) {
      *comma = 0;
      H[l].n_children = atoi(p);
      H[l].val = atoi(comma + 1);
    } else {
      H[l].n_children = atoi(p);
      H[l].val = 1;
    }
    p = x + 1;
  }
  domain D = { level, H };
  return D;
}

static domain myth_parse_domain(char * s, int nw) {
  domain D;
  if (s) {
    D = parse_domain_desc(s);
  } else {
    D.levels = (domain_level *)myth_malloc(sizeof(domain_level));
    D.levels->n_children = nw;
    D.levels->val = 1;
    D.n_levels = 1;
  }
  return D;
}

static long * myth_prob_from_domain(char * s, int n_workers) {
  domain D = myth_parse_domain(s, n_workers);
  int n_cpus = n_cpus_at_level(D, 0);
  int * p = myth_domain_to_array(D, n_cpus);
  long * P = myth_array_to_int_prob(p, n_cpus, n_workers);
  myth_free(D.levels);
  myth_free(p);
  return P;
}

static long * myth_prob_from_file(FILE * fp, int nw) {
  /* read a file describing the (relative) probability a worker chooses
     a victime
     
     N
     P0,0 P0,1 P0,2 ... P0,N-1
     P1,0 P1,1 P1,2 ... P1,N-1
          ...
     PN-1,0,        ... PN-1,N-1

  */
  /* nw is the number of workers used in this execution
     of the program; n_workers_in_file is the number of workers
     described in the file. they may be different. */
  int n_cpus = 0;
  if (fp) {
    int x = fscanf(fp, "%d", &n_cpus);
    assert(x == 1);
  } else {
    /* no file. default (uniform distribution) */
    n_cpus = nw;
  }
  /* p[i][j] is a relative probability that i chooses j as a victim */
  int * p = myth_malloc(n_cpus * n_cpus * sizeof(int));
  int i, j;
  for (i = 0; i < n_cpus; i++) {
    for (j = 0; j < n_cpus; j++) {
      if (fp) {
	int x = fscanf(fp, "%d", &p[i * n_cpus + j]);
	assert(x == 1);
	assert(p[i * n_cpus + j] > 0);
      } else {
	p[i * n_cpus + j] = 1; /* uniform (diagonal line set to zero below) */
      }
    }
  }
  long * P = myth_array_to_int_prob(p, n_cpus, nw);
  myth_free(p);
  return P;
}

static int * myth_min_success(char * s, int nw) {
  int n_cpus = 1;
  int l; 
  domain D = myth_parse_domain(s, nw);
  for (l = 0; l < D.n_levels; l++) {
    n_cpus *= D.levels[l].n_children;
  }
  int * S = myth_domain_to_array(D, n_cpus);
  myth_free(D.levels);
  return S;
}

static int * myth_double_check(char * s, int nw) {
  return myth_min_success(s, nw);
}

int myth_scheduler_global_init(int nw) {
  char * hierarchy = getenv("MYTH_CPU_HIERARCHY");
  char * prob_file = getenv("MYTH_PROB_FILE");
  if (hierarchy) {
    myth_steal_prob_table = myth_prob_from_domain(hierarchy, nw);
  } else if (prob_file) {
    FILE * fp = fopen(prob_file, "rb");
    if (!fp) { perror("fopen"); exit(1); }
    myth_steal_prob_table = myth_prob_from_file(fp, nw);
    fclose(fp);
  } else {
    /* default. uniform */
    myth_steal_prob_table = myth_prob_from_file(0, nw);
  }
  /*  */
  char * min_success = getenv("MYTH_MIN_SUCCESS");
  myth_min_success_table = myth_min_success(min_success, nw);

  char * double_check = getenv("MYTH_DOUBLE_CHECK");
  myth_double_check_table = myth_double_check(double_check, nw);

  char * cpu_domain = getenv("MYTH_CPU_DOMAIN");
  myth_cpu_domain = myth_parse_domain(cpu_domain, nw);
  
  return 0;
}


int myth_scheduler_worker_init(int rank, int nw) {
  myth_running_env_t env = &g_envs[rank];
  env->steal_prob = &myth_steal_prob_table[rank * nw];
  env->min_success = &myth_min_success_table[rank * nw];
  env->double_check = &myth_double_check_table[rank * nw];
  /* random seed */
  env->steal_rg[0] = rank;
  env->steal_rg[1] = rank + 1;
  env->steal_rg[2] = rank + 2;

  worker_status_init(env->worker_status, rank, nw);
  return 0;
}

#endif	/* EXPERIMENTAL_SCHEDULER */
