#pragma once
#include <HalStorage.h>
#include <SdCardFontRegistry.h>
#include <cstdio>
#include <cstring>
#include "FontInstaller.h"

// Stage a complete family beside the active one. Failed/cancelled transfers
// remove only staging; a recoverable backup survives interruption at publish.
class FontInstallTransaction {
 public:
  explicit FontInstallTransaction(const char* family) {
    if(!FontInstaller::isValidFamilyName(family) || std::strlen(family)>80)return;
    const char* root=SdCardFontRegistry::findFamilyRoot(family);
    if(!root) root=SdCardFontRegistry::defaultWriteRoot();
    if(!Storage.exists(root) && !Storage.mkdir(root))return;
    std::snprintf(live_,sizeof(live_),"%s/%s",root,family);
    std::snprintf(stage_,sizeof(stage_),"%s/.%s.download",root,family);
    std::snprintf(backup_,sizeof(backup_),"%s/.%s.previous",root,family);
    if(Storage.exists(backup_) && !Storage.exists(live_) && !Storage.rename(backup_,live_))return;
    if(Storage.exists(stage_) && !Storage.removeDir(stage_))return;
    ready_=Storage.mkdir(stage_);
  }
  ~FontInstallTransaction(){rollback();}
  FontInstallTransaction(const FontInstallTransaction&)=delete;
  FontInstallTransaction& operator=(const FontInstallTransaction&)=delete;
  bool ready() const{return ready_;}
  bool path(const char* filename,char* out,size_t n)const{
    if(!ready_||!FontInstaller::isValidCpfontFilename(filename))return false;
    int len=std::snprintf(out,n,"%s/%s",stage_,filename);
    return len>0 && static_cast<size_t>(len)<n;
  }
  void rollback(){if(ready_){Storage.removeDir(stage_);ready_=false;}}
  bool publish(){
    if(!ready_)return false;
    HalStorage::StorageLock lock;
    if(Storage.exists(backup_) && !Storage.removeDir(backup_))return false;
    const bool hadLive=Storage.exists(live_);
    if(hadLive && !Storage.rename(live_,backup_))return false;
    if(!Storage.rename(stage_,live_)){
      if(hadLive)Storage.rename(backup_,live_);
      return false;
    }
    ready_=false;
    // Do not jeopardize a successful publication if old-backup cleanup fails.
    if(hadLive)Storage.removeDir(backup_);
    return true;
  }
 private:
  char live_[176]{},stage_[192]{},backup_[192]{};
  bool ready_=false;
};
