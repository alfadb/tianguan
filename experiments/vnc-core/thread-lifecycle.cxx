// SPDX-License-Identifier: GPL-3.0-or-later
#include <os/Thread.h>
#include <os/Mutex.h>
#include <cstring>

struct State {
  State() : condition(&mutex), stopRequested(false), entered(false) {}
  os::Mutex mutex;
  os::Condition condition;
  bool stopRequested;
  bool entered;
};
class Worker : public os::Thread {
public:
  explicit Worker(State& state) : state(state) {}
  void stop() {
    os::AutoMutex lock(&state.mutex);
    state.stopRequested = true;
    state.condition.broadcast();
  }
protected:
  void worker() override {
    os::AutoMutex lock(&state.mutex);
    state.entered = true;
    state.condition.broadcast();
    while (!state.stopRequested)
      state.condition.wait();
  }
private:
  State& state;
};
int main(int argc, char** argv) {
  if (argc != 2) return 2;
  State state;
  Worker* thread = new Worker(state);
  thread->start();
  {
    os::AutoMutex lock(&state.mutex);
    while (!state.entered) state.condition.wait();
  }
  if (std::strcmp(argv[1], "cooperative") == 0) {
    thread->stop();
    thread->wait();
    if (thread->isRunning()) return 3;
    delete thread;
    return 0;
  }
  if (std::strcmp(argv[1], "running") == 0) {
    delete thread;
    return 4;
  }
  return 2;
}
