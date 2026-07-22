#include "Activity.h"

#include "ActivityBreadcrumb.h"
#include "ActivityManager.h"

#include <Esp.h>

namespace {
char s_lastActivity[48] = "boot";
}

const char* getLastActivityName() { return s_lastActivity; }

void Activity::onEnter() {
  // Breadcrumb for freezes/panics: which screen was active + heap snapshot.
  strncpy(s_lastActivity, name.c_str(), sizeof(s_lastActivity) - 1);
  s_lastActivity[sizeof(s_lastActivity) - 1] = '\0';
  LOG_INF("ACT", "enter %s free=%u maxAlloc=%u", name.c_str(), static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(ESP.getMaxAllocHeap()));
  LOG_DBG("ACT", "Entering activity: %s", name.c_str());
}

void Activity::onExit() {
  LOG_INF("ACT", "exit %s free=%u maxAlloc=%u", name.c_str(), static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(ESP.getMaxAllocHeap()));
  LOG_DBG("ACT", "Exiting activity: %s", name.c_str());
}

void Activity::requestUpdate(bool immediate) { activityManager.requestUpdate(immediate); }

RequestUpdateResult Activity::requestUpdateAndWait() { return activityManager.requestUpdateAndWait(); }

void Activity::onGoHome(HomeMenuItem item) { activityManager.goHome(item); }

void Activity::onSelectBook(const std::string& path) { activityManager.goToReader(path); }

void Activity::startActivityForResult(std::unique_ptr<Activity>&& activity, ActivityResultHandler resultHandler) {
  this->resultHandler = std::move(resultHandler);
  activityManager.pushActivity(std::move(activity));
}

void Activity::setResult(ActivityResult&& result) { this->result = std::move(result); }

void Activity::finishAfterBackPress() {
  mappedInput.suppressNextBackRelease();
  finish();
}

void Activity::finish() { activityManager.popActivity(); }
