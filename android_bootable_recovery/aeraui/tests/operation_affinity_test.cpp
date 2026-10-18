#include "operation_affinity.hpp"
#include <cassert>
#include <thread>
#include <cstdio>
#include <sys/wait.h>

int main() {
  using namespace aeraui::operation_affinity;
  assert(RestoreForWorker() == ENODATA);
  cpu_set_t baseline{};
  assert(sched_getaffinity(0, sizeof(baseline), &baseline) == 0);
  Remember(baseline);
  cpu_set_t narrow{};
  CPU_ZERO(&narrow);
  for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
    if (CPU_ISSET(cpu, &baseline)) { CPU_SET(cpu, &narrow); break; }
  }
  assert(sched_setaffinity(0, sizeof(narrow), &narrow) == 0);
  Remember(narrow); // A later interaction/transition must not replace baseline.
  std::thread worker([&] {
    cpu_set_t inherited{}, restored{};
    assert(sched_getaffinity(0, sizeof(inherited), &inherited) == 0);
    assert(CPU_EQUAL(&inherited, &narrow));
    assert(RestoreForWorker() == 0);
    assert(sched_getaffinity(0, sizeof(restored), &restored) == 0);
    assert(CPU_EQUAL(&restored, &baseline));
    std::thread child([&] {
      cpu_set_t mask{};
      assert(sched_getaffinity(0, sizeof(mask), &mask) == 0);
      assert(CPU_EQUAL(&mask, &baseline));
    });
    child.join();
  });
  worker.join();
  cpu_set_t ui{};
  assert(sched_getaffinity(0, sizeof(ui), &ui) == 0);
  assert(CPU_EQUAL(&ui, &narrow));
  const pid_t child = ForkWorker();
  assert(child >= 0);
  if (child == 0) {
    cpu_set_t mask{};
    const bool restored = sched_getaffinity(0, sizeof(mask), &mask) == 0 && CPU_EQUAL(&mask, &baseline);
    _exit(restored ? 0 : 1);
  }
  int status = 0;
  assert(waitpid(child, &status, 0) == child);
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  assert(sched_getaffinity(0, sizeof(ui), &ui) == 0);
  assert(CPU_EQUAL(&ui, &narrow));
  assert(sched_setaffinity(0, sizeof(baseline), &baseline) == 0);
  puts("PASS: worker and child restored; UI affinity unchanged");
}
