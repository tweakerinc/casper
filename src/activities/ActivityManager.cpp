#include "ActivityManager.h"

#include <FontCacheManager.h>
#include <HalPowerManager.h>

#include <algorithm>

#include "OpdsServerStore.h"
#include "boot_sleep/BootActivity.h"
#include "boot_sleep/SleepActivity.h"
#include "browser/OpdsBookBrowserActivity.h"
#include "home/CrashActivity.h"
#include "home/FileBrowserActivity.h"
#include "home/HomeActivity.h"
#include "home/RecentBooksActivity.h"
#include "network/CrossPointWebServerActivity.h"
#include "reader/ReaderActivity.h"
#include "settings/OpdsServerListActivity.h"
#include "settings/SettingsActivity.h"
#include "util/FullScreenMessageActivity.h"

static portMUX_TYPE activityManagerSpinlock = portMUX_INITIALIZER_UNLOCKED;

void ActivityManager::begin() {
#if defined(configNUM_CORES) && configNUM_CORES > 1
  constexpr BaseType_t renderTaskCore = 1;
#else
  constexpr BaseType_t renderTaskCore = 0;
#endif
  xTaskCreatePinnedToCore(&renderTaskTrampoline, "ActivityManagerRender",
                          8192,               // Stack size
                          this,               // Parameters
                          1,                  // Priority
                          &renderTaskHandle,  // Task handle
                          renderTaskCore  // Keep long renders/cover decodes off CPU 0's idle watchdog when available
  );
  assert(renderTaskHandle != nullptr && "Failed to create render task");
}

void ActivityManager::renderTaskTrampoline(void* param) {
  auto* self = static_cast<ActivityManager*>(param);
  self->renderTaskLoop();
}

void ActivityManager::waitForRenderIdle() {
  // Main task only — never call from the render task (would deadlock).
  while (renderInProgress.load(std::memory_order_acquire)) {
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

void ActivityManager::renderTaskLoop() {
  while (true) {
    // Wait for at least one paint request. ulTaskNotifyTake(pdTRUE) clears the
    // notification value so stacked wakes coalesce into a single take.
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    // Clear deferred flag so loop() does not immediately re-notify for the same paint.
    requestedUpdate.store(false);

    // Cover the entire paint (including Home multipass after it unlocks the
    // RenderLock) so replace/pop cannot destroy the activity mid-multipass.
    renderInProgress.store(true, std::memory_order_release);

    // Acquire the lock before reading currentActivity to avoid a TOCTOU race
    // where the main task deletes the activity between the null-check and render().
    RenderLock lock;
    if (currentActivity) {
      HalPowerManager::Lock powerLock;  // Ensure we don't go into low-power mode while rendering
      currentActivity->render(std::move(lock));
    }

    renderInProgress.store(false, std::memory_order_release);

    // Notify any task blocked in requestUpdateAndWait() that the render is done.
    TaskHandle_t waiter = nullptr;
    taskENTER_CRITICAL(&activityManagerSpinlock);
    waiter = waitingTaskHandle;
    waitingTaskHandle = nullptr;
    taskEXIT_CRITICAL(&activityManagerSpinlock);
    if (waiter) {
      xTaskNotify(waiter, 1, eSetValueWithOverwrite);
    }
  }
}

void ActivityManager::loop() {
  if (currentActivity) {
    if (!currentActivity->isHomeActivity() && mappedInput.wasHomeGesture()) {
      if (currentActivity->handleHomeGesture()) {
        return;
      }
      goHome();
      return;
    }

    // Note: do not hold a lock here, the loop() method must be responsible for acquire one if needed
    currentActivity->loop();
  }

  while (pendingAction != PendingAction::None) {
    // Always wait for the render task (including Home multipass after it unlocks
    // the RenderLock). Push used to skip this so Settings opened snappily during
    // greys — but multipass still touches heap/SD/BW chunks while Settings builds
    // its list → concurrence lock abort (see crash_report: leave-reader multipass
    // then Entering Settings). Multipass aborts quickly via hasPendingActivityChange,
    // so the wait is typically sub-second after the user navigates, not a full grey pass.
    waitForRenderIdle();

    if (pendingAction == PendingAction::Pop) {
      RenderLock lock;

      if (!currentActivity) {
        // Should never happen in practice
        LOG_ERR("ACT", "Pop set but currentActivity is null; ignoring pop request");
        pendingAction = PendingAction::None;
        continue;
      }

      ActivityResult pendingResult = std::move(currentActivity->result);

      // Destroy the current activity
      exitActivity(lock);
      pendingAction = PendingAction::None;

      if (stackActivities.empty()) {
        LOG_DBG("ACT", "No more activities on stack, going home");
        lock.unlock();  // goHome may acquire its own lock
        goHome();
        continue;  // Will launch goHome immediately

      } else {
        currentActivity = std::move(stackActivities.back());
        stackActivities.pop_back();
        LOG_DBG("ACT", "Popped from activity stack, new size = %zu", stackActivities.size());
        // Handle result if necessary
        if (currentActivity->resultHandler) {
          LOG_DBG("ACT", "Handling result for popped activity");

          // Move it here to avoid the case where handler calling another startActivityForResult()
          auto handler = std::move(currentActivity->resultHandler);
          currentActivity->resultHandler = nullptr;
          lock.unlock();  // Handler may acquire its own lock
          handler(pendingResult);
        } else {
          lock.unlock();
        }

        // Panel was owned by the child — parent gets a chance to refresh state.
        if (pendingAction == PendingAction::None && currentActivity) {
          currentActivity->onResume();
        }

        // Request an update to ensure the popped activity gets re-rendered
        if (pendingAction == PendingAction::None) {
          requestUpdate();
        }

        // Handler may request another pending action, we will handle it in the next loop iteration
        continue;
      }

    } else if (pendingAction == PendingAction::PopToHome) {
      // Phase 2: restore stacked Home instead of allocating a brand-new one.
      RenderLock lock;
      pendingAction = PendingAction::None;

      if (currentActivity) {
        exitActivity(lock);
      }
      // Drop non-home activities above Home (usually stack is just Home).
      while (!stackActivities.empty() && !stackActivities.back()->isHomeActivity()) {
        stackActivities.back()->onExit();
        stackActivities.pop_back();
      }
      if (!stackActivities.empty() && stackActivities.back()->isHomeActivity()) {
        currentActivity = std::move(stackActivities.back());
        stackActivities.pop_back();
        LOG_DBG("ACT", "Restored Home from stack (phase-2 fast path)");
        lock.unlock();
        currentActivity->onResume();
        if (pendingAction == PendingAction::None) {
          requestUpdate();
        }
        continue;
      }
      // No Home on stack — fall through to a fresh replace.
      lock.unlock();
      replaceActivity(std::make_unique<HomeActivity>(renderer, mappedInput, HomeMenuItem::NONE));
      continue;

    } else if (pendingActivity) {
      // Current activity has requested a new activity to be launched
      RenderLock lock;

      if (pendingAction == PendingAction::Replace) {
        // Destroy the current activity
        exitActivity(lock);
        // Clear the stack
        while (!stackActivities.empty()) {
          stackActivities.back()->onExit();
          stackActivities.pop_back();
        }
      } else if (pendingAction == PendingAction::Swap) {
        // Destroy current only — keep Home (or other parents) under the reader.
        exitActivity(lock);
        LOG_DBG("ACT", "Swapped activity, stack size = %zu", stackActivities.size());
      } else if (pendingAction == PendingAction::Push) {
        // Move current activity to stack
        stackActivities.push_back(std::move(currentActivity));
        LOG_DBG("ACT", "Pushed to activity stack, new size = %zu", stackActivities.size());
      }
      pendingAction = PendingAction::None;
      currentActivity = std::move(pendingActivity);

      lock.unlock();  // onEnter may acquire its own lock
      currentActivity->onEnter();

      // onEnter may request another pending action, we will handle it in the next loop iteration
      continue;
    } else {
      // Unknown pending action with no activity — clear to avoid a tight loop.
      LOG_ERR("ACT", "Clearing stale pendingAction=%d", static_cast<int>(pendingAction));
      pendingAction = PendingAction::None;
    }
  }

  if (requestedUpdate.exchange(false)) {
    // One dirty flag → one wake. eSetValueWithOverwrite avoids stacking paint counts.
    if (renderTaskHandle) {
      xTaskNotify(renderTaskHandle, 1, eSetValueWithOverwrite);
    }
  }
}

void ActivityManager::exitActivity(const RenderLock& lock) {
  // Note: lock must be held by the caller
  if (currentActivity) {
    currentActivity->onExit();
    currentActivity.reset();
  }
}

void ActivityManager::replaceActivity(std::unique_ptr<Activity>&& newActivity) {
  // Note: no lock here, this is usually called by loop() and we may run into deadlock
  if (currentActivity) {
    // Defer launch if we're currently in an activity, to avoid deleting the current activity
    // leading to the "delete this" problem
    pendingActivity = std::move(newActivity);
    pendingAction = PendingAction::Replace;
  } else {
    // No current activity, safe to launch immediately
    currentActivity = std::move(newActivity);
    currentActivity->onEnter();
  }
}

void ActivityManager::swapActivity(std::unique_ptr<Activity>&& newActivity) {
  // Same deferral rules as replace, but stack is preserved when the pending action runs.
  if (currentActivity) {
    pendingActivity = std::move(newActivity);
    pendingAction = PendingAction::Swap;
  } else {
    currentActivity = std::move(newActivity);
    currentActivity->onEnter();
  }
}

void ActivityManager::goToFileTransfer() {
  replaceActivity(std::make_unique<CrossPointWebServerActivity>(renderer, mappedInput));
}

void ActivityManager::goToSettings() {
  // Keep Home under Settings so theme changes resume Home with a real multipass
  // (replace used to tear Home down; a BW-only settle left Stats covers black).
  if (currentActivity && currentActivity->isHomeActivity()) {
    pushActivity(std::make_unique<SettingsActivity>(renderer, mappedInput));
  } else {
    replaceActivity(std::make_unique<SettingsActivity>(renderer, mappedInput));
  }
}

void ActivityManager::goToFileBrowser(std::string path) {
  replaceActivity(std::make_unique<FileBrowserActivity>(renderer, mappedInput, std::move(path)));
}

void ActivityManager::goToRecentBooks() {
  replaceActivity(std::make_unique<RecentBooksActivity>(renderer, mappedInput));
}

void ActivityManager::goToBrowser() {
  const auto& servers = OPDS_STORE.getServers();
  // Skip the server picker when there's only one server configured
  if (servers.size() == 1) {
    replaceActivity(std::make_unique<OpdsBookBrowserActivity>(renderer, mappedInput, servers[0]));
  } else {
    replaceActivity(std::make_unique<OpdsServerListActivity>(renderer, mappedInput, true));
  }
}

void ActivityManager::goToReader(std::string path) {
  // Phase 2: keep Home alive under the reader so Back can resume it (no full rebuild).
  if (currentActivity && currentActivity->isHomeActivity()) {
    pushActivity(std::make_unique<ReaderActivity>(renderer, mappedInput, std::move(path)));
  } else {
    replaceActivity(std::make_unique<ReaderActivity>(renderer, mappedInput, std::move(path)));
  }
}

void ActivityManager::goToSleep(bool fromTimeout) {
  replaceActivity(std::make_unique<SleepActivity>(renderer, mappedInput, fromTimeout));
  loop();  // Important: sleep screen must be rendered immediately, the caller will go to sleep right after this returns
}

void ActivityManager::goToBoot() { replaceActivity(std::make_unique<BootActivity>(renderer, mappedInput)); }

void ActivityManager::goToFullScreenMessage(std::string message, EpdFontFamily::Style style) {
  replaceActivity(std::make_unique<FullScreenMessageActivity>(renderer, mappedInput, std::move(message), style));
}

void ActivityManager::goHome(HomeMenuItem initialMenuItem) {
  // Phase 2: if Home is already under the stack (typical Read → Back), pop to it.
  // Skips allocating a new HomeActivity and re-running full onEnter book/thumb work.
  const bool homeOnStack =
      std::any_of(stackActivities.begin(), stackActivities.end(),
                  [](const std::unique_ptr<Activity>& a) { return a && a->isHomeActivity(); });
  if (homeOnStack) {
    if (pendingActivity) {
      pendingActivity.reset();
    }
    pendingAction = PendingAction::PopToHome;
    return;
  }

  if (initialMenuItem == HomeMenuItem::NONE && currentActivity) {
    const auto& activityName = currentActivity->name;
    if (activityName == "FileBrowser") {
      initialMenuItem = HomeMenuItem::FILE_BROWSER;
    } else if (activityName == "RecentBooks") {
      initialMenuItem = HomeMenuItem::RECENTS;
    } else if (activityName == "OpdsBookBrowser") {
      initialMenuItem = HomeMenuItem::OPDS_BROWSER;
    } else if (activityName == "CrossPointWebServer") {
      initialMenuItem = HomeMenuItem::FILE_TRANSFER;
    }
    // Do not map Settings → SETTINGS_MENU: that selected the classic bottom-row
    // Settings item and felt like "Back opened the menu" after leaving Settings.
  }
  replaceActivity(std::make_unique<HomeActivity>(renderer, mappedInput, initialMenuItem));
}
void ActivityManager::goToCrashReport() { replaceActivity(std::make_unique<CrashActivity>(renderer, mappedInput)); }

void ActivityManager::pushActivity(std::unique_ptr<Activity>&& activity) {
  if (pendingActivity) {
    // Should never happen in practice
    LOG_ERR("ACT", "pendingActivity while pushActivity is not expected");
    pendingActivity.reset();
  }
  pendingActivity = std::move(activity);
  pendingAction = PendingAction::Push;
}

void ActivityManager::popActivity() {
  if (pendingActivity) {
    // Should never happen in practice
    LOG_ERR("ACT", "pendingActivity while popActivity is not expected");
    pendingActivity.reset();
  }
  pendingAction = PendingAction::Pop;
}

bool ActivityManager::preventAutoSleep() const { return currentActivity && currentActivity->preventAutoSleep(); }

bool ActivityManager::isCurrentActivity(const Activity* activity) const {
  return activity != nullptr && currentActivity.get() == activity;
}

bool ActivityManager::hasPendingActivityChange() const { return pendingAction != PendingAction::None; }

bool ActivityManager::isReaderActivity() const {
  return std::any_of(stackActivities.begin(), stackActivities.end(),
                     [](const auto& activity) { return activity->isReaderActivity(); }) ||
         (currentActivity && currentActivity->isReaderActivity());
}

bool ActivityManager::handleForcedRefresh() { return currentActivity && currentActivity->handleForcedRefresh(); }

bool ActivityManager::skipLoopDelay() const { return currentActivity && currentActivity->skipLoopDelay(); }

ScreenshotInfo ActivityManager::getScreenshotInfo() const {
  if (currentActivity) {
    return currentActivity->getScreenshotInfo();
  }
  return {};
}

void ActivityManager::requestUpdate(bool immediate) {
  // Coalesce: many requestUpdate() calls collapse to one paint.
  // Deferred path: set dirty; loop() wakes the render task once per main tick.
  // Immediate path: set dirty and wake now (overwrite, do not stack counts).
  requestedUpdate.store(true);
  if (immediate && renderTaskHandle) {
    xTaskNotify(renderTaskHandle, 1, eSetValueWithOverwrite);
  }
}
void ActivityManager::requestUpdateAndWait() {
  if (!renderTaskHandle) {
    return;
  }

  // Atomic section to perform checks
  taskENTER_CRITICAL(&activityManagerSpinlock);
  auto currTaskHandler = xTaskGetCurrentTaskHandle();
  auto mutexHolder = xSemaphoreGetMutexHolder(renderingMutex);
  bool isRenderTask = (currTaskHandler == renderTaskHandle);
  bool alreadyWaiting = (waitingTaskHandle != nullptr);
  bool holdingRenderLock = (mutexHolder == currTaskHandler);
  if (!alreadyWaiting && !isRenderTask && !holdingRenderLock) {
    waitingTaskHandle = currTaskHandler;
  }
  taskEXIT_CRITICAL(&activityManagerSpinlock);

  // Render task cannot call requestUpdateAndWait() or it will cause a deadlock
  assert(!isRenderTask && "Render task cannot call requestUpdateAndWait()");

  // There should never be the case where 2 tasks are waiting for a render at the same time
  assert(!alreadyWaiting && "Already waiting for a render to complete");

  // Cannot call while holding RenderLock or it will cause a deadlock
  assert(!holdingRenderLock && "Cannot call requestUpdateAndWait() while holding RenderLock");

  requestedUpdate.store(true);
  xTaskNotify(renderTaskHandle, 1, eSetValueWithOverwrite);
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

// RenderLock

RenderLock::RenderLock() {
  xSemaphoreTake(activityManager.renderingMutex, portMAX_DELAY);
  isLocked = true;
}

RenderLock::RenderLock([[maybe_unused]] Activity&) {
  xSemaphoreTake(activityManager.renderingMutex, portMAX_DELAY);
  isLocked = true;
}

RenderLock::~RenderLock() {
  if (isLocked) {
    xSemaphoreGive(activityManager.renderingMutex);
    isLocked = false;
  }
}

void RenderLock::unlock() {
  if (isLocked) {
    xSemaphoreGive(activityManager.renderingMutex);
    isLocked = false;
  }
}

/**
 *
 * Checks if renderingMutex is busy.
 *
 * @return true if renderingMutex is busy, otherwise false.
 *
 */
bool RenderLock::peek() { return xQueuePeek(activityManager.renderingMutex, NULL, 0) != pdTRUE; };
