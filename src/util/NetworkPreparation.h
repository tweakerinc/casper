#pragma once
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <Epub/blocks/ImageBlock.h>
#include <Epub/converters/PngToFramebufferConverter.h>
#include "SdCardFontSystem.h"
#include "activities/ActivityManager.h"
#include "activities/RenderLock.h"

// Every network entry uses the same cache handoff before WIFI_STA/TLS.
// It releases only regenerable graphics/font resources, never user progress.
inline void prepareNetworkWorkingSet(GfxRenderer& renderer) {
  activityManager.waitForRenderIdle();
  RenderLock lock;
  ImageBlock::releaseRenderCache();
  PngToFramebufferConverter::releaseWarmIfHeapTight(0xFFFFFFFFu);
  sdFontSystem.releaseForNetwork(renderer);
  if(auto* fcm=renderer.getFontCacheManager()) {
    if(!fcm->isScanning()) fcm->clearCache();
  }
}
