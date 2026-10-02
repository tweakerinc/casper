#pragma once
#include <StreamingJsonParser.h>
#include <cstring>
#include <string_view>
#include "../../lib/Memory/FallibleString.h"
#include "../../lib/Memory/FallibleVector.h"

namespace fontmanifest {
struct File {
  casper_memory::FallibleString name;
  size_t size = 0;
  uint32_t crc32 = 0;
};
struct Family {
  casper_memory::FallibleString name, description;
  casper_memory::FallibleVector<File> files;
  size_t totalSize = 0;
  bool installed = false, hasUpdate = false;
};
// SAX parser with bounded token storage. Only catalog records survive; no
// second, chapter-sized copy of the manifest or JSON DOM is held alongside it.
class Parser {
 public:
  Parser() : json_(callbacks(this)) {}
  void feed(const char* p, size_t n) { if (!failed_) json_.feed(p,n); }
  bool ok() const { return !failed_ && !json_.hasError() && complete_ && depth_==0 && version_==1 && !baseUrl.empty() && !families.empty(); }
  casper_memory::FallibleString baseUrl;
  casper_memory::FallibleVector<Family> families;
 private:
  enum Role { Other, Root, Families, FamilyObject, Files, FileObject };
  enum Key { Ignore, Version, BaseUrl, FamiliesKey, Name, Description, FilesKey, Size, Crc };
  Role stack_[StreamingJsonParser::MAX_NESTING]{};
  Key keys_[StreamingJsonParser::MAX_NESTING]{};
  unsigned depth_ = 0;
  bool complete_ = false, failed_ = false, sawCrc_ = false, sawSize_ = false;
  uint32_t version_ = 0;
  Family family_;
  File file_;
  StreamingJsonParser json_;
  Role role() const { return depth_ ? stack_[depth_-1] : Other; }
  Key key() const { return depth_ ? keys_[depth_-1] : Ignore; }
  void start(bool array) {
    if (complete_ || depth_ == StreamingJsonParser::MAX_NESTING) { failed_=true; return; }
    Role next=Other;
    if (!depth_ && !array) next=Root;
    else if(array && role()==Root && key()==FamiliesKey) next=Families;
    else if(!array && role()==Families) { next=FamilyObject; family_=Family{}; }
    else if(array && role()==FamilyObject && key()==FilesKey) next=Files;
    else if(!array && role()==Files) { next=FileObject; file_=File{}; sawCrc_=sawSize_=false; }
    stack_[depth_]=next; keys_[depth_]=Ignore; ++depth_;
  }
  void finish(bool array) {
    if (!depth_) { failed_=true; return; }
    Role r=role();
    if(!array && r==FileObject) {
      if(file_.name.empty() || !sawCrc_ || !sawSize_ || file_.size==0 || family_.files.size()>=128 ||
          file_.size > SIZE_MAX-family_.totalSize) failed_=true;
      else {
        for(const auto& f:family_.files) if(f.name==file_.name) failed_=true;
        family_.totalSize+=file_.size;
        if(!failed_ && !family_.files.push_back(std::move(file_))) failed_=true;
      }
    } else if(!array && r==FamilyObject) {
      if(family_.name.empty() || family_.files.empty() || families.size()>=128) failed_=true;
      else if(!families.push_back(std::move(family_))) failed_=true;
    } else if(!array && r==Root) complete_=true;
    --depth_;
  }
  void gotKey(const char* p,size_t n) {
    if(!depth_) { failed_=true;return; }
    std::string_view s(p,n); Key k=Ignore;
    if(s=="version")k=Version;else if(s=="baseUrl")k=BaseUrl;else if(s=="families")k=FamiliesKey;
    else if(s=="name")k=Name;else if(s=="description")k=Description;else if(s=="files")k=FilesKey;
    else if(s=="size")k=Size;else if(s=="crc32")k=Crc;
    keys_[depth_-1]=k;
  }
  void gotString(const char* p,size_t n) {
    casper_memory::FallibleString* dst=nullptr;size_t limit=511;
    if(role()==Root && key()==BaseUrl)dst=&baseUrl;
    else if(role()==FamilyObject && key()==Name){dst=&family_.name;limit=80;}
    else if(role()==FamilyObject && key()==Description)dst=&family_.description;
    else if(role()==FileObject && key()==Name){dst=&file_.name;limit=120;}
    if(dst && (n>limit || std::memchr(p,0,n) || !dst->assign(p,n))) failed_=true;
  }
  void gotNumber(const char* p,size_t n) {
    if(!((role()==Root && key()==Version)||(role()==FileObject && (key()==Size||key()==Crc))))return;
    uint32_t v=0;if(n==0){failed_=true;return;}
    for(size_t i=0;i<n;++i){unsigned d=static_cast<unsigned>(p[i]-'0');if(d>9||v>(UINT32_MAX-d)/10){failed_=true;return;}v=v*10+d;}
    if(key()==Version)version_=v;else if(key()==Size){file_.size=v;sawSize_=true;}else{file_.crc32=v;sawCrc_=true;}
  }
  static JsonCallbacks callbacks(Parser* self) {
    JsonCallbacks c{};c.ctx=self;
    c.onKey=[](void* x,const char*p,size_t n){static_cast<Parser*>(x)->gotKey(p,n);};
    c.onString=[](void* x,const char*p,size_t n){static_cast<Parser*>(x)->gotString(p,n);};
    c.onNumber=[](void* x,const char*p,size_t n){static_cast<Parser*>(x)->gotNumber(p,n);};
    c.onObjectStart=[](void*x){static_cast<Parser*>(x)->start(false);};
    c.onObjectEnd=[](void*x){static_cast<Parser*>(x)->finish(false);};
    c.onArrayStart=[](void*x){static_cast<Parser*>(x)->start(true);};
    c.onArrayEnd=[](void*x){static_cast<Parser*>(x)->finish(true);};
    return c;
  }
};
} // namespace fontmanifest
