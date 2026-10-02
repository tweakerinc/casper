#pragma once
#include <HalStorage.h>
#include <algorithm>
#include <cstring>
#include <memory>
#include <new>
#include "../Memory/FallibleVector.h"
namespace rivulet {
// Fixed sliding input view; parser/style state survives window changes.
class HtmlInput {
 public:
  static constexpr size_t kWindow = 8192;
  const char* p = nullptr;
  const char* end = nullptr;
  HtmlInput(const char* data, size_t len) : p(data), end(data ? data+len : data), total_(len), initial_(data) {}
  HtmlInput(HalFile& file, bool (*cancel)(void*), void* ctx)
      : file_(&file), cancel_(cancel), ctx_(ctx), total_(file.size()), eof_(false) {
    if (casper_memory::allowAllocation()) buffer_.reset(new (std::nothrow) char[kWindow+1]);
    if (!buffer_) { failed_ = true; return; }
    p = end = buffer_.get(); refill();
  }
  size_t size() const { return total_; }
  size_t position() const { if(!p)return 0; return file_ ? file_->position()-static_cast<size_t>(end-p) : static_cast<size_t>(p-initial_); }
  bool ok() const { return !failed_ && !cancelled_; }
  bool eof() const { return eof_; }
  bool ready() {
    if (!ok() || !p) return false;
    if (file_ && !eof_ && end-p < static_cast<ptrdiff_t>(kWindow/2)) refill();
    return ok() && p < end;
  }
  // Bounded head search. If a fragment has no body tag, rewind once and
  // parse the fragment. This also keeps giant embedded CSS out of first ink.
  bool seekBody(size_t budget) {
    if(bodyFound_ || !p) return true;
    const size_t started=position();
    while (ready()) {
      if(position()-started>=budget) return false;
      if (*p == '<' && end-p >= 5 && (p[1]=='b'||p[1]=='B') && (p[2]=='o'||p[2]=='O') &&
          (p[3]=='d'||p[3]=='D') && (p[4]=='y'||p[4]=='Y') &&
          (end-p == 5 || p[5]=='>' || p[5]==' ' || p[5]=='\t' || p[5]=='\n' || p[5]=='\r')) {
        if(!fullTag()) return true;
        char quote=0;
        while(p<end) {
          const char c=*p++;
          if(quote) { if(c==quote)quote=0; }
          else if(c=='\''||c=='"')quote=c;
          else if(c=='>')break;
        }
        bodyFound_=true; return true;
      }
      ++p;
    }
    if (!ok()) return true;
    bodyFound_=true;
    if (!file_) { p = initial_; return true; }
    if (!file_->seek(0)) { failed_ = true; return true; }
    eof_ = false; p = end = buffer_.get(); refill(); return true;
  }
  void skipToBody() {while(!seekBody(kWindow)) {}}
  bool fullTag() {
    if (!file_ || !p || p == end || *p != '<') return true;
    for (int attempt=0; attempt<2; ++attempt) {
      char quote=0;
      for (const char* q=p+1; q<end; ++q) {
        if (quote) { if (*q==quote) quote=0; }
        else if (*q=='\'' || *q=='"') quote=*q;
        else if (*q=='>') return true;
      }
      if (eof_) return true;
      refill();
    }
    failed_ = true; return false; // no success for a truncated oversized tag
  }
 private:
  void refill() {
    if (!file_ || failed_ || cancelled_ || eof_) return;
    if (cancel_ && cancel_(ctx_)) { cancelled_ = true; return; }
    const size_t remaining = static_cast<size_t>(end-p);
    if (remaining == kWindow) return;
    std::memmove(buffer_.get(),p,remaining); p = buffer_.get(); end = p+remaining;
    const size_t left = total_ >= file_->position() ? total_-file_->position() : 0;
    const size_t need = std::min(kWindow-remaining,left);
    if (need) {
      const int got=file_->read(buffer_.get()+remaining,need);
      if (got != static_cast<int>(need)) { failed_=true; return; }
      end += need;
    }
    buffer_[end-buffer_.get()] = 0; eof_ = file_->position() == total_;
  }
  const char* initial_ = nullptr;
  bool bodyFound_=false;
  HalFile* file_ = nullptr;
  bool (*cancel_)(void*) = nullptr;
  void* ctx_ = nullptr;
  std::unique_ptr<char[]> buffer_;
  size_t total_ = 0;
  bool eof_ = true, failed_ = false, cancelled_ = false;
};
} // namespace rivulet
