#pragma once
#include "ChapterStyles.h"
#include "HtmlInput.h"

namespace rivulet {
// Read only the document head. Each linked sheet is returned to the loader as
// a separate work unit; neither all of the book's CSS nor the XHTML body is
// copied into RAM. Views are valid until the next call to step().
class StylesheetHead {
 public:
  enum class Result { Working, Link, Inline, Done, Failed };
  explicit StylesheetHead(HalFile& file) : input_(file,nullptr,nullptr) {}
  std::string_view value() const {return value_;}
  Result step(size_t budget=4096) {
    if(done_)return Result::Done;
    value_={};const size_t start=input_.position();
    while(input_.ready()) {
      if(input_.position()-start>=budget)return Result::Working;
      if(comment_) {
        char c=*input_.p++;
        if(c=='>'&&dashes_>=2){comment_=false;dashes_=0;}else dashes_=c=='-'?std::min(dashes_+1,2):0;
        continue;
      }
      if(*input_.p!='<') {
        const char c=*input_.p++;
        if(inline_&&!append(c))return Result::Failed;
        continue;
      }
      // Comments may span more than the input window. Consume them as a
      // stream, not as one bounded tag (and never interpret links inside).
      if(input_.end-input_.p>=4 && std::memcmp(input_.p,"<!--",4)==0){
        input_.p+=4;comment_=true;dashes_=0;continue;
      }
      if(!input_.fullTag())return Result::Failed;
      const char* p=input_.p;const char* q=p+1;char quote=0;
      for(;q<input_.end;++q){if(quote){if(*q==quote)quote=0;}else if(*q=='\''||*q=='"')quote=*q;else if(*q=='>')break;}
      if(q==input_.end)return Result::Failed;
      std::string_view raw(p+1,q-p-1);
      if(raw.size()>=3&&raw.substr(0,3)=="!--") {input_.p+=4;comment_=true;dashes_=0;continue;}
      auto s=ChapterStyles::trim(raw);bool closing=!s.empty()&&s.front()=='/';if(closing)s.remove_prefix(1);
      size_t n=0;while(n<s.size()&&!ChapterStyles::space(s[n])&&s[n]!='/')++n;
      const auto name=s.substr(0,n),attrs=s.substr(n);
      if(inline_&&!(closing&&equal(name,"style"))){if(!append('<'))return Result::Failed;++input_.p;continue;}
      input_.p=q+1;
      if(script_){if(closing&&equal(name,"script"))script_=false;continue;}
      if(equal(name,"script")&&!closing){script_=true;continue;}
      if(equal(name,"style")) {
        if(closing&&inline_){inline_=false;value_={css_.data(),css_.size()};return Result::Inline;}
        if(!closing){css_.clear();inline_=true;}continue;
      }
      if(equal(name,"body")||(closing&&equal(name,"head"))){done_=true;return Result::Done;}
      if(!closing&&equal(name,"link")) {
        std::string_view rel,href;
        if(ChapterStyles::attr(attrs,"rel",rel)&&token(rel,"stylesheet")&&ChapterStyles::attr(attrs,"href",href)&&!href.empty()) {
          value_=href;return Result::Link;
        }
      }
      // A fragment without <head>/<body> starts at the first content element.
      if(!closing && !equal(name,"html")&&!equal(name,"head")&&!equal(name,"title")&&!equal(name,"meta")&&!equal(name,"link") &&
         !name.empty()&&name.front()!='!'&&name.front()!='?') {done_=true;return Result::Done;}
    }
    if(!input_.ok()||inline_||comment_)return Result::Failed;
    done_=true;return Result::Done;
  }
 private:
  static bool equal(std::string_view a,std::string_view b){
    if(a.size()!=b.size())return false;
    for(size_t i=0;i<a.size();++i){const char c=a[i]>='A'&&a[i]<='Z'?char(a[i]+32):a[i];if(c!=b[i])return false;}return true;
  }
  static bool token(std::string_view list,std::string_view needle){
    while(!(list=ChapterStyles::trim(list)).empty()){size_t n=0;while(n<list.size()&&!ChapterStyles::space(list[n]))++n;
      if(equal(list.substr(0,n),needle))return true;list.remove_prefix(n);}return false;
  }
  bool append(char c){return css_.size()<8192&&css_.push_back(c);}
  HtmlInput input_;
  casper_memory::FallibleString css_;
  std::string_view value_;
  bool done_=false,inline_=false,script_=false,comment_=false;int dashes_=0;
};
} // namespace rivulet
