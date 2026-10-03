#pragma once
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>
#include "freertos/semphr.h"
#include "common/FsApiConstants.h"
#define FS_DATE(y,m,d) static_cast<uint16_t>(((y)-1980)*512+(m)*32+(d))
#define FS_TIME(h,m,s) static_cast<uint16_t>((h)*2048+(m)*32+(s)/2)
struct FsDateTime {static void setCallback(void(*)(uint16_t*,uint16_t*)) {}};
namespace fsprobe {inline unsigned closes=0, destroys=0;inline bool failClose=false;}
class FsFile {
 std::shared_ptr<std::vector<uint8_t>> bytes_; size_t pos_=0; bool open_=false;
 public:
 FsFile()=default;
 explicit FsFile(std::shared_ptr<std::vector<uint8_t>> bytes):bytes_(std::move(bytes)),open_(true){}
 FsFile(FsFile&& o)noexcept:bytes_(std::move(o.bytes_)),pos_(o.pos_),open_(o.open_){o.open_=false;}
 FsFile& operator=(FsFile&& o)noexcept{bytes_=std::move(o.bytes_);pos_=o.pos_;open_=o.open_;o.open_=false;return *this;}
 FsFile(const FsFile&)=delete;
 ~FsFile(){++fsprobe::destroys; if(open_){assert(storageTestLockDepth>0);(void)close();}}
 bool close(){if(!open_)return false;assert(storageTestLockDepth>0);++fsprobe::closes;if(fsprobe::failClose)return false;open_=false;return true;}
 bool isOpen()const{return open_;}
 void flush(){assert(storageTestLockDepth>0);}
 size_t size()const{return fileSize();}
 size_t fileSize()const{return open_?bytes_->size():0;}
 size_t getName(char* out,size_t n){if(n)out[0]=0;return 0;}
 bool seekSet(uint64_t p){assert(storageTestLockDepth>0);if(!open_||p>SIZE_MAX)return false;pos_=size_t(p);return true;}
 bool seekCur(int64_t n){return n>=0?seekSet(pos_+n):(uint64_t(-n)<=pos_&&seekSet(pos_+n));}
 size_t position()const{return pos_;}
 int available()const{return open_&&pos_<bytes_->size()?int(bytes_->size()-pos_):0;}
 int read(void* out,size_t n){assert(storageTestLockDepth>0);if(!open_)return -1;size_t k=std::min(n,bytes_->size()-std::min(pos_,bytes_->size()));if(k)std::memcpy(out,bytes_->data()+pos_,k);pos_+=k;return int(k);}
 int read(){uint8_t b;return read(&b,1)==1?b:-1;}
 size_t write(const void* in,size_t n){assert(storageTestLockDepth>0);if(!open_)return 0;if(pos_+n>bytes_->size())bytes_->resize(pos_+n);if(n)std::memcpy(bytes_->data()+pos_,in,n);pos_+=n;return n;}
 size_t write(uint8_t b){return write(&b,1);}
 bool rename(const char*){return open_;}
 bool isDirectory()const{return false;}
 void rewindDirectory(){}
 FsFile openNextFile(){return {};}
};
