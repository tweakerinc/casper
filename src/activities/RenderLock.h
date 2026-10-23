#pragma once

class Activity;  // forward declaration

// RAII helper to lock rendering mutex for the duration of a scope.
class RenderLock {
  bool isLocked = false;

 public:
  explicit RenderLock();
  // Same mutex as the default ctor. Activities pass *this so the lock is
  // visibly tied to the activity whose engine/renderer it protects.
  explicit RenderLock(Activity&);
  RenderLock(const RenderLock&) = delete;
  RenderLock& operator=(const RenderLock&) = delete;
  ~RenderLock();
  void unlock();
  static bool peek();
  static bool heldByCurrentTask();
};
