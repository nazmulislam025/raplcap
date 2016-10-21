/**
 * Implementation that uses MSRs directly.
 *
 * See the Intel 64 and IA-32 Architectures Software Developer's Manual for MSR
 * register bit fields.
 *
 * @author Connor Imes
 * @date 2016-10-19
 */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <unistd.h>
#include "raplcap.h"

#define MSR_RAPL_POWER_UNIT       0x606

/* Package RAPL Domain */
#define MSR_PKG_POWER_LIMIT       0x610

/* PP0 RAPL Domain */
#define MSR_PP0_POWER_LIMIT       0x638

/* PP1 RAPL Domain, may reflect to uncore devices */
#define MSR_PP1_POWER_LIMIT       0x640

/* DRAM RAPL Domain */
#define MSR_DRAM_POWER_LIMIT      0x618

/* Platform (PSys) Domain (Skylake and newer) */
#define MSR_PLATFORM_POWER_LIMIT  0x65C

typedef struct raplcap_msr_direct {
  int* fds;
  uint32_t nfds;
  // assuming consistent unit values between sockets
  double power_units;
  double time_units;
} raplcap_msr_direct;

static inline int open_msr(uint32_t core) {
  char msr_filename[32];
  int fd;
  sprintf(msr_filename, "/dev/cpu/%"PRIu32"/msr", core);
  fd = open(msr_filename, O_RDWR);
  if (fd < 0) {
    return -1;
  }
  return fd;
}

static inline int read_msr(int fd, int which, uint64_t* data) {
  assert(data != NULL);
  if (pread(fd, data, sizeof(uint64_t), which) != sizeof(uint64_t)) {
    return -1;
  }
  return 0;
}

static inline int write_msr(int fd, int which, uint64_t data) {
  if (pwrite(fd, &data, sizeof(uint64_t), which) != sizeof(uint64_t)) {
    return -1;
  }
  return 0;
}

// modified from LLNL's libmsr
static inline uint32_t get_num_sockets() {
  unsigned first = 0;
  unsigned second = 0;
  uint64_t rax = 0xb;
  uint64_t rbx = 0;
  uint64_t rcx = 0x0;
  uint64_t rdx = 0;
  int allcores = 0;

  uint64_t coresPerSocket;
  uint64_t hyperThreads;
  int HTenabled;
  uint64_t sockets;

  // Use rcx = 0 to see if hyperthreading is supported. If > 1, then there is
  // HT.
  // Use rcx = 1 to see how many cores are available per socket (including
  // HT, if supported).
  FILE* thread = fopen("/sys/devices/system/cpu/cpu0/topology/thread_siblings_list", "r");
  if (thread == NULL) {
    return 0;
  }
  int ret = fscanf(thread, "%u,%u", &first, &second);
  if (ret < 2 || ret == EOF) {
    /* Hyperthreading is disabled. */
    HTenabled = 0;
  } else {
    HTenabled = 1;
  }
  fclose(thread);

  asm volatile(
      "cpuid"
      : "=a" (rax), "=b" (rbx), "=c" (rcx), "=d" (rdx)
      : "0" (rax), "2"(rcx)
  );
  hyperThreads = ((rbx) & 0xFFFF);
  rax = 0xb;
  rbx = 0;
  rcx = 0x1;
  rdx = 0;

  asm volatile(
      "cpuid"
      : "=a" (rax), "=b" (rbx), "=c" (rcx), "=d" (rdx)
      : "0" (rax), "2"(rcx)
  );
  coresPerSocket = ((rbx) & 0xFFFF) / hyperThreads;
  // get_nprocs_conf() returns max number of logical processors (including
  // hyperthreading)
  // get_nprocs() returns num logical processors depending on whether
  // hyperthreading is enabled or not
  allcores = get_nprocs_conf();
  if (allcores == coresPerSocket * (HTenabled + 1)) {
    sockets = 1;
  } else {
    sockets = 2;
  }

  return sockets;
}

int raplcap_init(raplcap* rc) {
  int err_save = 0;
  if (rc == NULL) {
    errno = EINVAL;
    return -1;
  }
  memset(rc, 0, sizeof(raplcap));
  // tODO: Defaulting to 1 socket for now
  uint32_t sockets = 1; // get_num_sockets();
  if (sockets == 0) {
    return -1;
  }
  raplcap_msr_direct* state = malloc(sizeof(raplcap_msr_direct));
  if (state == NULL) {
    return -1;
  }
  state->fds = calloc(sockets, sizeof(int));
  if (state->fds == NULL) {
    free(state);
    return -1;
  }
  uint32_t i, j;
  for (i = 0; i < sockets; i++) {
    // TODO: when >1 socket, decide which MSRs to actually open and open them
    state->fds[i] = open_msr(i);
    if (state->fds[i] < 0) {
      err_save = errno;
      // cleanup
      for (j = 0; j < i; j++) {
        close(state->fds[j]);
      }
      free(state->fds);
      free(state);
      errno = err_save;
      return -1;
    }
  }

  rc->nsockets = sockets;
  rc->state = state;

  uint64_t msrval;
  if (read_msr(state->fds[0], MSR_RAPL_POWER_UNIT, &msrval)) {
    raplcap_destroy(rc);
  }
  state->power_units = pow(0.5, (double) (msrval & 0xf));
  state->time_units = pow(0.5, (double) ((msrval >> 16) & 0xf));

  return 0;
}

int raplcap_destroy(raplcap* rc) {
  int ret = 0;
  int err_save = 0;
  uint32_t i;
  if (rc != NULL && rc->state != NULL) {
    raplcap_msr_direct* state = (raplcap_msr_direct*) rc->state;
    for (i = 0; i < state->nfds; i++) {
      if (state->fds[i] > 0 && close(state->fds[i])) {
        err_save = errno;
        ret = -1;
      }
    }
    free(state->fds);
    free(state);
    rc->state = NULL;
  }
  errno = err_save;
  return ret;
}

uint32_t raplcap_get_num_sockets(const raplcap* rc) {
  return rc == NULL ? get_num_sockets() : rc->nsockets;
}

int raplcap_is_zone_supported(uint32_t socket, const raplcap* rc, raplcap_zone zone) {
  if (rc == NULL || socket >= rc->nsockets) {
    errno = EINVAL;
    return -1;
  }
  // TODO: Discover dynamically
  switch (zone) {
    case RAPLCAP_ZONE_PACKAGE:
    case RAPLCAP_ZONE_CORE:
      // always supported
      return 1;
    case RAPLCAP_ZONE_UNCORE:
#if defined(RAPL_UNCORE_SUPPORTED)
      return RAPL_UNCORE_SUPPORTED;
#else
      return 1;
#endif
    case RAPLCAP_ZONE_DRAM:
#if defined(RAPL_DRAM_SUPPORTED)
      return RAPL_DRAM_SUPPORTED;
#else
      return 1;
#endif
    case RAPLCAP_ZONE_PSYS:
#if defined(RAPL_PSYS_SUPPORTED)
      return RAPL_PSYS_SUPPORTED;
#else
      return 1;
#endif
    default:
      errno = EINVAL;
      return -1;
  }
}

/**
 * Get the bits requested and shift right if needed.
 * First and last are inclusive.
 */
static inline uint64_t get_bits(uint64_t msrval, uint32_t first, uint32_t last) {
  assert(first <= last);
  assert(last < 64);
  return (msrval >> first) & ((1 << (last - first + 1)) - 1);
}

static inline int is_pkg_platform_enabled(uint64_t msrval) {
  return get_bits(msrval, 15, 15) || get_bits(msrval, 47, 47);
}

static inline int is_core_uncore_dram_enabled(uint64_t msrval) {
  return get_bits(msrval, 15, 15);
}

int raplcap_is_zone_enabled(uint32_t socket, const raplcap* rc, raplcap_zone zone) {
  int ret = 0;
  uint64_t msrval = 0;
  if (rc == NULL || rc->state == NULL || socket >= rc->nsockets) {
    errno = EINVAL;
    return -1;
  }
  raplcap_msr_direct* state = (raplcap_msr_direct*) rc->state;
  switch (zone) {
    case RAPLCAP_ZONE_PACKAGE:
      ret = read_msr(state->fds[socket], MSR_PKG_POWER_LIMIT, &msrval);
      if (!ret) {
        ret = is_pkg_platform_enabled(msrval);
      }
      break;
    case RAPLCAP_ZONE_CORE:
      ret = read_msr(state->fds[socket], MSR_PP0_POWER_LIMIT, &msrval);
      if (!ret) {
        ret = is_core_uncore_dram_enabled(msrval);
      }
      break;
    case RAPLCAP_ZONE_UNCORE:
      ret = read_msr(state->fds[socket], MSR_PP1_POWER_LIMIT, &msrval);
      if (!ret) {
        ret = is_core_uncore_dram_enabled(msrval);
      }
      break;
    case RAPLCAP_ZONE_DRAM:
      ret = read_msr(state->fds[socket], MSR_DRAM_POWER_LIMIT, &msrval);
      if (!ret) {
        ret = is_core_uncore_dram_enabled(msrval);
      }
      break;
    case RAPLCAP_ZONE_PSYS:
      ret = read_msr(state->fds[socket], MSR_PLATFORM_POWER_LIMIT, &msrval);
      if (!ret) {
        ret = is_pkg_platform_enabled(msrval);
      }
      break;
    default:
      errno = EINVAL;
      ret = -1;
      break;
  }
  return ret;
}

static inline uint64_t replace_bits(uint64_t msrval, uint64_t data, uint32_t first, uint32_t last) {
  // first and last are inclusive
  assert(first <= last);
  assert(last < 64);
  uint64_t mask = (((uint64_t) 1 << (last - first + 1)) - 1) << first;
  return (msrval & ~mask) | (data & mask);
}

static inline uint64_t set_pkg_platform_enabled(uint64_t msrval, int enabled) {
  int set = enabled ? 1 : 0;
  // set RAPL enable
  msrval = replace_bits(msrval, set ? 1 : 0, 15, 15);
  msrval = replace_bits(msrval, set ? 1 : 0, 47, 47);
  // set clamping enable
  msrval = replace_bits(msrval, set ? 1 : 0, 16, 16);
  return replace_bits(msrval, set ? 1 : 0, 48, 48);
}

static inline uint64_t set_core_uncore_dram_enabled(uint64_t msrval, int enabled) {
  int set = enabled ? 1 : 0;
  // set RAPL enable
  msrval = replace_bits(msrval, set ? 1 : 0, 15, 15);
  // set clamping enable
  return replace_bits(msrval, set ? 1 : 0, 16, 16);
}

int raplcap_set_zone_enabled(uint32_t socket, const raplcap* rc, raplcap_zone zone, int enabled) {
  int ret = 0;
  uint64_t msrval = 0;
  if (rc == NULL || rc->state == NULL || socket >= rc->nsockets) {
    errno = EINVAL;
    return -1;
  }
  raplcap_msr_direct* state = (raplcap_msr_direct*) rc->state;
  switch (zone) {
    case RAPLCAP_ZONE_PACKAGE:
      ret = read_msr(state->fds[socket], MSR_PKG_POWER_LIMIT, &msrval);
      if (!ret) {
        msrval = set_pkg_platform_enabled(msrval, enabled);
        ret = write_msr(state->fds[socket], MSR_PKG_POWER_LIMIT, msrval);
      }
      break;
    case RAPLCAP_ZONE_CORE:
      ret = read_msr(state->fds[socket], MSR_PP0_POWER_LIMIT, &msrval);
      if (!ret) {
        msrval = set_core_uncore_dram_enabled(msrval, enabled);
        ret = write_msr(state->fds[socket], MSR_PP0_POWER_LIMIT, msrval);
      }
      break;
    case RAPLCAP_ZONE_UNCORE:
      ret = read_msr(state->fds[socket], MSR_PP1_POWER_LIMIT, &msrval);
      if (!ret) {
        msrval = set_core_uncore_dram_enabled(msrval, enabled);
        ret = write_msr(state->fds[socket], MSR_PP1_POWER_LIMIT, msrval);
      }
      break;
    case RAPLCAP_ZONE_DRAM:
      ret = read_msr(state->fds[socket], MSR_DRAM_POWER_LIMIT, &msrval);
      if (!ret) {
        msrval = set_core_uncore_dram_enabled(msrval, enabled);
        ret = write_msr(state->fds[socket], MSR_DRAM_POWER_LIMIT, msrval);
      }
      break;
    case RAPLCAP_ZONE_PSYS:
      ret = read_msr(state->fds[socket], MSR_PLATFORM_POWER_LIMIT, &msrval);
      if (!ret) {
        msrval = set_pkg_platform_enabled(msrval, enabled);
        ret = write_msr(state->fds[socket], MSR_PLATFORM_POWER_LIMIT, msrval);
      }
      break;
    default:
      errno = EINVAL;
      ret = -1;
      break;
  }
  return ret;
}

static inline void to_raplcap(raplcap_limit* limit, double seconds, double watts) {
  assert(limit != NULL);
  limit->seconds = seconds;
  limit->watts = watts;
}

/**
 * F is a single-digit decimal floating-point value between 1.0 and 1.3 with
 * the fraction digit represented by 2 bits.
 */
static inline double to_time_window_F(uint32_t bits) {
  assert(bits <= 3);
  return 1.0 + 0.1 * bits;
}

/**
 * Get the power and time window values for long and short limits.
 */
static inline void get_pkg_platform(uint64_t msrval, const raplcap_msr_direct* state,
                                    raplcap_limit* limit_long, raplcap_limit* limit_short) {
  assert(state != NULL);
  if (limit_long != NULL) {
    // bits 14:0
    // The unit of this field is specified by the “Power Units” field of MSR_RAPL_POWER_UNIT.
    double power_long = state->power_units * (double) get_bits(msrval, 0, 14);
    // Time limit = 2^Y * (1.0 + Z/4.0) * Time_Unit
    // Here “Y” is the unsigned integer value represented. by bits 21:17, “Z” is an unsigned integer represented by
    // bits 23:22. “Time_Unit” is specified by the “Time Units” field of MSR_RAPL_POWER_UNIT
    double time_long = pow(2.0, (double) get_bits(msrval, 17, 21)) * (1.0 + (get_bits(msrval, 22, 23) / 4.0)) * state->time_units;
    to_raplcap(limit_long, time_long, power_long);
  }
  if (limit_short != NULL) {
    // bits 46:32
    // The unit of this field is specified by the “Power Units” field of MSR_RAPL_POWER_UNIT.
    double power_short = state->power_units * (double) get_bits(msrval, 32, 46);
    // Time limit = 2^Y * (1.0 + Z/4.0) * Time_Unit
    // Here “Y” is the unsigned integer value represented. by bits 53:49, “Z” is an unsigned integer represented by
    // bits 55:54. “Time_Unit” is specified by the “Time Units” field of MSR_RAPL_POWER_UNIT. This field may have
    // a hard-coded value in hardware and ignores values written by software.
    double time_short =  pow(2.0, (double) (get_bits(msrval, 49, 53))) * (1.0 + (get_bits(msrval, 54, 55) / 4.0)) * state->time_units;
    to_raplcap(limit_short, time_short, power_short);
  }
}

static inline void get_core_uncore_dram(uint64_t msrval, const raplcap_msr_direct* state, raplcap_limit* limit_long) {
  assert(state != NULL);
  if (limit_long != NULL) {
    // bits 14:0
    // units specified by the “Power Units” field of MSR_RAPL_POWER_UNIT
    double power_long = state->power_units * (double) get_bits(msrval, 0, 14);
    // bits 21:17
    // 2^Y *F; where F is a single-digit decimal floating-point value between 1.0 and 1.3 with the fraction digit
    // represented by bits 23:22, Y is an unsigned integer represented by bits 21:17. The unit of this field is specified
    // by the “Time Units” field of MSR_RAPL_POWER_UNIT.
    double time_long =  pow(2.0, (double) get_bits(msrval, 17, 21)) * to_time_window_F(get_bits(msrval, 22, 23)) * state->time_units;
    to_raplcap(limit_long, time_long, power_long);
  }
}

int raplcap_get_limits(uint32_t socket, const raplcap* rc, raplcap_zone zone,
                       raplcap_limit* limit_long, raplcap_limit* limit_short) {
  int ret = 0;
  uint64_t msrval = 0;
  if (rc == NULL || rc->state == NULL || socket >= rc->nsockets) {
    errno = EINVAL;
    return -1;
  }
  raplcap_msr_direct* state = (raplcap_msr_direct*) rc->state;
  if (limit_long != NULL) {
    memset(limit_long, 0, sizeof(raplcap_limit));
  }
  if (limit_short != NULL) {
    memset(limit_short, 0, sizeof(raplcap_limit));
  }
  switch (zone) {
    case RAPLCAP_ZONE_PACKAGE:
      ret = read_msr(state->fds[socket], MSR_PKG_POWER_LIMIT, &msrval);
      if (!ret) {
        get_pkg_platform(msrval, state, limit_long, limit_short);
      }
      break;
    case RAPLCAP_ZONE_CORE:
      ret = read_msr(state->fds[socket], MSR_PP0_POWER_LIMIT, &msrval);
      if (!ret) {
        get_core_uncore_dram(msrval, state, limit_long);
      }
      break;
    case RAPLCAP_ZONE_UNCORE:
      ret = read_msr(state->fds[socket], MSR_PP1_POWER_LIMIT, &msrval);
      if (!ret) {
        get_core_uncore_dram(msrval, state, limit_long);
      }
      break;
    case RAPLCAP_ZONE_DRAM:
      ret = read_msr(state->fds[socket], MSR_DRAM_POWER_LIMIT, &msrval);
      if (!ret) {
        get_core_uncore_dram(msrval, state, limit_long);
      }
      break;
    case RAPLCAP_ZONE_PSYS:
      ret = read_msr(state->fds[socket], MSR_PLATFORM_POWER_LIMIT, &msrval);
      if (!ret) {
        get_pkg_platform(msrval, state, limit_long, limit_short);
      }
      break;
    default:
      errno = EINVAL;
      ret = -1;
      break;
  }
  return ret ? -1 : 0;
}

/**
 * Computes bit field based on equations in get_pkg_platform(...).
 * Needs to solve for a different value in the equation though.
 */
static inline uint64_t set_pkg_platform(uint64_t msrval, const raplcap_msr_direct* state,
                                        const raplcap_limit* limit_long, const raplcap_limit* limit_short) {
  assert(state != NULL);
  if (limit_long != NULL) {
    if (limit_long->watts > 0) {
      double power_long = (limit_long->watts / state->power_units);
      msrval = replace_bits(msrval, power_long, 0, 14);
    }
    if (limit_long->seconds > 0) {
      double time_long = log10((4.0 * limit_long->seconds) / (state->time_units * (get_bits(msrval, 22, 23) + 4.0))) / log10(2.0);
      printf("%lf\n", time_long);
      msrval = replace_bits(msrval, time_long, 17, 21);
    }
  }
  if (limit_short != NULL) {
    if (limit_short->watts > 0) {
      double power_short = (limit_short->watts / state->power_units);
      msrval = replace_bits(msrval, power_short, 32, 46);
    }
    if (limit_short->seconds > 0) {
      double time_short = log10((4.0 * limit_short->seconds) / (state->time_units * (get_bits(msrval, 54, 55) + 4.0))) / log10(2.0);
      msrval = replace_bits(msrval, time_short, 49, 53);
    }
  }
  return msrval;
}

/**
 * Computes bit field based on equations in get_core_uncore_dram(...)
 * Needs to solve for a different value in the equation though.
 */
static inline uint64_t set_core_uncore_dram(uint64_t msrval, const raplcap_msr_direct* state, const raplcap_limit* limit_long) {
  assert(state != NULL);
  if (limit_long != NULL) {
    if (limit_long->watts > 0) {
      double power_long = (limit_long->watts / state->power_units);
      msrval = replace_bits(msrval, power_long, 0, 14);
    }
    if (limit_long->seconds > 0) {
      double time_long = log10(limit_long->seconds / to_time_window_F(get_bits(msrval, 22, 23))) / log10(2.0);
      msrval = replace_bits(msrval, time_long, 17, 21);
    }
  }
  return msrval;
}

int raplcap_set_limits(uint32_t socket, const raplcap* rc, raplcap_zone zone,
                       const raplcap_limit* limit_long, const raplcap_limit* limit_short) {
  int ret = 0;
  uint64_t msrval = 0;
  if (rc == NULL || rc->state == NULL || socket >= rc->nsockets) {
    errno = EINVAL;
    return -1;
  }
  raplcap_msr_direct* state = (raplcap_msr_direct*) rc->state;
  switch (zone) {
    case RAPLCAP_ZONE_PACKAGE:
      ret = read_msr(state->fds[socket], MSR_PKG_POWER_LIMIT, &msrval);
      if (!ret) {
        msrval = set_pkg_platform(msrval, state, limit_long, limit_short);
        ret = write_msr(state->fds[socket], MSR_PKG_POWER_LIMIT, msrval);
      }
      break;
    case RAPLCAP_ZONE_CORE:
      ret = read_msr(state->fds[socket], MSR_PP0_POWER_LIMIT, &msrval);
      if (!ret) {
        msrval = set_core_uncore_dram(msrval, state, limit_long);
        ret = write_msr(state->fds[socket], MSR_PP0_POWER_LIMIT, msrval);
      }
      break;
    case RAPLCAP_ZONE_UNCORE:
      ret = read_msr(state->fds[socket], MSR_PP1_POWER_LIMIT, &msrval);
      if (!ret) {
        msrval = set_core_uncore_dram(msrval, state, limit_long);
        ret = write_msr(state->fds[socket], MSR_PP1_POWER_LIMIT, msrval);
      }
      break;
    case RAPLCAP_ZONE_DRAM:
      ret = read_msr(state->fds[socket], MSR_DRAM_POWER_LIMIT, &msrval);
      if (!ret) {
        msrval = set_core_uncore_dram(msrval, state, limit_long);
        ret = write_msr(state->fds[socket], MSR_DRAM_POWER_LIMIT, msrval);
      }
      break;
    case RAPLCAP_ZONE_PSYS:
      ret = read_msr(state->fds[socket], MSR_PLATFORM_POWER_LIMIT, &msrval);
      if (!ret) {
        msrval = set_pkg_platform(msrval, state, limit_long, limit_short);
        ret = write_msr(state->fds[socket], MSR_PLATFORM_POWER_LIMIT, msrval);
      }
      break;
    default:
      errno = EINVAL;
      ret = -1;
      break;
  }
  return ret ? -1 : 0;
}