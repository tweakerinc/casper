#pragma once
#include <map>
#include "Print.h"
#include "SdFat.h"
class SDCardManager {
 std::map<std::string,std::shared_ptr<std::vector<uint8_t>>> files_;
 public:
 static SDCardManager& getInstance(){static SDCardManager s;return s;}
 bool begin(){return true;} bool ready()const{return true;}
 std::vector<String> listFiles(const char*,int){return {};}
 String readFile(const char*){return {};}
 bool readFileToStream(const char*,Print&,size_t){return false;}
 size_t readFileToBuffer(const char*,char*,size_t,size_t){return 0;}
 bool writeFile(const char*,const String&){return false;}
 bool ensureDirectoryExists(const char*){return true;}
 FsFile open(const char* path,oflag_t flags=O_RDONLY){auto it=files_.find(path);if(it==files_.end()&&!(flags&O_CREAT))return {};auto& v=files_[path];if(!v)v=std::make_shared<std::vector<uint8_t>>();if(flags&O_TRUNC)v->clear();return FsFile(v);}
 bool mkdir(const char*,bool){return true;}
 bool exists(const char* p){return files_.count(p);}
 bool remove(const char* p){return files_.erase(p);}
 bool rename(const char* from,const char* to){auto it=files_.find(from);if(it==files_.end()||exists(to))return false;files_[to]=it->second;files_.erase(it);return true;}
 bool rmdir(const char*){return true;} bool removeDir(const char*){return true;}
 bool openFileForRead(const char*,const char* p,FsFile& f){f=open(p);return f.isOpen();}
 bool openFileForWrite(const char*,const char* p,FsFile& f){f=open(p,O_RDWR|O_CREAT|O_TRUNC);return f.isOpen();}
};
