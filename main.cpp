#include <cstdint>
#include <mach/mach.h>
#include <mach/thread_info.h> // thread_identifier_info_data_t, thread_info()
#include <mach/thread_policy.h> // thread_port_t, thread_policy_set()
#include <pthread.h> // pthread_mach_thread_np(), pthread_threadid_np()
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/sysctl.h>

#if defined(__x86_64__) || defined(__i386__)
  #include <cpuid.h>
#endif

#include <cassert>
#include <chrono>
#include <format>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

//
// @Note(impcuong): Abbrev-list
//  + np  := Non-portable (https://github.com/apple/darwin-libpthread/blob/main/src/pthread.c#L920)
//  + cnt := count
//  + snd := send
//  + rcv := receive
//  + ctl := control
//  + rm  := remove
//  + rc  := return-code
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

#define SYSCTL_CORE_COUNT "machdep.cpu.core_count"

int32_t estimate_core_quan()
{
  int32_t core_quan = 0;
  size_t sz = sizeof(core_quan);
  sysctlbyname(SYSCTL_CORE_COUNT, &core_quan, &sz, NULL, 0);
  return core_quan;
}

int pthread_setaffinity_np(pthread_t thread, size_t each_cpu_sz, cpu_set_t *cpu)
{
  int32_t core_quan = estimate_core_quan();
  int core_id = 0;
  for (; core_id < core_quan * each_cpu_sz; core_id++)
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
#if defined(__arm64__) || defined(__aarch64__)
  thread_identifier_info_data_t thread_id_info;
  mach_msg_type_number_t info_sz = THREAD_IDENTIFIER_INFO_COUNT;
  kern_return_t rc = thread_info(mach_thread_self() /*target_act=*/, THREAD_IDENTIFIER_INFO /*flavor=*/,
      reinterpret_cast<thread_info_t>(&thread_id_info) /*thread_info_out=*/, &info_sz /*thread_info_outCnt=*/);

  uint64_t tid;
  pthread_threadid_np(NULL /*pthread_t=*/, &tid);

  assert(tid == thread_id_info.thread_id);
  if (rc != KERN_SUCCESS)
    cpu_id = -1;
  else
    // @Docs: https://developer.apple.com/documentation/kernel/thread_identifier_info_data_t/1579032-thread_id
    cpu_id = static_cast<int>(thread_id_info.thread_id);
#else
  uint32_t cpu_info[4];
  CPU_ID(cpu_info, 1 /*leaf=*/, 0 /*subleaf=*/);
  if ((cpu_info[3] & (1 << 9)) == 0)
    cpu_id = -1;
  else
    cpu_id = static_cast<unsigned>(cpu_info[1] >> 24);
#endif

  cpu_id = std::max(cpu_id, 0);
}
// End @From

#define MSG_ALLOWED_TYPE 1
#define MSG_BUF_LEN 1024

// @Docs: https://www.man7.org/linux/man-pages/man3/msgsnd.3p.html
struct thread_shared_msg
{
  long type;
  char content[MSG_BUF_LEN];
};

int main()
{
  int32_t core_quan = estimate_core_quan();
  assert(core_quan > 0);

  // -----

  int cpu_id = -1;
  struct thread_shared_msg msg = {0};
  msg.type = MSG_ALLOWED_TYPE;
  std::string tmp_content = std::format("INFO: Daddy's dsize {}", cpu_id);
  std::strncpy(msg.content, tmp_content.c_str(), sizeof(msg.content) - 1);
  msg.content[MSG_BUF_LEN - 1] = '\0';

  key_t send_key = ftok("." /*path=*/, 'a' /*proj_id=*/); // System V IPC key.
  int msg_id = msgget(send_key, IPC_CREAT | 0666);
  assert(msg_id != -1);

  int msg_send_rc = msgsnd(msg_id, &msg, sizeof(msg.content), 0);
  assert(msg_send_rc != -1);

  // -----

  std::mutex io_mutex; // A mutex ensures orderly access to std::cout from multiple threads.
  std::vector<std::thread> thread_pool(core_quan);
  for (int tid = 0; tid < core_quan; tid++)
  {
    thread_pool[tid] = std::thread(
        [&msg_id, &cpu_id, &io_mutex, tid]
        {
          std::this_thread::sleep_for(std::chrono::milliseconds(20));

          key_t rcv_key = ftok("." /*path=*/, 'a' /*proj_id=*/);
          int rcv_msg_id = msgget(rcv_key, 0666);

          struct thread_shared_msg rcv_msg = {0};
          rcv_msg.type = MSG_ALLOWED_TYPE;
          int msg_rcv_rc = msgrcv(rcv_msg_id, &rcv_msg, sizeof(rcv_msg.content), rcv_msg.type, 0 /*msgflg=*/);
          if (msg_rcv_rc == -1)
          {
            std::lock_guard<std::mutex> lock(io_mutex);
            std::cerr << "ERROR: Thread " << tid << " failed to receive: " << strerror(errno) << std::endl;
            return;
          }

          std::atomic<int> tick = 0;
          sched_getcpu(cpu_id);
          while (true)
          {
            std::lock_guard<std::mutex> io_lock(io_mutex);
            std::cout << "INFO: Thread #" << tid << "\n";
            std::cout << "  + CPU: " << cpu_id << "\n";
            std::cout << "  + Received message: " << rcv_msg.content << "\n";
            tick++;
            if (tick == 15)
              break;
          }

          std::snprintf(rcv_msg.content, MSG_BUF_LEN, "%s + %d", rcv_msg.content, cpu_id);
          msgsnd(msg_id, &rcv_msg, sizeof(rcv_msg.content), 0);

          std::this_thread::sleep_for(std::chrono::milliseconds(900));
        }
    );

    cpu_set_t cpu; // Object representing a set of CPUs.
    CPU_ZERO(&cpu);
    CPU_SET(tid /*id=*/, &cpu);
    int affinity_thread_rc = pthread_setaffinity_np(thread_pool[tid].native_handle(), sizeof(cpu_set_t), &cpu);
    if (affinity_thread_rc != 0)
      std::cerr << "ERROR: Affinity thread's RC = " << affinity_thread_rc << "\n";
  }

  for (auto &t : thread_pool)
    if (t.joinable())
      t.join();

  // -----

  msgctl(msg_id, IPC_RMID, nullptr);

  return 0;
}
