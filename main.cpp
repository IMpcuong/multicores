#include <cpuid.h>
#include <mach/mach.h>
#include <mach/thread_policy.h> // thread_port_t, thread_policy_set()
#include <pthread.h> // pthread_mach_thread_np()
#include <sys/sysctl.h>

#include <cassert>
#include <chrono>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

//
// @Note(impcuong): Abbrev-list
//  + np  := Non-portable (https://github.com/apple/darwin-libpthread/blob/main/src/pthread.c#L920)
//  + cnt := count
//

//
// @Docs:
//  + https://www.hybridkernel.com/2015/01/18/binding_threads_to_cores_osx.html
//  + https://eli.thegreenplace.net/2016/c11-threads-affinity-and-hyperthreading/
//

typedef struct cpu_set
{
  uint32_t cnt;
} cpu_set_t;

static inline void CPU_ZERO(cpu_set_t *cpu)
{
  cpu->cnt = 0;
}

static inline void CPU_SET(int id, cpu_set_t *cpu)
{
  cpu->cnt |= (1 << id);
}

// @Docs: https://linux.die.net/man/3/cpu_isset
// Test to see if CPU cpu is a member of set.
static inline int CPU_ISSET(int id, cpu_set_t *cpu)
{
  return cpu->cnt & (1 << id);
}

int pthread_setaffinity_np(pthread_t thread, size_t cpu_sz, cpu_set_t *cpu)
{
  int core_id = 0;
  for (; core_id < 8 * cpu_sz; core_id++)
    if (CPU_ISSET(core_id, cpu))
      break;

  thread_port_t mach_thread_port = pthread_mach_thread_np(thread);
  thread_affinity_policy_data_t mach_thread_policy = { .affinity_tag = core_id };
  kern_return_t rc = thread_policy_set(mach_thread_port /*thread=*/, THREAD_AFFINITY_POLICY /*flavor=*/,
      reinterpret_cast<thread_policy_t>(&mach_thread_policy) /*policy_info=*/, 0 /*policy_infoCnt=*/);

  return rc;
}

// @From: https://stackoverflow.com/a/40398183/12535617
#define CPU_ID(info, leaf, subleaf) __cpuid_count(leaf, subleaf, info[0], info[1], info[2], info[3])

void sched_getcpu(int &cpu_id)
{
  uint32_t cpu_info[4];
  CPU_ID(cpu_info, 1 /*leaf=*/, 0 /*subleaf=*/);
  if ((cpu_info[3] & (1 << 9)) == 0)
    cpu_id = -1;
  else
    cpu_id = static_cast<unsigned>(cpu_info[1] >> 24);

  cpu_id = std::max(cpu_id, 0);
}
// End @From

#define SYSCTL_CORE_COUNT "machdep.cpu.core_count"

int main()
{
  int32_t core_quan = 0;
  size_t mem = sizeof(core_quan);
  sysctlbyname(SYSCTL_CORE_COUNT, &core_quan, &mem, NULL, 0);
  assert(core_quan > 0);

  std::mutex io_mutex; // A mutex ensures orderly access to std::cout from multiple threads.
  std::vector<std::thread> thread_pool(core_quan);
  for (int idx = 0; idx < core_quan; idx++)
  {
    int cpu_id = -1;
    sched_getcpu(cpu_id);
    thread_pool[idx] = std::thread(
        [&io_mutex, &cpu_id, idx]
        {
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
          while (true)
          {
            std::lock_guard<std::mutex> io_lock(io_mutex);
            std::cout << "INFO: Thread #" << idx << ": on CPU " << cpu_id << "\n";
          }

          std::this_thread::sleep_for(std::chrono::milliseconds(900));
        }
    );

    cpu_set_t cpu; // Object representing a set of CPUs.
    CPU_ZERO(&cpu);
    CPU_SET(idx /*id=*/, &cpu);
    int can_bind_affinity_to_thread = pthread_setaffinity_np(thread_pool[idx].native_handle(), sizeof(cpu_set_t), &cpu);
    if (can_bind_affinity_to_thread != 0)
      std::cerr << "ERROR: Cannot bind to the thread #" << cpu_id << "\n";
  }

  for (auto &t : thread_pool)
    t.join();

  return 0;
}
