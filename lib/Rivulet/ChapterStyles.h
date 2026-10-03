#pragma once
// Chapter-scoped stylesheet rules. Never load every stylesheet in a book into RAM.
#include "../Epub/Epub/css/CssParser.h"
#include "../Memory/FallibleString.h"
#include "../Memory/FallibleVector.h"
#include <Esp.h>
#include <algorithm>
#include <cstring>
#include <string_view>

namespace rivulet {
class ChapterStyles {
 public:
  static constexpr size_t kMaxRules = 128, kMaxSelectors = 8192;
  static bool space(char c) {return c==' '||c=='\n'||c=='\r'||c=='\t'||c=='\f';}
  static std::string_view trim(std::string_view s) {
    while(!s.empty()&&space(s.front()))s.remove_prefix(1);
    while(!s.empty()&&space(s.back()))s.remove_suffix(1);
    return s;
  }
  static bool attr(std::string_view attrs,std::string_view name,std::string_view& value) {
    size_t i=0;
    while(i<attrs.size()){
      while(i<attrs.size()&&(space(attrs[i])||attrs[i]=='/'))++i;
      size_t a=i;while(i<attrs.size()&&!space(attrs[i])&&attrs[i]!='='&&attrs[i]!='>')++i;
      auto key=attrs.substr(a,i-a);while(i<attrs.size()&&space(attrs[i]))++i;
      if(i==attrs.size()||attrs[i]!='='){if(i<attrs.size())++i;continue;}
      ++i;while(i<attrs.size()&&space(attrs[i]))++i;if(i==attrs.size())return false;
      char q=(attrs[i]=='\''||attrs[i]=='"')?attrs[i++]:0;a=i;
      while(i<attrs.size()&&(q?attrs[i]!=q:!space(attrs[i])&&attrs[i]!='>'))++i;
      if(key==name){value=attrs.substr(a,i-a);return true;}if(q&&i<attrs.size())++i;
    }return false;
  }
  // Fixed scratch buffers, checked storage. Unsupported selectors do not become
  // broad matches; memory or I/O failure fails the preparation transaction.
  template<class Reader> bool load(Reader& f) {
    char selector[256]{}, decl[2048]{}, input[512];size_t ns=0,nd=0;
    int depth=0;bool comment=false,slash=false,star=false;char quote=0;bool escaped=false,ignored=false;
    while(true){int got=f.read(input,sizeof(input));if(got<0)return false;if(!got)break;
      for(int k=0;k<got;++k){char c=input[k];
        if(comment){if(star&&c=='/'){comment=false;star=false;}else star=c=='*';continue;}
        if(!quote&&slash){slash=false;if(c=='*'){comment=true;continue;}if(!put('/',depth,selector,ns,decl,nd))return false;}
        if(!quote&&c=='/'){slash=true;continue;}
        if(quote){if(depth&&!ignored&&!put(c,depth,selector,ns,decl,nd))return false;
          if(!escaped&&c==quote)quote=0;escaped=!escaped&&c=='\\';continue;}
        if(c=='\''||c=='"'){quote=c;escaped=false;}
        if(c=='{'){
          if(depth++==0){auto sel=trim({selector,ns});ignored=!sel.empty()&&sel.front()=='@';nd=0;}
          continue;
        }
        if(c=='}'){
          if(depth==0)return false;
          if(--depth==0){if(!ignored&&!add({selector,ns},{decl,nd}))return false;ns=nd=0;ignored=false;}
          continue;
        }
        if(ignored)continue;
        if(depth==0&&c==';'){ns=0;continue;} // @charset/@import not misread as a selector
        if(!put(c,depth,selector,ns,decl,nd))return false;
      }
      yield();
    }
    return !depth&&!quote&&!comment;
  }
  CssStyle resolve(std::string_view tag,std::string_view classes,std::string_view id={}) const {
    CssStyle result;
    // Specificity first, then source order. Class/ID names remain case-sensitive.
    for(unsigned priority: {0u,1u,10u,11u,100u,101u})for(const auto&r:rules_){
      auto sel=std::string_view(names_.data()+r.offset,r.length);
      if(r.priority==priority&&matches(sel,tag,classes,id))result.applyOver(r.style);
    }
    return result;
  }
  size_t size()const{return rules_.size();}
 private:
  struct Rule {
    CssStyle style;
    uint16_t offset = 0, length = 0;
    uint8_t priority = 0;
    Rule() noexcept : style{} {}
    Rule(const CssStyle& s, uint16_t o, uint16_t n, uint8_t p) noexcept
      : style(s), offset(o), length(n), priority(p) {}
  };
  casper_memory::FallibleVector<Rule> rules_;
  casper_memory::FallibleString names_;
  static bool put(char c,int depth,char*sel,size_t&ns,char*dec,size_t&nd){
    if(depth){if(nd==2047)return false;dec[nd++]=c;}else{if(ns==255)return false;sel[ns++]=c;}return true;
  }
  bool add(std::string_view selectors,std::string_view declarations){
    const auto style=CssParser::parseInlineStyle(declarations);if(!style.defined.anySet())return true;
    while(!selectors.empty()){
      size_t comma=selectors.find(',');auto sel=trim(selectors.substr(0,comma));
      if(comma==std::string_view::npos)selectors={};else selectors.remove_prefix(comma+1);
      if(sel.empty()||sel.find_first_of(" >+~[:\\\t\r\n")!=std::string_view::npos)continue;
      size_t p=sel.find_first_of(".#");
      if(p!=std::string_view::npos&&(p+1==sel.size()||sel.find_first_of(".#",p+1)!=std::string_view::npos))continue;
      unsigned priority=sel=="*"?0:p==std::string_view::npos?1:(sel[p]=='#'?100:10)+(p?1:0);
      if(rules_.size()>=kMaxRules||names_.size()+sel.size()>kMaxSelectors)return false;
      Rule r{style,static_cast<uint16_t>(names_.size()),static_cast<uint16_t>(sel.size()),static_cast<uint8_t>(priority)};
      if(!names_.append(sel)||!rules_.push_back(r))return false;
    }return true;
  }
  static bool matches(std::string_view sel,std::string_view tag,std::string_view classes,std::string_view id){
    if(sel=="*")return true;
    size_t p=sel.find_first_of(".#");if(p==std::string_view::npos)return sel==tag;
    if(p&&sel.substr(0,p)!=tag)return false;
    const auto key=sel.substr(p+1);if(sel[p]=='#')return key==id;
    while(!classes.empty()){classes=trim(classes);size_t n=0;while(n<classes.size()&&!space(classes[n]))++n;
      if(classes.substr(0,n)==key)return true;classes.remove_prefix(n);
    }return false;
  }
};
} // namespace rivulet
