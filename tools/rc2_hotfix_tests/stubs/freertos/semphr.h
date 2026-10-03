#pragma once
#include <cassert>
#include <mutex>
using SemaphoreHandle_t=std::recursive_mutex*;
inline std::recursive_mutex storageTestMutex;
inline thread_local int storageTestLockDepth=0;
constexpr unsigned portMAX_DELAY=0xffffffff;
inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(){return &storageTestMutex;}
inline bool xSemaphoreTakeRecursive(SemaphoreHandle_t m,unsigned){m->lock();++storageTestLockDepth;return true;}
inline bool xSemaphoreGiveRecursive(SemaphoreHandle_t m){--storageTestLockDepth;m->unlock();return true;}
