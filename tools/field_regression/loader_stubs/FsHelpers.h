#pragma once
#include <string>
#include <filesystem>
namespace FsHelpers {
inline std::string normalisePath(const std::string& s){return std::filesystem::path(s).lexically_normal().generic_string();}
inline std::string decodeUriEscapes(const std::string& s){std::string out;auto hex=[](char c)->int{if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;};for(size_t i=0;i<s.size();++i){if(s[i]=='%'&&i+2<s.size()&&hex(s[i+1])>=0&&hex(s[i+2])>=0){out+=char(hex(s[i+1])*16+hex(s[i+2]));i+=2;}else out+=s[i];}return out;}
}
