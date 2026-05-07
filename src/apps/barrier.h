#pragma once
#include <dirent.h>
#include <unistd.h>
#include <chrono>
#include <string>
#include <thread>

// Call after machnet_connect(). Writes a ready file, then blocks until
// n_clients ready files exist in barrier_dir. No-op if barrier_dir is empty.
inline void barrier_wait(const std::string& barrier_dir, int n_clients) {
  if (barrier_dir.empty() || n_clients <= 1) return;

  std::string ready_file =
      barrier_dir + "/ready_" + std::to_string(getpid());
  FILE* f = fopen(ready_file.c_str(), "w");
  if (f) fclose(f);

  while (true) {
    int count = 0;
    DIR* dir = opendir(barrier_dir.c_str());
    if (dir) {
      struct dirent* ent;
      while ((ent = readdir(dir)) != nullptr) {
        std::string name(ent->d_name);
        if (name.rfind("ready_", 0) == 0) count++;
      }
      closedir(dir);
    }
    if (count >= n_clients) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}
